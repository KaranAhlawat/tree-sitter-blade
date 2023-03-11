#include <tree_sitter/parser.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>

#include "tag.h"

// Some helper macros
#define PEEK lexer->lookahead
#define S_ADVANCE lexer->advance (lexer, false)
#define S_SKIP lexer->advance (lexer, true)
#define S_MARK_END lexer->mark_end (lexer)
#define S_RESULT(s) lexer->result_symbol = s;
#define S_EOF lexer->eof (lexer)
#define SYM(s) (valid_symbols[s])

namespace
{

using std::string;
using std::vector;

enum TokenType
{
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

void
print_valid_syms (const bool *valid_symbols)
{
  auto a = (unsigned)TokenType::END;
  std::cerr << "\nVS: START\n";
  for (unsigned i = 0; i <= a; ++i)
    {
      std::cerr << SYM (i) << " ";
    }
  std::cerr << "\nVS: END\n";
}

struct Scanner
{
  Scanner () {}

  unsigned
  serialize (char *buffer)
  {
    uint16_t tag_count = tags.size () > UINT16_MAX ? UINT16_MAX : tags.size ();
    uint16_t serialized_tag_count = 0;

    unsigned i = sizeof (tag_count);
    std::memcpy (&buffer[i], &tag_count, sizeof (tag_count));
    i += sizeof (tag_count);

    for (; serialized_tag_count < tag_count; serialized_tag_count++)
      {
        Tag &tag = tags[serialized_tag_count];
        if (tag.type == CUSTOM)
          {
            unsigned name_length = tag.custom_tag_name.size ();
            if (name_length > UINT8_MAX)
              name_length = UINT8_MAX;
            if (i + 2 + name_length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE)
              break;
            buffer[i++] = static_cast<char> (tag.type);
            buffer[i++] = name_length;
            tag.custom_tag_name.copy (&buffer[i], name_length);
            i += name_length;
          }
        else
          {
            if (i + 1 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE)
              break;
            buffer[i++] = static_cast<char> (tag.type);
          }
      }

    std::memcpy (&buffer[0], &serialized_tag_count,
                 sizeof (serialized_tag_count));
    return i;
  }

  void
  deserialize (const char *buffer, unsigned length)
  {
    tags.clear ();
    if (length > 0)
      {
        unsigned i = 0;
        uint16_t tag_count, serialized_tag_count;

        std::memcpy (&serialized_tag_count, &buffer[i],
                     sizeof (serialized_tag_count));
        i += sizeof (serialized_tag_count);

        std::memcpy (&tag_count, &buffer[i], sizeof (tag_count));
        i += sizeof (tag_count);

        tags.resize (tag_count);
        for (unsigned j = 0; j < serialized_tag_count; j++)
          {
            Tag &tag = tags[j];
            tag.type = static_cast<TagType> (buffer[i++]);
            if (tag.type == CUSTOM)
              {
                uint16_t name_length = static_cast<uint8_t> (buffer[i++]);
                tag.custom_tag_name.assign (&buffer[i],
                                            &buffer[i + name_length]);
                i += name_length;
              }
          }
      }
  }

  string
  scan_tag_name (TSLexer *lexer)
  {
    string tag_name;
    while (iswalnum (PEEK) || PEEK == '-' || PEEK == ':')
      {
        tag_name += towupper (PEEK);
        S_ADVANCE;
      }
    return tag_name;
  }

  bool
  scan_comment (TSLexer *lexer)
  {
    if (PEEK != '-')
      return false;
    S_ADVANCE;
    if (PEEK != '-')
      return false;
    S_ADVANCE;

    unsigned dashes = 0;
    while (PEEK)
      {
        switch (PEEK)
          {
          case '-':
            ++dashes;
            break;
          case '>':
            if (dashes >= 2)
              {
                S_RESULT (COMMENT);
                S_ADVANCE;
                S_MARK_END;
                return true;
              }
          default:
            dashes = 0;
          }
        S_ADVANCE;
      }
    return false;
  }

  bool
  scan_raw_text (TSLexer *lexer)
  {
    if (!tags.size ())
      return false;

    S_MARK_END;

    const string &end_delimiter
        = tags.back ().type == SCRIPT ? "</SCRIPT" : "</STYLE";

    unsigned delimiter_index = 0;
    while (PEEK)
      {
        if (towupper (PEEK) == end_delimiter[delimiter_index])
          {
            delimiter_index++;
            if (delimiter_index == end_delimiter.size ())
              break;
            S_ADVANCE;
          }
        else
          {
            delimiter_index = 0;
            S_ADVANCE;
            S_MARK_END;
          }
      }

    S_RESULT (RAW_TEXT);
    return true;
  }

