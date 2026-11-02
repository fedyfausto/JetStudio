extends SyntaxHighlighter
# JetScript colours as Godot's GDScript editor shows them, token by token: keywords, types, annotations (@export),
# node paths ($Player/Ship), strings, numbers, comments, function names and members after a dot.

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

# word -> colour (keywords and type names)
var words := {}


func _init(keywords: Array, controlFlow: Array, baseTypes: Array, engineTypes: Array):
	for w in keywords: words[w] = KEYWORD

	for w in controlFlow: words[w] = CONTROL_FLOW

	for w in engineTypes: words[w] = ENGINE_TYPE

	for w in baseTypes: words[w] = BASE_TYPE


static func _startsWord(c: String) -> bool:
	return c == "_" or (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or c.unicode_at(0) > 127


static func _inWord(c: String) -> bool:
	return _startsWord(c) or (c >= "0" and c <= "9")


static func _digit(c: String) -> bool:
	return c >= "0" and c <= "9"


func _get_line_syntax_highlighting(lineNumber: int) -> Dictionary:
	var text := get_text_edit().get_line(lineNumber)
	var colors := {}
	var n := text.length()
	var i := 0
	var afterDot := false

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
			i = _skipString(text, i)
			colors[start] = {"color": STRING}
			afterDot = false
			continue

		if c == "@" and i + 1 < n and _startsWord(text[i + 1]):
			i += 1

			while i < n and _inWord(text[i]): i += 1

			colors[start] = {"color": ANNOTATION}
			continue

		if c == "$":
			i += 1

			if i < n and (text[i] == "\"" or text[i] == "'"):
				i = _skipString(text, i)
			else:
				while i < n and (_inWord(text[i]) or text[i] == "/"): i += 1

			colors[start] = {"color": NODE_PATH}
			afterDot = false
			continue

		if _digit(c) or (c == "." and i + 1 < n and _digit(text[i + 1]) and not afterDot):
			i += 1

			while i < n and (_inWord(text[i]) or text[i] == "." or ((text[i] == "-" or text[i] == "+") and text[i - 1] in ["e", "E"])):
				i += 1

			colors[start] = {"color": NUMBER}
			afterDot = false
			continue

		if _startsWord(c):
			while i < n and _inWord(text[i]): i += 1

			var word := text.substr(start, i - start)
			var next := i

			while next < n and (text[next] == " " or text[next] == "\t"): next += 1

			var call := next < n and text[next] == "("
			var color := TEXT

			if afterDot: color = FUNCTION if call else MEMBER
			elif words.has(word): color = words[word]
			elif call: color = FUNCTION
			colors[start] = {"color": color}
			afterDot = false
			continue

		colors[start] = {"color": SYMBOL}
		afterDot = c == "."
		i += 1

	return colors


# Index just past the closing quote (escapes skipped), or the end of the line.
func _skipString(text: String, i: int) -> int:
	var quote := text[i]
	i += 1

	while i < text.length() and text[i] != quote:
		if text[i] == "\\": i += 1

		i += 1

	return mini(i + 1, text.length())
