#include <tree_sitter/parser.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cwctype>
#include <cstring>
#include <iostream>
#include "tag.h"

// Some helper macros
#define PEEK lexer->lookahead
#define S_ADVANCE lexer->advance(lexer, false)
#define S_SKIP lexer->advance(lexer, true)
#define S_MARK_END lexer->mark_end(lexer)
#define S_RESULT(s) lexer->result_symbol = s;
#define S_EOF lexer->eof(lexer)
#define SYM(s) (valid_symbols[s])


namespace {

using std::vector;
using std::string;

enum TokenType {
  START_TAG_NAME,
  SCRIPT_START_TAG_NAME,
  STYLE_START_TAG_NAME,
  END_TAG_NAME,
  ERRONEOUS_END_TAG_NAME,
  SELF_CLOSING_TAG_DELIMITER,
  IMPLICIT_END_TAG,
  RAW_TEXT,
  COMMENT,
  TEXT,
  RAW_ECHO_PHP,
  ECHO_TAG_REGULAR_DELIMITER,
  ECHO_TAG_VERBATIM_DELIMITER,
  END
};

void print_valid_syms(const bool *valid_symbols) {
    auto a = (unsigned)TokenType::END;
    std::cerr << "\nVS: START\n";
    for (unsigned i = 0; i <= a; ++i) {
      std::cerr << valid_symbols[i] << " ";
    }
    std::cerr << "\nVS: END\n";
}

struct Scanner {
  Scanner() {}