  bool
  scan_raw_php (TSLexer *lexer)
  {
    S_MARK_END;

    while (PEEK)
      {
        if (towupper (PEEK) == '}')
          {
            S_ADVANCE;
            if (towupper (PEEK) == '}')
              {
                S_ADVANCE;
                break;
              }
          }
        else if (towupper (PEEK) == '!')
          {
            S_ADVANCE;
            if (towupper (PEEK) == '!')
              {
                S_ADVANCE;
                if (towupper (PEEK) == '}')
                  {
                    S_ADVANCE;
                    break;
                  }
              }
          }
        else
          {
            S_ADVANCE;
            S_MARK_END;
          }
      }

    S_RESULT (RAW_ECHO_PHP);
    return true;
  }

  bool
  scan_implicit_end_tag (TSLexer *lexer)
  {
    Tag *parent = tags.empty () ? NULL : &tags.back ();

    bool is_closing_tag = false;
    if (PEEK == '/')
      {
        is_closing_tag = true;
        S_ADVANCE;
      }
    else
      {
        if (parent && parent->is_void ())
          {
            tags.pop_back ();
            S_RESULT (IMPLICIT_END_TAG);
            return true;
          }
      }

    string tag_name = scan_tag_name (lexer);
    if (tag_name.empty ())
      return false;

    Tag next_tag = Tag::for_name (tag_name);

    if (is_closing_tag)
      {
        // The tag correctly closes the topmost element on the stack
        if (!tags.empty () && tags.back () == next_tag)
          return false;

        // Otherwise, dig deeper and queue implicit end tags (to be nice in
        // the case of malformed HTML)
        if (std::find (tags.begin (), tags.end (), next_tag) != tags.end ())
          {
            tags.pop_back ();
            S_RESULT (IMPLICIT_END_TAG);
            return true;
          }
      }
    else if (parent && !parent->can_contain (next_tag))
      {
        tags.pop_back ();
        S_RESULT (IMPLICIT_END_TAG);
        return true;
      }

    return false;
  }

  bool
  scan_start_tag_name (TSLexer *lexer)
  {
    string tag_name = scan_tag_name (lexer);
    if (tag_name.empty ())
      return false;
    Tag tag = Tag::for_name (tag_name);
    tags.push_back (tag);
    switch (tag.type)
      {
      case SCRIPT:
        S_RESULT (SCRIPT_START_TAG_NAME);
        break;
      case STYLE:
        S_RESULT (STYLE_START_TAG_NAME);
        break;
      default:
        S_RESULT (START_TAG_NAME);
        break;
      }
    return true;
  }

  bool
  scan_end_tag_name (TSLexer *lexer)
  {
    string tag_name = scan_tag_name (lexer);
    if (tag_name.empty ())
      return false;
    Tag tag = Tag::for_name (tag_name);
    if (!tags.empty () && tags.back () == tag)
      {
        tags.pop_back ();
        S_RESULT (END_TAG_NAME);
      }
    else
      {
        S_RESULT (ERRONEOUS_END_TAG_NAME);
      }
    return true;
  }

  bool
  scan_self_closing_tag_delimiter (TSLexer *lexer)
  {
    S_ADVANCE;
    if (PEEK == '>')
      {
        S_ADVANCE;
        if (!tags.empty ())
          {
            tags.pop_back ();
            S_RESULT (SELF_CLOSING_TAG_DELIMITER);
          }
        return true;
      }
    return false;
  }

  bool
  check_open_echo_delimiter (TSLexer *lexer, bool is_verabtim)
  {
    // {{, @{{, {!!
    if (!PEEK)
      return false;

    if (is_verabtim)
      {
        if (PEEK != '@')
          return false;
        S_ADVANCE;
        if (PEEK != '{')
          return false;
        S_ADVANCE;
        if (PEEK != '{')
          return false;
        S_ADVANCE;
      }
    else
      {
        if (PEEK != '{')
          return false;
        S_ADVANCE;
        if (PEEK != '{' && PEEK != '!')
          return false;
        if (PEEK == '{')
          {
            S_ADVANCE;
            return true;
          }
        S_ADVANCE;
        if (PEEK != '!')
          return false;
        S_ADVANCE;
      }

    return true;
  }

  bool
  scan_open_echo_delimiter (TSLexer *lexer, bool is_verbatim)
  {
    if (!check_open_echo_delimiter (lexer, is_verbatim))
      return false;
    if (is_verbatim)
      S_RESULT (ECHO_TAG_VERBATIM_DELIMITER)
    else
      S_RESULT (ECHO_TAG_REGULAR_DELIMITER);
    return true;
  }

