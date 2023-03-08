const html = require("./tree-sitter-html/grammar");

// [NOTE] Verbatim missing for now
const echo_opening_tags = [
	"{{{",
	"{{",
	"{!!",
	"{{--"
]

const echo_closing_tags = [
	"}}}",
	"}}",
	"!}}",
	"--}}"
]

module.exports = grammar(html, {
	name: "blade",
	rules: {}
});
