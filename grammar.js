/// <reference types="tree-sitter-cli/dsl" />
// @ts-check
const html = require("./tree-sitter-html/grammar");

// [NOTE] Verbatim missing for now
const echo_opening_tags = [
  "{{", // regular
  "{!!", // raw
  "@{{", //verabtim
]

const echo_closing_tags = [
  "}}", // regular
  "!!}", // raw
]

module.exports = grammar(html, {
  name: "blade",
  externals: ($, original) => [
    ...original,
    $.raw_echo_php
  ],
  rules: {
    _node: ($, original) => choice(
      $.echo_statement,
      original
    ),

    echo_statement: $ => seq(
      alias($.echo_start_tag, $.start_tag),
      optional($.raw_echo_php),
      alias($.echo_end_tag, $.end_tag)
    ),

    echo_start_tag: $ => choice(...echo_opening_tags.map(tag => token(prec(1, tag)))),

    echo_end_tag: $ => choice(...echo_closing_tags)
  }
});