  unsigned serialize(char *buffer) {
    uint16_t tag_count = tags.size() > UINT16_MAX ? UINT16_MAX : tags.size();
    uint16_t serialized_tag_count = 0;

    unsigned i = sizeof(tag_count);
    std::memcpy(&buffer[i], &tag_count, sizeof(tag_count));
    i += sizeof(tag_count);

    for (; serialized_tag_count < tag_count; serialized_tag_count++) {
      Tag &tag = tags[serialized_tag_count];
      if (tag.type == CUSTOM) {
        unsigned name_length = tag.custom_tag_name.size();
        if (name_length > UINT8_MAX) name_length = UINT8_MAX;
        if (i + 2 + name_length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
        buffer[i++] = static_cast<char>(tag.type);
        buffer[i++] = name_length;
        tag.custom_tag_name.copy(&buffer[i], name_length);
        i += name_length;
      } else {
        if (i + 1 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
        buffer[i++] = static_cast<char>(tag.type);
      }
    }

    std::memcpy(&buffer[0], &serialized_tag_count, sizeof(serialized_tag_count));
    return i;
  }

  void deserialize(const char *buffer, unsigned length) {
    tags.clear();
    if (length > 0) {
      unsigned i = 0;
      uint16_t tag_count, serialized_tag_count;

      std::memcpy(&serialized_tag_count, &buffer[i], sizeof(serialized_tag_count));
      i += sizeof(serialized_tag_count);

      std::memcpy(&tag_count, &buffer[i], sizeof(tag_count));
      i += sizeof(tag_count);

      tags.resize(tag_count);
      for (unsigned j = 0; j < serialized_tag_count; j++) {
        Tag &tag = tags[j];
        tag.type = static_cast<TagType>(buffer[i++]);
        if (tag.type == CUSTOM) {
          uint16_t name_length = static_cast<uint8_t>(buffer[i++]);
          tag.custom_tag_name.assign(&buffer[i], &buffer[i + name_length]);
          i += name_length;
        }
      }
    }
  }

  string scan_tag_name(TSLexer *lexer) {
    string tag_name;
    while (iswalnum(lexer->lookahead) ||
           lexer->lookahead == '-' ||
           lexer->lookahead == ':') {
      tag_name += towupper(lexer->lookahead);
      lexer->advance(lexer, false);
    }
    return tag_name;
  }

  bool scan_comment(TSLexer *lexer) {
    if (lexer->lookahead != '-') return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != '-') return false;
    lexer->advance(lexer, false);

    unsigned dashes = 0;
    while (lexer->lookahead) {
      switch (lexer->lookahead) {
        case '-':
          ++dashes;
          break;
        case '>':
          if (dashes >= 2) {
            lexer->result_symbol = COMMENT;
            lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            return true;
          }
        default:
          dashes = 0;
      }
      lexer->advance(lexer, false);
    }
    return false;
  }

  bool scan_raw_text(TSLexer *lexer) {
    if (!tags.size()) return false;

    lexer->mark_end(lexer);

    const string &end_delimiter = tags.back().type == SCRIPT
      ? "</SCRIPT"
      : "</STYLE";

    unsigned delimiter_index = 0;
    while (lexer->lookahead) {
      if (towupper(lexer->lookahead) == end_delimiter[delimiter_index]) {
        delimiter_index++;
        if (delimiter_index == end_delimiter.size()) break;
        lexer->advance(lexer, false);
      } else {
        delimiter_index = 0;
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
      }
    }

    lexer->result_symbol = RAW_TEXT;
    return true;
  }

  bool scan_raw_php(TSLexer *lexer) {
    S_MARK_END;

    while (PEEK) {
      if (towupper(PEEK) == '}') {
        S_ADVANCE;
        if (towupper(PEEK) == '}') {
          S_ADVANCE;
          break;
        }
      } else if (towupper(PEEK) == '!') {
        S_ADVANCE;
        if (towupper(PEEK) == '!') {
          S_ADVANCE;
          if (towupper(PEEK) == '}') {
            S_ADVANCE;
            break;
          }
        }
      } else {
        S_ADVANCE;
        S_MARK_END;
      }
    }

    S_RESULT(RAW_ECHO_PHP);
    return true;
  }

  bool scan_implicit_end_tag(TSLexer *lexer) {
    Tag *parent = tags.empty() ? NULL : &tags.back();

    bool is_closing_tag = false;
    if (lexer->lookahead == '/') {
      is_closing_tag = true;
      lexer->advance(lexer, false);
    } else {
      if (parent && parent->is_void()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    }

    string tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;

    Tag next_tag = Tag::for_name(tag_name);

    if (is_closing_tag) {
      // The tag correctly closes the topmost element on the stack
      if (!tags.empty() && tags.back() == next_tag) return false;

      // Otherwise, dig deeper and queue implicit end tags (to be nice in
      // the case of malformed HTML)
      if (std::find(tags.begin(), tags.end(), next_tag) != tags.end()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    } else if (parent && !parent->can_contain(next_tag)) {
      tags.pop_back();
      lexer->result_symbol = IMPLICIT_END_TAG;
      return true;
    }

    return false;
  }

  bool scan_start_tag_name(TSLexer *lexer) {
    string tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;
    Tag tag = Tag::for_name(tag_name);
    tags.push_back(tag);
    switch (tag.type) {
      case SCRIPT:
        lexer->result_symbol = SCRIPT_START_TAG_NAME;
        break;
      case STYLE:
        lexer->result_symbol = STYLE_START_TAG_NAME;
        break;
      default:
        lexer->result_symbol = START_TAG_NAME;
        break;
    }
    return true;
  }

  bool scan_end_tag_name(TSLexer *lexer) {
    string tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;
    Tag tag = Tag::for_name(tag_name);
    if (!tags.empty() && tags.back() == tag) {
      tags.pop_back();
      lexer->result_symbol = END_TAG_NAME;
    } else {
      lexer->result_symbol = ERRONEOUS_END_TAG_NAME;
    }
    return true;
  }

  bool scan_self_closing_tag_delimiter(TSLexer *lexer) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '>') {
      lexer->advance(lexer, false);
      if (!tags.empty()) {
        tags.pop_back();
        lexer->result_symbol = SELF_CLOSING_TAG_DELIMITER;
      }
      return true;
    }
    return false;
  }

  bool check_open_echo_delimiter(TSLexer *lexer, bool is_verabtim) {
    // {{, @{{, {!!
    if (S_EOF || !PEEK) return false;
    if (is_verabtim) {
      if (PEEK != '@') return false;
      S_ADVANCE;
      if (PEEK != '{') return false;
      S_ADVANCE;
      if (PEEK != '{') return false;
      S_ADVANCE;
    } else {
      if (PEEK != '{') return false;
      S_ADVANCE;
      if (PEEK != '{' && PEEK != '!') return false;
      if (PEEK == '{') {
        S_ADVANCE;
        return true;
      }
      S_ADVANCE;
      if (PEEK != '!') return false;
      S_ADVANCE;
    }

    return true;
  }

  bool scan_open_echo_delimiter(TSLexer *lexer, bool is_verbatim) {
    if (!check_open_echo_delimiter(lexer, is_verbatim)) return false;
    if (is_verbatim) S_RESULT(ECHO_TAG_VERBATIM_DELIMITER)
    else S_RESULT(ECHO_TAG_REGULAR_DELIMITER);
    return true;
  }

  bool scan_text(TSLexer *lexer) {
    // check if we have anything valid to scan
    if (S_EOF || !PEEK) return false;
    S_MARK_END;

    // [^<>\s]
    if (PEEK == '<' || PEEK == '>'
     || PEEK == ' ') {
      return false;
    }

    if (PEEK == '{') {
      S_MARK_END;
      if (check_open_echo_delimiter(lexer, false)) {
        return false;
      }
    }

    if (PEEK == '@') {
      S_MARK_END;
      if (check_open_echo_delimiter(lexer, true)) {
        return false;
      }
    }

    S_ADVANCE;

    // Takes care of the optional
    if (S_EOF || !PEEK) {
      S_MARK_END;
      S_RESULT(TEXT);
      return true;
    }

    // [^<>]*
    while (!S_EOF && PEEK) {
      if (PEEK == '<'
       || PEEK == '>') {
        break;
       }

      if (PEEK == '{') {
        S_MARK_END;
        if (check_open_echo_delimiter(lexer, false)) {
          S_RESULT(TEXT);
          return true;
        }
      }

      if (PEEK == '@') {
        S_MARK_END;
        if (check_open_echo_delimiter(lexer, true)) {
          S_RESULT(TEXT);
          return true;
        }
      }
      S_ADVANCE;
    }

    if (S_EOF || !PEEK) {
      S_MARK_END;
      S_RESULT(TEXT);
      return true;
    }

    if (!S_EOF && PEEK) {
      if (PEEK == '<'
       || PEEK == '>'
       || PEEK == ' ') {
        S_MARK_END;
        S_RESULT(TEXT);
        return true;
       }

      if (PEEK == '{') {
        S_MARK_END;
        if (check_open_echo_delimiter(lexer, false)) {
          S_RESULT(TEXT);
          return true;
        }
      }

      if (PEEK == '@') {
        S_MARK_END;
        if (check_open_echo_delimiter(lexer, true)) {
          S_RESULT(TEXT);
          return true;
        }
      }

      S_ADVANCE;
    }

    S_MARK_END;
    S_RESULT(TEXT);
    return true;
  }

  bool scan(TSLexer *lexer, const bool *valid_symbols) {
    while (iswspace(lexer->lookahead)) {
      lexer->advance(lexer, true);
    }

    if (valid_symbols[RAW_TEXT] && !valid_symbols[START_TAG_NAME] && !valid_symbols[END_TAG_NAME]) {
      return scan_raw_text(lexer);
    }

    if (SYM(RAW_ECHO_PHP)) {
      return scan_raw_php(lexer);
    }

    switch (lexer->lookahead) {
      case '<':
        lexer->mark_end(lexer);
        lexer->advance(lexer, false);

        if (lexer->lookahead == '!') {
          lexer->advance(lexer, false);
          return scan_comment(lexer);
        }

        if (valid_symbols[IMPLICIT_END_TAG]) {
          return scan_implicit_end_tag(lexer);
        }
        break;

      case '\0':
        if (valid_symbols[IMPLICIT_END_TAG]) {
          return scan_implicit_end_tag(lexer);
        }
        break;

      case '/':
        if (valid_symbols[SELF_CLOSING_TAG_DELIMITER]) {
          return scan_self_closing_tag_delimiter(lexer);
        }
        break;

      case '{':
      case '@':
        if (SYM(ECHO_TAG_REGULAR_DELIMITER)) {
          return scan_open_echo_delimiter(lexer, false);
        }
        if (SYM(ECHO_TAG_VERBATIM_DELIMITER)) {
          return scan_open_echo_delimiter(lexer, true);
        }
        break;

      default:
        if (valid_symbols[TEXT]) {
          return scan_text(lexer);
        }

        if ((valid_symbols[START_TAG_NAME] || valid_symbols[END_TAG_NAME]) && !valid_symbols[RAW_TEXT]) {
          return valid_symbols[START_TAG_NAME]
            ? scan_start_tag_name(lexer)
            : scan_end_tag_name(lexer);
        }
    }

    return false;
  }

  vector<Tag> tags;
};

}

extern "C" {

void *tree_sitter_blade_external_scanner_create() {
  return new Scanner();
}

bool tree_sitter_blade_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

unsigned tree_sitter_blade_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->serialize(buffer);
}

void tree_sitter_blade_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  scanner->deserialize(buffer, length);
}

void tree_sitter_blade_external_scanner_destroy(void *payload) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  delete scanner;
}

}
