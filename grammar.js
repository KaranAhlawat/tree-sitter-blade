/// <reference types="tree-sitter-cli/dsl" />
// @ts-check
const html = require("./tree-sitter-html/grammar");

// [NOTE] Verbatim missing for now
const echo_opening_tags = [
  "{{", // regular
  "{!!", // raw
  "@{{" // escaped
]

const echo_closing_tags = [
  "}}", // regular
  "!!}", // raw
]

module.exports = grammar(html, {
  name: "blade",
  externals: ($, original) => [
    ...original,
    ...echo_closing_tags  
  ],
  rules: {
    _node: ($, original) => choice(
      $.echo_statement,
      original
    ),

    echo_statement: $ => seq(
      alias($.echo_start_tag, $.start_tag),
      optional($.raw_text),
      alias($.echo_end_tag, $.end_tag)
    ),

    echo_start_tag: $ => token(prec(1,
      choice(
        seq('@', token.immediate(choice(...echo_opening_tags))),
        choice(...echo_opening_tags)
      )
    )),

    echo_end_tag: $ => choice(...echo_closing_tags)
  }
});
