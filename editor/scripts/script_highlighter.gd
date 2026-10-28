extends SyntaxHighlighter
## JetScript colours as Godot's GDScript editor shows them, token by token: keywords, types, annotations (@export),
## node paths ($Player/Ship), strings, numbers, comments, function names and members after a dot.

const TEXT := Color("cdcfd2")
const COMMENT := Color("cdcfd2", 0.5)
const STRING := Color("ffeda1")
const NUMBER := Color("a1ffe0")
const SYMBOL := Color("abc9ff")
const FUNCTION := Color("57b3ff")
const MEMBER := Color("bce0ff")
const ANNOTATION := Color("ffb373")
const NODE_PATH := Color("63c259")
const KEYWORD := Color("ff7085")
const CONTROL_FLOW := Color("ff8ccc")
const BASE_TYPE := Color("42ffc2")
const ENGINE_TYPE := Color("8fffdb")

var words := {} # word -> colour (keywords and type names)

func _init(keywords: Array, control_flow: Array, base_types: Array, engine_types: Array) -> void:
	for w in keywords: words[w] = KEYWORD
	for w in control_flow: words[w] = CONTROL_FLOW
	for w in engine_types: words[w] = ENGINE_TYPE
	for w in base_types: words[w] = BASE_TYPE

static func _starts_word(c: String) -> bool:
	return c == "_" or (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or c.unicode_at(0) > 127

static func _in_word(c: String) -> bool:
	return _starts_word(c) or (c >= "0" and c <= "9")

static func _digit(c: String) -> bool:
	return c >= "0" and c <= "9"

func _get_line_syntax_highlighting(line_number: int) -> Dictionary:
	var text := get_text_edit().get_line(line_number)
	var colors := {}
	var n := text.length()
	var i := 0
	var after_dot := false
	while i < n:
		var c := text[i]
		var start := i
		if c == " " or c == "\t":
			i += 1
			continue
		if c == "#":
			colors[start] = {"color": COMMENT}
			break
		if c == "\"" or c == "'":
			i = _skip_string(text, i)
			colors[start] = {"color": STRING}
			after_dot = false
			continue
		if c == "@" and i + 1 < n and _starts_word(text[i + 1]):
			i += 1
			while i < n and _in_word(text[i]): i += 1
			colors[start] = {"color": ANNOTATION}
			continue
		if c == "$":
			i += 1
			if i < n and (text[i] == "\"" or text[i] == "'"):
				i = _skip_string(text, i)
			else:
				while i < n and (_in_word(text[i]) or text[i] == "/"): i += 1
			colors[start] = {"color": NODE_PATH}
			after_dot = false
			continue
		if _digit(c) or (c == "." and i + 1 < n and _digit(text[i + 1]) and not after_dot):
			i += 1
			while i < n and (_in_word(text[i]) or text[i] == "." or ((text[i] == "-" or text[i] == "+") and text[i - 1] in ["e", "E"])):
				i += 1
			colors[start] = {"color": NUMBER}
			after_dot = false
			continue
		if _starts_word(c):
			while i < n and _in_word(text[i]): i += 1
			var word := text.substr(start, i - start)
			var next := i
			while next < n and (text[next] == " " or text[next] == "\t"): next += 1
			var call := next < n and text[next] == "("
			var color := TEXT
			if after_dot: color = FUNCTION if call else MEMBER
			elif words.has(word): color = words[word]
			elif call: color = FUNCTION
			colors[start] = {"color": color}
			after_dot = false
			continue
		colors[start] = {"color": SYMBOL}
		after_dot = c == "."
		i += 1
	return colors

## Index just past the closing quote (escapes skipped), or the end of the line.
func _skip_string(text: String, i: int) -> int:
	var quote := text[i]
	i += 1
	while i < text.length() and text[i] != quote:
		if text[i] == "\\": i += 1
		i += 1
	return mini(i + 1, text.length())