  bool
  scan_text (TSLexer *lexer)
  {
    // check if we have anything valid to scan
    if (!PEEK)
      return false;
    S_MARK_END;

    // [^<>\s]
    if (PEEK == '<' || PEEK == '>' || PEEK == ' ')
      {
        return false;
      }

    if (PEEK == '{')
      {
        S_MARK_END;
        if (check_open_echo_delimiter (lexer, false))
          {
            return false;
          }
      }

    if (PEEK == '@')
      {
        S_MARK_END;
        if (check_open_echo_delimiter (lexer, true))
          {
            return false;
          }
      }

    S_ADVANCE;

    // Takes care of the optional
    if (!PEEK)
      {
        S_MARK_END;
        S_RESULT (TEXT);
        return true;
      }

    // [^<>]*
    while (PEEK)
      {
        if (PEEK == '<' || PEEK == '>')
          {
            break;
          }

        if (PEEK == '{')
          {
            S_MARK_END;
            if (check_open_echo_delimiter (lexer, false))
              {
                S_RESULT (TEXT);
                return true;
              }
          }

        if (PEEK == '@')
          {
            S_MARK_END;
            if (check_open_echo_delimiter (lexer, true))
              {
                S_RESULT (TEXT);
                return true;
              }
          }
        S_ADVANCE;
      }

    if (!PEEK)
      {
        S_MARK_END;
        S_RESULT (TEXT);
        return true;
      }

    if (PEEK)
      {
        if (PEEK == '<' || PEEK == '>' || PEEK == ' ')
          {
            S_MARK_END;
            S_RESULT (TEXT);
            return true;
          }

        if (PEEK == '{')
          {
            S_MARK_END;
            if (check_open_echo_delimiter (lexer, false))
              {
                S_RESULT (TEXT);
                return true;
              }
          }

        if (PEEK == '@')
          {
            S_MARK_END;
            if (check_open_echo_delimiter (lexer, true))
              {
                S_RESULT (TEXT);
                return true;
              }
          }

        S_ADVANCE;
      }

    S_MARK_END;
    S_RESULT (TEXT);
    return true;
  }

  bool
  scan (TSLexer *lexer, const bool *valid_symbols)
  {
    while (iswspace (PEEK))
      {
        S_SKIP;
      }

    if (SYM (RAW_TEXT) && !SYM (START_TAG_NAME) && !SYM (END_TAG_NAME))
      {
        return scan_raw_text (lexer);
      }

    if (SYM (RAW_ECHO_PHP))
      {
        return scan_raw_php (lexer);
      }

    switch (PEEK)
      {
      case '<':
        S_MARK_END;
        S_ADVANCE;

        if (PEEK == '!')
          {
            S_ADVANCE;
            return scan_comment (lexer);
          }

        if (SYM (IMPLICIT_END_TAG))
          {
            return scan_implicit_end_tag (lexer);
          }
        break;

      case '\0':
        if (SYM (IMPLICIT_END_TAG))
          {
            return scan_implicit_end_tag (lexer);
          }
        break;

      case '/':
        if (SYM (SELF_CLOSING_TAG_DELIMITER))
          {
            return scan_self_closing_tag_delimiter (lexer);
          }
        break;

      case '{':
      case '@':
        if (SYM (ECHO_TAG_REGULAR_DELIMITER))
          {
            return scan_open_echo_delimiter (lexer, false);
          }
        if (SYM (ECHO_TAG_VERBATIM_DELIMITER))
          {
            return scan_open_echo_delimiter (lexer, true);
          }
        break;

      default:
        if (SYM (TEXT))
          {
            return scan_text (lexer);
          }

        if (SYM (START_TAG_NAME) || SYM (END_TAG_NAME) && !SYM (RAW_TEXT))
          {
            return SYM (START_TAG_NAME) ? scan_start_tag_name (lexer)
                                        : scan_end_tag_name (lexer);
          }
      }

    return false;
  }

  vector<Tag> tags;
};

} // namespace

extern "C"
{

  void *
  tree_sitter_blade_external_scanner_create ()
  {
    return new Scanner ();
  }

  bool
  tree_sitter_blade_external_scanner_scan (void *payload, TSLexer *lexer,
                                           const bool *valid_symbols)
  {
    Scanner *scanner = static_cast<Scanner *> (payload);
    return scanner->scan (lexer, valid_symbols);
  }

  unsigned
  tree_sitter_blade_external_scanner_serialize (void *payload, char *buffer)
  {
    Scanner *scanner = static_cast<Scanner *> (payload);
    return scanner->serialize (buffer);
  }

  void
  tree_sitter_blade_external_scanner_deserialize (void *payload,
                                                  const char *buffer,
                                                  unsigned length)
  {
    Scanner *scanner = static_cast<Scanner *> (payload);
    scanner->deserialize (buffer, length);
  }

  void
  tree_sitter_blade_external_scanner_destroy (void *payload)
  {
    Scanner *scanner = static_cast<Scanner *> (payload);
    delete scanner;
  }
}
