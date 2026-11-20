/**************************************************************************/
/*  haxe_source.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "haxe_source.h"

#include "core/object/class_db.h"

namespace {

enum TokenType {
	TOK_IDENT,
	TOK_NUMBER,
	TOK_STRING,
	TOK_SYMBOL,
	TOK_META,
	TOK_EOF,
};

struct Token {
	TokenType type = TOK_EOF;
	String text;
	int line = 1;
	int column = 1;
	int offset = 0;
	int end_offset = 0;
};

// A doc comment (slash-star-star ... star-slash), kept apart from the token
// stream so the parser can attach it to the declaration below it.
struct DocComment {
	String text;
	int end_offset = 0;
};

/* Doc comments are plain text: drop the leading `*` of each line and the
 * indentation, but keep the line breaks. */
String clean_doc_comment(const String &p_text) {
	String result;
	for (const String &raw_line : p_text.split("\n")) {
		String line = raw_line.strip_edges();
		if (line.begins_with("*")) {
			line = line.substr(1).strip_edges();
		}
		if (line.is_empty()) {
			if (!result.is_empty()) {
				result += "\n";
			}
			continue;
		}
		if (!result.is_empty() && !result.ends_with("\n")) {
			result += "\n";
		}
		result += line;
	}
	return result.strip_edges();
}

bool is_ident_start(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || p_char == '_' || p_char == '$';
}

bool is_ident_char(char32_t p_char) {
	return is_ident_start(p_char) || (p_char >= '0' && p_char <= '9');
}

class Lexer {
public:
	explicit Lexer(const String &p_source) :
			source(p_source) {}

	/* Filled by tokenize(); the parser attaches them to declarations. */
	Vector<DocComment> doc_comments;

	Vector<Token> tokenize() {
		Vector<Token> tokens;
		int length = source.length();
		int index = 0;
		int line = 1;
		int line_start = 0;
		bool line_has_code = false;
		while (index < length) {
			char32_t c = source[index];
			if (c == '\n') {
				index++;
				line++;
				line_start = index;
				line_has_code = false;
				continue;
			}
			if (c == ' ' || c == '\t' || c == '\r') {
				index++;
				continue;
			}
			if (c == '/' && index + 1 < length && source[index + 1] == '/') {
				while (index < length && source[index] != '\n') {
					index++;
				}
				continue;
			}
			if (c == '/' && index + 1 < length && source[index + 1] == '*') {
				// `/** ... */` documents the declaration that follows it;
				// `/* ... */` and `/**/` are plain comments.
				const bool is_doc = index + 2 < length && source[index + 2] == '*' &&
						!(index + 3 < length && source[index + 3] == '/');
				index += is_doc ? 3 : 2;
				const int body_start = index;
				while (index + 1 < length && !(source[index] == '*' && source[index + 1] == '/')) {
					if (source[index] == '\n') {
						line++;
						line_start = index + 1;
					}
					index++;
				}
				if (is_doc) {
					DocComment comment;
					comment.text = clean_doc_comment(source.substr(body_start, index - body_start));
					comment.end_offset = MIN(index + 2, length);
					doc_comments.push_back(comment);
				}
				index += 2;
				continue;
			}

			Token token;
			token.line = line;
			token.column = index - line_start + 1;
			token.offset = index;

			if (c == '@') {
				// Metadata: @:name
				int start = index;
				index++;
				if (index < length && source[index] == ':') {
					index++;
					while (index < length && is_ident_char(source[index])) {
						index++;
					}
				}
				token.type = TOK_META;
				token.text = source.substr(start, index - start);
				token.end_offset = index;
				tokens.push_back(token);
				line_has_code = true;
				continue;
			}
			if (is_ident_start(c)) {
				int start = index;
				while (index < length && is_ident_char(source[index])) {
					index++;
				}
				token.type = TOK_IDENT;
				token.text = source.substr(start, index - start);
				token.end_offset = index;
				tokens.push_back(token);
				line_has_code = true;
				continue;
			}
			if (c >= '0' && c <= '9') {
				int start = index;
				while (index < length) {
					char32_t n = source[index];
					if ((n >= '0' && n <= '9') || n == '.' || n == 'x' || n == 'X' || (n >= 'a' && n <= 'f') || (n >= 'A' && n <= 'F')) {
						index++;
					} else {
						break;
					}
				}
				token.type = TOK_NUMBER;
				token.text = source.substr(start, index - start);
				token.end_offset = index;
				tokens.push_back(token);
				line_has_code = true;
				continue;
			}
			if (c == '"' || c == '\'') {
				char32_t quote = c;
				int start = index;
				index++;
				while (index < length && source[index] != quote) {
					if (source[index] == '\\' && index + 1 < length) {
						index += 2;
						continue;
					}
					if (source[index] == '\n') {
						line++;
						line_start = index + 1;
					}
					index++;
				}
				if (index < length) {
					index++;
				}
				token.type = TOK_STRING;
				token.text = source.substr(start, index - start);
				token.end_offset = index;
				tokens.push_back(token);
				line_has_code = true;
				continue;
			}

			token.type = TOK_SYMBOL;
			// Keep multi-character operators together when they matter for
			// type parsing (=>, ->, ?. are irrelevant to declarations but
			// keeping them reduces token count).
			String symbol = String::chr(c);
			if (index + 1 < length) {
				char32_t next = source[index + 1];
				if ((c == '-' && next == '>') || (c == '=' && next == '>') || (c == '=' && next == '=') || (c == '!' && next == '=') ||
						(c == '&' && next == '&') || (c == '|' && next == '|') || (c == '?' && next == '.') ||
						(c == ':' && next == ':') || (c == '<' && next == '=') || (c == '>' && next == '=') ||
						(c == '+' && next == '+') || (c == '-' && next == '-') || (c == '+' && next == '=') ||
						(c == '-' && next == '=') || (c == '*' && next == '=') || (c == '/' && next == '=')) {
					symbol += String::chr(next);
					index++;
				}
			}
			index++;
			token.text = symbol;
			token.end_offset = index;
			tokens.push_back(token);
			line_has_code = true;
		}
		(void)line_has_code;
		Token eof;
		eof.type = TOK_EOF;
		eof.line = line;
		eof.offset = length;
		eof.end_offset = length;
		tokens.push_back(eof);
		return tokens;
	}

private:
	const String &source;
};

String clean_type(const String &p_type) {
	String type = p_type.strip_edges();
	while (type.begins_with("?")) {
		type = type.substr(1).strip_edges();
	}
	if (type.begins_with("godot.")) {
		type = type.substr(6);
	}
	return type;
}

/* Extracts the source text of a declaration initializer without running the
 * parser over it: the lexer gives reliable offsets. */
String source_between(const String &p_source, const Vector<Token> &p_tokens, int p_from, int p_to) {
	if (p_from < 0 || p_to < 0 || p_from >= p_tokens.size() || p_to >= p_tokens.size()) {
		return String();
	}
	int start = p_tokens[p_from].offset;
	int end = p_tokens[p_to].offset;
	if (end <= start) {
		return String();
	}
	return p_source.substr(start, end - start).strip_edges();
}

/* Splits an annotation's argument list on top-level commas. A comma inside a
 * quoted string (`@:exportEnum("Idle,Run")`) or inside nested brackets belongs
 * to the argument and must not split it. */
Vector<String> split_meta_arguments(const String &p_arguments) {
	Vector<String> parts;
	String current;
	int depth = 0;
	char32_t quote = 0;
	for (int i = 0; i < p_arguments.length(); i++) {
		const char32_t c = p_arguments[i];
		if (quote != 0) {
			current += c;
			if (c == quote) {
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			current += c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth--;
		}
		if (c == ',' && depth <= 0) {
			parts.push_back(current.strip_edges());
			current = String();
			continue;
		}
		current += c;
	}
	if (!current.strip_edges().is_empty() || !parts.is_empty()) {
		parts.push_back(current.strip_edges());
	}
	return parts;
}

/* Removes the quotes around an annotation argument, when it has them. */
String unquote_meta_argument(const String &p_argument) {
	String value = p_argument.strip_edges();
	if (value.length() >= 2 && ((value.begins_with("\"") && value.ends_with("\"")) || (value.begins_with("'") && value.ends_with("'")))) {
		return value.substr(1, value.length() - 2);
	}
	return value;
}

class SourceParser {
public:
	SourceParser(const String &p_source, HaxeClassInfo &p_info) :
			source(p_source), info(p_info), tokens(source_tokens()) {
		set_targets(p_info.members, p_info.variables, p_info.functions);
		current_type_name = p_info.name;
	}

	Error parse() {
		// Package declaration.
		for (int i = 0; i < tokens.size(); i++) {
			if (tokens[i].type == TOK_IDENT && tokens[i].text == "package") {
				String package;
				int j = i + 1;
				// A dotted package is a sequence of identifiers separated by
				// dots (`package game.sub;`), so the dots are part of the name.
				while (j < tokens.size() && (tokens[j].type == TOK_IDENT || (tokens[j].type == TOK_SYMBOL && tokens[j].text == "."))) {
					package += tokens[j].text;
					j++;
				}
				info.package = package;
				if (j < tokens.size() && tokens[j].text == ";") {
					i = j;
				}
				break;
			}
			if (tokens[i].type == TOK_EOF) {
				break;
			}
		}

		// Find the primary type declaration. A script file is normally a
		// `class`, but a `.hx` file may hold only an `enum`, a `typedef` or an
		// `interface`; those are indexed too, so completion can see them.
		int class_index = -1;
		static const char *PRIMARY_KEYWORDS[] = { "class", "abstract", nullptr };
		static const char *FALLBACK_KEYWORDS[] = { "enum", "interface", "typedef", nullptr };
		const char **keyword_sets[] = { PRIMARY_KEYWORDS, FALLBACK_KEYWORDS };
		for (int set = 0; set < 2 && class_index < 0; set++) {
			const char **keywords = keyword_sets[set];
			int depth = 0;
			for (int i = 0; i < tokens.size(); i++) {
				const Token &token = tokens[i];
				if (token.type == TOK_SYMBOL && token.text == "{") {
					depth++;
				} else if (token.type == TOK_SYMBOL && token.text == "}") {
					depth--;
				} else if (depth == 0 && token.type == TOK_IDENT) {
					// An `enum abstract` must not be mistaken for an
					// `abstract` declaration.
					if (token.text == "abstract" && i > 0 && tokens[i - 1].type == TOK_IDENT && tokens[i - 1].text == "enum") {
						continue;
					}
					for (int k = 0; keywords[k] != nullptr; k++) {
						if (token.text == keywords[k]) {
							class_index = i;
							break;
						}
					}
					if (class_index >= 0) {
						break;
					}
				}
			}
		}
		if (class_index < 0) {
			info.error = "No type declaration found.";
			return ERR_PARSE_ERROR;
		}

		String keyword = tokens[class_index].text;
		info.is_abstract = keyword == "abstract";
		info.class_line = tokens[class_index].line;
		info.doc = doc_for_offset(tokens[class_index].offset);
		// Collect metadata preceding the class, including annotations that are
		// separated from it by imports.
		for (int i = class_index - 1; i >= 0; i--) {
			if (tokens[i].type == TOK_META) {
				apply_metadata(tokens[i].text, info.is_tool, info.is_abstract, info.is_global_class);
			} else if (tokens[i].type != TOK_IDENT && tokens[i].type != TOK_SYMBOL) {
				break;
			} else if (tokens[i].type == TOK_SYMBOL && tokens[i].text == ";") {
				continue;
			}
		}

		int i = class_index + 1;
		if (keyword == "enum" && i < tokens.size() && tokens[i].type == TOK_IDENT && tokens[i].text == "abstract") {
			// `enum abstract Name(...)`: the values are declared with `var`,
			// and they are reached through the type (`Name.Value`).
			info.is_abstract = true;
			i++;
		}
		if (i >= tokens.size() || tokens[i].type != TOK_IDENT) {
			info.error = "Expected type name.";
			return ERR_PARSE_ERROR;
		}
		info.name = tokens[i].text;
		current_type_name = info.name;
		i++;

		if (keyword == "typedef") {
			/* `typedef Name = SomeType;` aliases another type, so the members
			 * of the alias are those of the target. Generic parameters may sit
			 * between the name and the `=` (`typedef Map<K, V> = ...`). A body
			 * instead of a target (`typedef Point = { x:Float }`) is parsed
			 * further down like a structure. */
			int j = i;
			int angle = 0;
			bool found_equals = false;
			for (; j < tokens.size(); j++) {
				const Token &token = tokens[j];
				if (token.type != TOK_SYMBOL) {
					continue;
				}
				if (token.text == "<") {
					angle++;
				} else if (token.text == ">") {
					angle--;
				} else if (angle <= 0 && token.text == "=") {
					found_equals = true;
					j++;
					break;
				} else if (angle <= 0 && (token.text == ";" || token.text == "{")) {
					break;
				}
			}
			if (found_equals && j < tokens.size() && !(tokens[j].type == TOK_SYMBOL && tokens[j].text == "{")) {
				int alias_start = j;
				while (j < tokens.size() && !(tokens[j].type == TOK_SYMBOL && tokens[j].text == ";")) {
					j++;
				}
				if (j > alias_start) {
					String alias;
					for (int k = alias_start; k < j; k++) {
						alias += tokens[k].text;
					}
					info.base = clean_type(alias);
					info.fq_name = info.package.is_empty() ? info.name : info.package + "." + info.name;
					info.valid = true;
					return OK;
				}
			}
		}
		if (i < tokens.size() && tokens[i].type == TOK_IDENT && tokens[i].text == "extends") {
			i++;
			String base;
			while (i < tokens.size() && tokens[i].type != TOK_EOF) {
				if (tokens[i].type == TOK_IDENT || (tokens[i].type == TOK_SYMBOL && tokens[i].text == ".")) {
					base += tokens[i].text;
					i++;
				} else if (tokens[i].type == TOK_SYMBOL && tokens[i].text == "<") {
					// Generic base class: skip to the matching '>'.
					int nested = 0;
					while (i < tokens.size() && tokens[i].type != TOK_EOF) {
						if (tokens[i].text == "<") {
							nested++;
						} else if (tokens[i].text == ">") {
							nested--;
							if (nested == 0) {
								i++;
								break;
							}
						}
						i++;
					}
				} else {
					break;
				}
			}
			info.base = clean_type(base);
		}

		info.fq_name = info.package.is_empty() ? info.name : info.package + "." + info.name;
		info.valid = true;

		// Find the type body.
		while (i < tokens.size() && !(tokens[i].type == TOK_SYMBOL && tokens[i].text == "{")) {
			i++;
		}
		if (i >= tokens.size()) {
			info.error = "Expected type body.";
			return ERR_PARSE_ERROR;
		}
		i++;
		int body_end = 0;
		if (keyword == "enum" && !info.is_abstract) {
			body_end = parse_enum_body(i);
		} else if (keyword == "typedef") {
			body_end = parse_structure_body(i);
		} else {
			// `enum abstract` values are reached through the type, and
			// interface members are implicitly public and static-like in Haxe
			// completion terms; both are recorded as static so `Name.` lists
			// them.
			force_static_members = keyword == "enum";
			body_end = parse_body(i);
			force_static_members = false;
		}
		parse_secondary_types(body_end);
		return OK;
	}

	/* Enum constructors: `Red;`, `Rgb(r:Int, g:Int, b:Int);`. A constructor
	 * without arguments is recorded as a static value (`Color.Red`); one with
	 * arguments as a static function, so completion inserts `Rgb(`. */
	int parse_enum_body(int p_index) {
		int depth = 1;
		int i = p_index;
		while (i < tokens.size() && depth > 0) {
			const Token &token = tokens[i];
			if (token.type == TOK_EOF) {
				break;
			}
			if (token.type == TOK_SYMBOL && token.text == "{") {
				depth++;
				i++;
				continue;
			}
			if (token.type == TOK_SYMBOL && token.text == "}") {
				depth--;
				i++;
				if (depth == 0) {
					break;
				}
				continue;
			}
			if (depth != 1) {
				i++;
				continue;
			}
			if (token.type == TOK_IDENT) {
				if (token.text == "var") {
					parse_variable(i, Vector<String>(), Vector<String>(), true);
					i = skip_to_member_end(i);
					continue;
				}
				if (token.text == "function") {
					parse_function(i, Vector<String>(), Vector<String>(), true);
					i = skip_function(i);
					continue;
				}
				record_enum_constructor(i);
				// Skip the constructor's argument list.
				int j = i + 1;
				if (j < tokens.size() && tokens[j].type == TOK_SYMBOL && tokens[j].text == "(") {
					int nested = 0;
					while (j < tokens.size()) {
						if (tokens[j].type == TOK_SYMBOL && tokens[j].text == "(") {
							nested++;
						} else if (tokens[j].type == TOK_SYMBOL && tokens[j].text == ")") {
							nested--;
							if (nested == 0) {
								break;
							}
						}
						j++;
					}
				}
				i = j + 1;
				continue;
			}
			i++;
		}
		return i;
	}

	void record_enum_constructor(int p_name_index) {
		if (p_name_index >= tokens.size() || tokens[p_name_index].type != TOK_IDENT) {
			return;
		}
		HaxeFunctionInfo function;
		function.name = tokens[p_name_index].text;
		function.line = tokens[p_name_index].line;
		function.is_static = true;
		int i = p_name_index + 1;
		if (i < tokens.size() && tokens[i].type == TOK_SYMBOL && tokens[i].text == "(") {
			// Reuse the argument walker so completion can hint the signature.
			parse_function_arguments(i, function);
			functions_target->push_back(function);
			return;
		}
		// A bare constructor is used as a value, not a call.
		HaxeMemberInfo member;
		member.name = function.name;
		member.line = function.line;
		member.is_static = true;
		member.type = current_type_name;
		variables_target->push_back(member);
	}

	/* Anonymous structure in a `typedef` (`typedef Point = { x:Float, y:Float }`).
	 * Fields may be separated by `,` or `;` and written with or without the
	 * `var` keyword; functions are parsed as usual. */
	int parse_structure_body(int p_index) {
		int depth = 1;
		int i = p_index;
		while (i < tokens.size() && depth > 0) {
			const Token &token = tokens[i];
			if (token.type == TOK_EOF) {
				break;
			}
			if (token.type == TOK_SYMBOL && (token.text == "{" || token.text == "(" || token.text == "[")) {
				depth++;
				i++;
				continue;
			}
			if (token.type == TOK_SYMBOL && (token.text == "}" || token.text == ")" || token.text == "]")) {
				depth--;
				i++;
				if (depth == 0) {
					break;
				}
				continue;
			}
			if (depth != 1) {
				i++;
				continue;
			}
			if (token.type == TOK_IDENT && token.text == "function") {
				parse_function(i, Vector<String>(), Vector<String>(), false);
				i = skip_function(i);
				continue;
			}
			if (token.type == TOK_IDENT && token.text == "var") {
				parse_variable(i, Vector<String>(), Vector<String>(), false);
				i = skip_to_member_end(i);
				continue;
			}
			if (token.type == TOK_IDENT) {
				// `x:Float`
				int colon = i + 1;
				if (colon < tokens.size() && tokens[colon].type == TOK_SYMBOL && tokens[colon].text == ":") {
					HaxeMemberInfo member;
					member.name = token.text;
					member.line = token.line;
					member.type = read_type(++colon);
					variables_target->push_back(member);
					i = colon;
					continue;
				}
			}
			i++;
		}
		return i;
	}

	/* Top level types declared after the primary class. Haxe allows several
	 * types per file; they are imported as `package.FileName.Type` and are
	 * visible from the declaring file. Their members are parsed with the same
	 * routines as the primary class so completion and lookup can use them. */
	void parse_secondary_types(int p_from) {
		int depth = 0;
		for (int i = p_from; i < tokens.size(); i++) {
			const Token &token = tokens[i];
			if (token.type == TOK_SYMBOL && token.text == "{") {
				depth++;
				continue;
			}
			if (token.type == TOK_SYMBOL && token.text == "}") {
				depth--;
				continue;
			}
			if (depth != 0 || token.type != TOK_IDENT) {
				continue;
			}
			bool is_type_keyword = token.text == "class" || token.text == "abstract" || token.text == "interface" ||
					token.text == "enum" || token.text == "typedef";
			if (!is_type_keyword) {
				continue;
			}
			i = parse_secondary_type(i);
		}
	}

	/* Parses one secondary type starting at its keyword and returns the index
	 * of the last token it consumed. */
	int parse_secondary_type(int p_keyword) {
		int name_index = p_keyword + 1;
		// `enum abstract Name(...)` names the type after two keywords.
		if (tokens[p_keyword].text == "enum" && name_index < tokens.size() && tokens[name_index].type == TOK_IDENT && tokens[name_index].text == "abstract") {
			name_index++;
		}
		if (name_index >= tokens.size() || tokens[name_index].type != TOK_IDENT) {
			return p_keyword;
		}

		HaxeSecondaryTypeInfo type;
		type.name = tokens[name_index].text;
		type.line = tokens[name_index].line;
		type.is_abstract = tokens[p_keyword].text == "abstract";
		String keyword = tokens[p_keyword].text;
		bool is_enum = keyword == "enum";
		bool is_enum_abstract = is_enum && tokens[p_keyword + 1].type == TOK_IDENT && tokens[p_keyword + 1].text == "abstract";
		if (is_enum_abstract) {
			type.is_abstract = true;
		}

		int i = name_index + 1;
		// Skip generic parameters and an `enum abstract`'s underlying type.
		while (i < tokens.size() && !(tokens[i].type == TOK_SYMBOL && (tokens[i].text == "{" || tokens[i].text == ";"))) {
			i++;
		}
		if (i >= tokens.size() || tokens[i].text == ";") {
			// A `typedef Name = Other;` alias: the members are those of the
			// aliased type. Generic parameters may sit before the `=`.
			if (keyword == "typedef") {
				int equals = -1;
				int angle = 0;
				for (int j = name_index + 1; j < i; j++) {
					if (tokens[j].type != TOK_SYMBOL) {
						continue;
					}
					if (tokens[j].text == "<") {
						angle++;
					} else if (tokens[j].text == ">") {
						angle--;
					} else if (angle <= 0 && tokens[j].text == "=") {
						equals = j;
						break;
					}
				}
				if (equals >= 0) {
					String alias;
					for (int j = equals + 1; j < i; j++) {
						alias += tokens[j].text;
					}
					type.base = clean_type(alias);
				}
			}
			record_secondary_type(type);
			return i;
		}

		// The body starts at `{`; `extends` comes before it.
		for (int j = name_index + 1; j < i; j++) {
			if (tokens[j].type == TOK_IDENT && tokens[j].text == "extends") {
				String base;
				int k = j + 1;
				while (k < i && (tokens[k].type == TOK_IDENT || (tokens[k].type == TOK_SYMBOL && tokens[k].text == "."))) {
					base += tokens[k].text;
					k++;
				}
				type.base = clean_type(base);
				break;
			}
		}

		set_targets(type.members, type.variables, type.functions);
		current_type_name = type.name;
		int body_end = 0;
		if (is_enum && !is_enum_abstract) {
			body_end = parse_enum_body(i + 1);
		} else if (keyword == "typedef") {
			body_end = parse_structure_body(i + 1);
		} else {
			force_static_members = is_enum;
			body_end = parse_body(i + 1);
			force_static_members = false;
		}
		record_secondary_type(type);
		return body_end - 1;
	}

	void record_secondary_type(const HaxeSecondaryTypeInfo &p_type) {
		if (p_type.name.is_empty() || p_type.name == info.name || info.secondary_classes.has(p_type.name)) {
			return;
		}
		info.secondary_classes.push_back(p_type.name);
		info.secondary_types.push_back(p_type);
	}

	/* Routes member declarations to the type currently being parsed. */
	void set_targets(Vector<HaxeMemberInfo> &p_members, Vector<HaxeMemberInfo> &p_variables, Vector<HaxeFunctionInfo> &p_functions) {
		members_target = &p_members;
		variables_target = &p_variables;
		functions_target = &p_functions;
	}

private:
	const String &source;
	HaxeClassInfo &info;
	Vector<Token> source_tokens_cache;
	Vector<DocComment> doc_comments;
	const Vector<Token> &source_tokens() {
		if (source_tokens_cache.is_empty()) {
			Lexer lexer(source);
			source_tokens_cache = lexer.tokenize();
			doc_comments = lexer.doc_comments;
		}
		return source_tokens_cache;
	}
	Vector<Token> tokens;

	/* Doc comment attached to the declaration that starts at `p_offset`.
	 * A doc comment belongs to the next declaration; only blank lines and
	 * annotation lines (`@:export`, `@:tool`, ...) may sit in between, since
	 * those describe the declaration as well. */
	String doc_for_offset(int p_offset) {
		for (int i = doc_comments.size() - 1; i >= 0; i--) {
			const DocComment &comment = doc_comments[i];
			if (comment.end_offset > p_offset) {
				continue;
			}
			bool attached = true;
			int start = comment.end_offset;
			while (start < p_offset) {
				int end = source.find_char('\n', start);
				// The line the declaration starts on holds whatever precedes
				// the declaration itself (`public`, `override`, ...), so only
				// whole lines in between are checked for foreign code.
				if (end < 0 || end >= p_offset) {
					break;
				}
				const String line = source.substr(start, end - start).strip_edges();
				if (!line.is_empty() && !line.begins_with("@")) {
					attached = false;
					break;
				}
				start = end + 1;
			}
			return attached ? comment.text : String();
		}
		return String();
	}

	/* Where `parse_variable` / `parse_function` append. They point at the
	 * primary class while its body is parsed and at the secondary type being
	 * parsed otherwise. */
	Vector<HaxeMemberInfo> *members_target = nullptr;
	Vector<HaxeMemberInfo> *variables_target = nullptr;
	Vector<HaxeFunctionInfo> *functions_target = nullptr;

	/* Set while an `enum abstract` body is parsed: its values are reached
	 * through the type, so they are recorded as static. */
	bool force_static_members = false;
	/* The type whose body is being parsed; enum constructor values are typed
	 * with it. */
	String current_type_name;

	void apply_metadata(const String &p_meta, bool &r_tool, bool &r_abstract, bool &r_global_class) {
		if (p_meta == "@:tool" || p_meta == "@:gdTool" || p_meta == "@:editor") {
			r_tool = true;
		}
		if (p_meta == "@:abstract") {
			r_abstract = true;
		}
		if (p_meta == "@:globalClass" || p_meta == "@:gdGlobalClass") {
			r_global_class = true;
		}
	}

	/* Parses a class body starting at the token after its `{`. Returns the
	 * index of the token after the closing `}`. */
	int parse_body(int p_index) {
		int depth = 1;
		int i = p_index;
		Vector<String> metadata;
		Vector<String> metadata_arguments;
		// `static` is a plain keyword, not a metadata, so it is tracked while
		// walking the modifiers that precede a declaration.
		bool is_static_member = false;

		while (i < tokens.size() && depth > 0) {
			const Token &token = tokens[i];
			if (token.type == TOK_EOF) {
				break;
			}
			if (token.type == TOK_SYMBOL && token.text == "{") {
				depth++;
				i++;
				continue;
			}
			if (token.type == TOK_SYMBOL && token.text == "}") {
				depth--;
				i++;
				if (depth == 0) {
					break;
				}
				continue;
			}
			if (depth != 1) {
				i++;
				continue;
			}

			if (token.type == TOK_META) {
				metadata.push_back(token.text);
				String arguments;
				int j = i + 1;
				if (j < tokens.size() && tokens[j].type == TOK_SYMBOL && tokens[j].text == "(") {
					int nested = 0;
					while (j < tokens.size()) {
						if (tokens[j].type == TOK_SYMBOL && tokens[j].text == "(") {
							nested++;
						} else if (tokens[j].type == TOK_SYMBOL && tokens[j].text == ")") {
							nested--;
							if (nested == 0) {
								break;
							}
						}
						j++;
					}
					arguments = source_between(source, tokens, i + 2, j);
					i = j;
				}
				metadata_arguments.push_back(arguments);
				i++;
				continue;
			}

			if (token.type == TOK_IDENT && (token.text == "var" || token.text == "final")) {
				// `final name = ...;` declares a field without the `var`
				// keyword; it is parsed like `var` so it still completes and
				// (with `@:export`) reaches the Inspector.
				parse_variable(i, metadata, metadata_arguments, is_static_member);
				metadata.clear();
				metadata_arguments.clear();
				is_static_member = false;
				i = skip_to_member_end(i);
				continue;
			}
			if (token.type == TOK_IDENT && token.text == "function") {
				parse_function(i, metadata, metadata_arguments, is_static_member);
				metadata.clear();
				metadata_arguments.clear();
				is_static_member = false;
				i = skip_function(i);
				continue;
			}
			if (token.type == TOK_IDENT && token.text == "static") {
				is_static_member = true;
				i++;
				continue;
			}
			if (token.type == TOK_IDENT && (token.text == "public" || token.text == "private" || token.text == "override" || token.text == "inline" || token.text == "extern" || token.text == "final" || token.text == "dynamic" || token.text == "macro" || token.text == "function" || token.text == "var")) {
				i++;
				continue;
			}
			metadata.clear();
			metadata_arguments.clear();
			is_static_member = false;
			i++;
		}
		return i;
	}

	bool has_meta(const Vector<String> &p_metadata, const String &p_name) const {
		for (const String &meta : p_metadata) {
			if (meta == p_name) {
				return true;
			}
		}
		return false;
	}

	String meta_argument(const Vector<String> &p_metadata, const Vector<String> &p_arguments, const String &p_name, int p_index = 0) const {
		for (int i = 0; i < p_metadata.size(); i++) {
			if (p_metadata[i] == p_name) {
				if (i >= p_arguments.size()) {
					return String();
				}
				Vector<String> parts = split_meta_arguments(p_arguments[i]);
				if (p_index < parts.size()) {
					return unquote_meta_argument(parts[p_index]);
				}
				return String();
			}
		}
		return String();
	}

	/* The whole argument text of an annotation, with its quotes removed.
	 * Hint strings are comma separated, and both spellings produce the same
	 * value: one quoted list (`@:exportEnum("Idle,Run")`) and one quoted value
	 * per entry (`@:exportEnum("Idle", "Run")`). */
	String meta_text(const Vector<String> &p_metadata, const Vector<String> &p_arguments, const String &p_name) const {
		for (int i = 0; i < p_metadata.size(); i++) {
			if (p_metadata[i] != p_name) {
				continue;
			}
			if (i >= p_arguments.size()) {
				return String();
			}
			String result;
			for (const String &part : split_meta_arguments(p_arguments[i])) {
				if (!result.is_empty()) {
					result += ",";
				}
				result += unquote_meta_argument(part);
			}
			return result;
		}
		return String();
	}

	void parse_variable(int p_index, const Vector<String> &p_metadata, const Vector<String> &p_arguments, bool p_static) {
		int i = p_index + 1;
		if (i >= tokens.size() || tokens[i].type != TOK_IDENT) {
			return;
		}
		HaxeMemberInfo member;
		member.name = tokens[i].text;
		member.line = tokens[i].line;
		member.doc = doc_for_offset(tokens[p_index].offset);
		member.is_static = p_static || force_static_members || has_meta(p_metadata, "@:static");
		i++;
		if (i < tokens.size() && tokens[i].type == TOK_SYMBOL && tokens[i].text == ":") {
			i++;
			member.type = read_type(i);
		}
		if (i < tokens.size() && tokens[i].type == TOK_SYMBOL && tokens[i].text == "=") {
			int start = i + 1;
			int j = start;
			int nested = 0;
			while (j < tokens.size() && tokens[j].type != TOK_EOF) {
				if (tokens[j].type == TOK_SYMBOL && (tokens[j].text == "(" || tokens[j].text == "[" || tokens[j].text == "{")) {
					nested++;
				} else if (tokens[j].type == TOK_SYMBOL && (tokens[j].text == ")" || tokens[j].text == "]" || tokens[j].text == "}")) {
					if (nested == 0) {
						break;
					}
					nested--;
				} else if (nested == 0 && tokens[j].type == TOK_SYMBOL && tokens[j].text == ";") {
					break;
				}
				j++;
			}
			member.default_source = source_between(source, tokens, start - 1, j);
			if (member.default_source.begins_with("=")) {
				member.default_source = member.default_source.substr(1).strip_edges();
			}
			bool ok = false;
			member.default_value = haxe_eval_literal(member.default_source, ok);
			if (!ok) {
				member.default_value = Variant();
			}
		}
		// Without an explicit `:Type`, the initializer still tells the editor
		// what the variable holds (`@:export var speed = 120.0` is a Float),
		// so the Inspector shows a real editor and completion reaches the
		// members of `var node = new Node()`. A declaration whose initializer
		// carries no type information stays untyped.
		if (member.type.is_empty() && !member.default_source.is_empty()) {
			member.type = haxe_infer_initializer_type(member.default_source);
		}
		// An export hint is enough on its own: `@:exportRange(...) var speed`
		// is an exported property, exactly like `@:export @:exportRange(...)`.
		// Requiring both made the hint silently do nothing.
		const bool has_export_hint = has_meta(p_metadata, "@:exportRange") ||
				has_meta(p_metadata, "@:exportEnum") || has_meta(p_metadata, "@:exportFile") ||
				has_meta(p_metadata, "@:exportDir") || has_meta(p_metadata, "@:exportMultiline");
		member.exported = has_meta(p_metadata, "@:export") || has_meta(p_metadata, "@:exported") || has_export_hint;
		member.group = meta_argument(p_metadata, p_arguments, "@:exportGroup");
		member.subgroup = meta_argument(p_metadata, p_arguments, "@:exportSubgroup");

		if (has_meta(p_metadata, "@:exportRange")) {
			member.hint = PROPERTY_HINT_RANGE;
			String minimum = meta_argument(p_metadata, p_arguments, "@:exportRange", 0);
			String maximum = meta_argument(p_metadata, p_arguments, "@:exportRange", 1);
			String step = meta_argument(p_metadata, p_arguments, "@:exportRange", 2);
			member.hint_string = minimum + "," + maximum + (step.is_empty() ? String() : "," + step);
		} else if (has_meta(p_metadata, "@:exportEnum")) {
			member.hint = PROPERTY_HINT_ENUM;
			member.hint_string = meta_text(p_metadata, p_arguments, "@:exportEnum");
		} else if (has_meta(p_metadata, "@:exportFile")) {
			member.hint = PROPERTY_HINT_FILE;
			member.hint_string = meta_text(p_metadata, p_arguments, "@:exportFile");
		} else if (has_meta(p_metadata, "@:exportDir")) {
			member.hint = PROPERTY_HINT_DIR;
		} else if (has_meta(p_metadata, "@:exportMultiline")) {
			member.hint = PROPERTY_HINT_MULTILINE_TEXT;
		}

		// Static variables are not engine properties: hxcpp keeps them in the
		// class-wide `__GetStatic`/`__SetStatic` tables, which an instance
		// cannot reach through `__Field`. Exporting one would therefore fail
		// on every Inspector refresh, so only instance variables become
		// inspector properties. The variable itself stays visible to the
		// parser and to completion.
		if (member.exported && !member.is_static) {
			members_target->push_back(member);
		}
		variables_target->push_back(member);
	}

	void parse_function(int p_index, const Vector<String> &p_metadata, const Vector<String> &p_arguments, bool p_static) {
		int i = p_index + 1;
		if (i >= tokens.size() || tokens[i].type != TOK_IDENT) {
			return;
		}
		HaxeFunctionInfo function;
		function.name = tokens[i].text;
		function.line = tokens[i].line;
		function.doc = doc_for_offset(tokens[p_index].offset);
		function.is_static = p_static || force_static_members || has_meta(p_metadata, "@:static");
		function.is_signal = has_meta(p_metadata, "@:signal") || has_meta(p_metadata, "@:gdSignal");
		i++;
		if (i < tokens.size() && tokens[i].type == TOK_SYMBOL && tokens[i].text == "<") {
			int nested = 0;
			while (i < tokens.size()) {
				if (tokens[i].text == "<") {
					nested++;
				} else if (tokens[i].text == ">") {
					nested--;
					if (nested == 0) {
						i++;
						break;
					}
				}
				i++;
			}
		}
		if (i >= tokens.size() || !(tokens[i].type == TOK_SYMBOL && tokens[i].text == "(")) {
			return;
		}
		i = parse_function_arguments(i, function);
		if (i < tokens.size() && tokens[i].type == TOK_SYMBOL && tokens[i].text == ":") {
			i++;
			function.return_type = read_type(i);
		}
		functions_target->push_back(function);
	}

	/* Parses a parameter list starting at the `(`. Returns the index just past
	 * the matching `)`. */
	int parse_function_arguments(int p_index, HaxeFunctionInfo &r_function) {
		int i = p_index + 1;
		int depth = 1;
		String argument_name;
		String argument_type;
		int state = 0; // 0 = name, 1 = type, 2 = default
		while (i < tokens.size() && depth > 0) {
			const Token &token = tokens[i];
			if (token.type == TOK_SYMBOL && token.text == "(") {
				depth++;
			} else if (token.type == TOK_SYMBOL && token.text == ")") {
				depth--;
				if (depth == 0) {
					if (!argument_name.is_empty()) {
						HaxeArgumentInfo argument;
						argument.name = argument_name;
						argument.type = clean_type(argument_type);
						r_function.arguments.push_back(argument);
					}
					i++;
					break;
				}
			}
			if (depth == 1) {
				if (state == 0 && token.type == TOK_IDENT) {
					argument_name = token.text;
					argument_type = String();
					state = 1;
				} else if (state == 1 && token.type == TOK_SYMBOL && token.text == ":") {
					// The type starts after the colon; `read_type` would
					// otherwise stop on the colon itself and yield nothing.
					int type_index = i + 1;
					argument_type = read_type(type_index);
					state = 3;
					i = type_index;
					continue;
				} else if (state == 3 && token.type == TOK_SYMBOL && token.text == ",") {
					HaxeArgumentInfo argument;
					argument.name = argument_name;
					argument.type = clean_type(argument_type);
					r_function.arguments.push_back(argument);
					argument_name = String();
					argument_type = String();
					state = 0;
				}
			}
			i++;
		}
		return i;
	}

	String read_type(int &p_index) {
		String type;
		int i = p_index;
		int nested = 0;
		while (i < tokens.size() && tokens[i].type != TOK_EOF) {
			const Token &token = tokens[i];
			if (token.type == TOK_IDENT) {
				type += token.text;
			} else if (token.type == TOK_SYMBOL && token.text == ".") {
				type += ".";
			} else if (token.type == TOK_SYMBOL && token.text == "<") {
				nested++;
				type += "<";
			} else if (token.type == TOK_SYMBOL && token.text == ">") {
				nested--;
				type += ">";
				if (nested == 0) {
					i++;
					break;
				}
			} else if (token.type == TOK_SYMBOL && token.text == ",") {
				// A comma only continues the type inside generic arguments
				// (`Map<String, Int>`); at the top level it ends it.
				if (nested <= 0) {
					break;
				}
				type += ",";
			} else if (token.type == TOK_SYMBOL && token.text == "->") {
				type += "->";
			} else if (token.type == TOK_SYMBOL && token.text == "?") {
				type += "?";
			} else {
				break;
			}
			i++;
			if (type.length() > 200) {
				break;
			}
		}
		p_index = i;
		return clean_type(type);
	}

	int skip_to_member_end(int p_index) {
		int i = p_index;
		int nested = 0;
		while (i < tokens.size() && tokens[i].type != TOK_EOF) {
			const Token &token = tokens[i];
			if (token.type == TOK_SYMBOL && (token.text == "(" || token.text == "[" || token.text == "{")) {
				nested++;
			} else if (token.type == TOK_SYMBOL && (token.text == ")" || token.text == "]" || token.text == "}")) {
				nested--;
			} else if (nested == 0 && token.type == TOK_SYMBOL && token.text == ";") {
				return i + 1;
			}
			i++;
		}
		return i;
	}

	int skip_function(int p_index) {
		int i = p_index;
		while (i < tokens.size() && !(tokens[i].type == TOK_SYMBOL && tokens[i].text == "(")) {
			i++;
		}
		// Skip the parameter list.
		int depth = 0;
		while (i < tokens.size()) {
			if (tokens[i].type == TOK_SYMBOL && tokens[i].text == "(") {
				depth++;
			} else if (tokens[i].type == TOK_SYMBOL && tokens[i].text == ")") {
				depth--;
				if (depth == 0) {
					i++;
					break;
				}
			}
			i++;
		}
		// Skip a return type annotation.
		while (i < tokens.size() && tokens[i].type != TOK_EOF) {
			if (tokens[i].type == TOK_SYMBOL && tokens[i].text == "{") {
				break;
			}
			if (tokens[i].type == TOK_SYMBOL && tokens[i].text == ";") {
				return i + 1;
			}
			i++;
		}
		if (i >= tokens.size()) {
			return i;
		}
		int brace_depth = 0;
		while (i < tokens.size()) {
			if (tokens[i].type == TOK_SYMBOL && tokens[i].text == "{") {
				brace_depth++;
			} else if (tokens[i].type == TOK_SYMBOL && tokens[i].text == "}") {
				brace_depth--;
				if (brace_depth == 0) {
					return i + 1;
				}
			}
			i++;
		}
		return i;
	}
};

} // namespace

MethodInfo HaxeFunctionInfo::to_method_info() const {
	MethodInfo method;
	method.name = name;
	for (const HaxeArgumentInfo &argument : arguments) {
		PropertyInfo property;
		String class_name;
		property.type = haxe_type_to_variant(argument.type, class_name);
		property.name = argument.name;
		if (!class_name.is_empty()) {
			property.hint = PROPERTY_HINT_TYPE_STRING;
			property.hint_string = class_name;
		}
		method.arguments.push_back(property);
	}
	String class_name;
	method.return_val.type = haxe_type_to_variant(return_type, class_name);
	if (!class_name.is_empty()) {
		method.return_val.hint = PROPERTY_HINT_TYPE_STRING;
		method.return_val.hint_string = class_name;
	}
	return method;
}

PropertyInfo HaxeMemberInfo::to_property_info() const {
	PropertyInfo property;
	String class_name;
	property.type = haxe_type_to_variant(type, class_name);
	property.name = name;
	property.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
	if (!class_name.is_empty()) {
		property.class_name = class_name;
	}
	if (hint != PROPERTY_HINT_NONE) {
		property.hint = hint;
		property.hint_string = hint_string;
	}
	return property;
}

MethodInfo HaxeClassInfo::to_signal_method_info(const HaxeFunctionInfo &p_function) const {
	MethodInfo method = p_function.to_method_info();
	method.flags |= METHOD_FLAG_VIRTUAL;
	return method;
}

const HaxeFunctionInfo *HaxeClassInfo::find_function(const String &p_name) const {
	for (const HaxeFunctionInfo &function : functions) {
		if (function.name == p_name) {
			return &function;
		}
	}
	return nullptr;
}

const HaxeMemberInfo *HaxeClassInfo::find_member(const String &p_name) const {
	for (const HaxeMemberInfo &member : members) {
		if (member.name == p_name) {
			return &member;
		}
	}
	return nullptr;
}

const HaxeMemberInfo *HaxeClassInfo::find_variable(const String &p_name) const {
	for (const HaxeMemberInfo &member : variables) {
		if (member.name == p_name) {
			return &member;
		}
	}
	return nullptr;
}

Error parse_haxe_source(const String &p_source, const String &p_path, HaxeClassInfo &r_info) {
	r_info = HaxeClassInfo();
	r_info.path = p_path;
	SourceParser parser(p_source, r_info);
	return parser.parse();
}

Variant::Type haxe_type_to_variant(const String &p_type, String &r_class_name) {
	r_class_name = String();
	String type = clean_type(p_type);
	if (type.is_empty() || type == "Void" || type == "Null" || type == "Dynamic") {
		return Variant::NIL;
	}
	if (type == "Int" || type == "UInt") {
		return Variant::INT;
	}
	if (type == "Float") {
		return Variant::FLOAT;
	}
	if (type == "Bool") {
		return Variant::BOOL;
	}
	if (type == "String") {
		return Variant::STRING;
	}
	if (type == "StringName") {
		return Variant::STRING_NAME;
	}
	if (type == "NodePath") {
		return Variant::NODE_PATH;
	}
	if (type == "Vector2") {
		return Variant::VECTOR2;
	}
	if (type == "Vector2i") {
		return Variant::VECTOR2I;
	}
	if (type == "Rect2") {
		return Variant::RECT2;
	}
	if (type == "Rect2i") {
		return Variant::RECT2I;
	}
	if (type == "Vector3") {
		return Variant::VECTOR3;
	}
	if (type == "Vector3i") {
		return Variant::VECTOR3I;
	}
	if (type == "Transform2D") {
		return Variant::TRANSFORM2D;
	}
	if (type == "Vector4") {
		return Variant::VECTOR4;
	}
	if (type == "Vector4i") {
		return Variant::VECTOR4I;
	}
	if (type == "Plane") {
		return Variant::PLANE;
	}
	if (type == "Quaternion") {
		return Variant::QUATERNION;
	}
	if (type == "AABB") {
		return Variant::AABB;
	}
	if (type == "Basis") {
		return Variant::BASIS;
	}
	if (type == "Transform3D") {
		return Variant::TRANSFORM3D;
	}
	if (type == "Projection") {
		return Variant::PROJECTION;
	}
	if (type == "Color") {
		return Variant::COLOR;
	}
	if (type == "RID") {
		return Variant::RID;
	}
	if (type == "Callable") {
		return Variant::CALLABLE;
	}
	if (type == "Signal") {
		return Variant::SIGNAL;
	}
	if (type == "Array" || type.begins_with("Array<")) {
		return Variant::ARRAY;
	}
	if (type == "Dictionary" || type.begins_with("Map<")) {
		return Variant::DICTIONARY;
	}
	if (type == "PackedByteArray") {
		return Variant::PACKED_BYTE_ARRAY;
	}
	if (type == "PackedInt32Array") {
		return Variant::PACKED_INT32_ARRAY;
	}
	if (type == "PackedInt64Array") {
		return Variant::PACKED_INT64_ARRAY;
	}
	if (type == "PackedFloat32Array") {
		return Variant::PACKED_FLOAT32_ARRAY;
	}
	if (type == "PackedFloat64Array") {
		return Variant::PACKED_FLOAT64_ARRAY;
	}
	if (type == "PackedStringArray") {
		return Variant::PACKED_STRING_ARRAY;
	}
	if (type == "PackedVector2Array") {
		return Variant::PACKED_VECTOR2_ARRAY;
	}
	if (type == "PackedVector3Array") {
		return Variant::PACKED_VECTOR3_ARRAY;
	}
	if (type == "PackedColorArray") {
		return Variant::PACKED_COLOR_ARRAY;
	}
	if (type == "PackedVector4Array") {
		return Variant::PACKED_VECTOR4_ARRAY;
	}

	// Anything else is treated as an engine object class (or a project script
	// class, which is validated by the parser / class discovery).
	if (type.contains("<")) {
		type = type.substr(0, type.find("<"));
	}
	r_class_name = type;
	return Variant::OBJECT;
}

String variant_type_to_haxe(Variant::Type p_type) {
	switch (p_type) {
		case Variant::NIL:
			return "Dynamic";
		case Variant::BOOL:
			return "Bool";
		case Variant::INT:
			return "Int";
		case Variant::FLOAT:
			return "Float";
		case Variant::STRING:
			return "String";
		case Variant::STRING_NAME:
			return "StringName";
		case Variant::NODE_PATH:
			return "NodePath";
		case Variant::VECTOR2:
			return "Vector2";
		case Variant::VECTOR2I:
			return "Vector2i";
		case Variant::RECT2:
			return "Rect2";
		case Variant::RECT2I:
			return "Rect2i";
		case Variant::VECTOR3:
			return "Vector3";
		case Variant::VECTOR3I:
			return "Vector3i";
		case Variant::TRANSFORM2D:
			return "Transform2D";
		case Variant::VECTOR4:
			return "Vector4";
		case Variant::VECTOR4I:
			return "Vector4i";
		case Variant::PLANE:
			return "Plane";
		case Variant::QUATERNION:
			return "Quaternion";
		case Variant::AABB:
			return "AABB";
		case Variant::BASIS:
			return "Basis";
		case Variant::TRANSFORM3D:
			return "Transform3D";
		case Variant::PROJECTION:
			return "Projection";
		case Variant::COLOR:
			return "Color";
		case Variant::RID:
			return "RID";
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Signal";
		case Variant::ARRAY:
			return "Array<Dynamic>";
		case Variant::DICTIONARY:
			return "Dictionary";
		case Variant::PACKED_BYTE_ARRAY:
			return "PackedByteArray";
		case Variant::PACKED_INT32_ARRAY:
			return "PackedInt32Array";
		case Variant::PACKED_INT64_ARRAY:
			return "PackedInt64Array";
		case Variant::PACKED_FLOAT32_ARRAY:
			return "PackedFloat32Array";
		case Variant::PACKED_FLOAT64_ARRAY:
			return "PackedFloat64Array";
		case Variant::PACKED_STRING_ARRAY:
			return "PackedStringArray";
		case Variant::PACKED_VECTOR2_ARRAY:
			return "PackedVector2Array";
		case Variant::PACKED_VECTOR3_ARRAY:
			return "PackedVector3Array";
		case Variant::PACKED_COLOR_ARRAY:
			return "PackedColorArray";
		case Variant::PACKED_VECTOR4_ARRAY:
			return "PackedVector4Array";
		default:
			return "Dynamic";
	}
}

String haxe_infer_initializer_type(const String &p_source) {
	String source = p_source.strip_edges();
	if (source.is_empty()) {
		return String();
	}

	// `new SomeType(...)` (or `new game.SomeType(...)`): the declared type is
	// the constructed class. A generic argument list is dropped.
	if (source.begins_with("new ") || source.begins_with("new\t")) {
		String rest = source.substr(3).strip_edges();
		String name;
		for (int i = 0; i < rest.length(); i++) {
			char32_t c = rest[i];
			bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
			if (!ident) {
				break;
			}
			name += c;
		}
		return name;
	}

	// Collection literals. The element types are not known without a type
	// checker, so they are left as `Dynamic`, which still tells the Inspector
	// (and completion) that the value is a collection.
	if (source.begins_with("[")) {
		return "Array<Dynamic>";
	}
	if (source.begins_with("{")) {
		return "Dictionary";
	}

	// Literals and the math value constructors (`Vector2(1, 2)`, `Color(...)`)
	// are evaluated by the same helper the default-value parser uses.
	bool ok = false;
	Variant literal = haxe_eval_literal(source, ok);
	if (!ok || literal.get_type() == Variant::NIL) {
		return String();
	}
	return variant_type_to_haxe(literal.get_type());
}

Variant haxe_eval_literal(const String &p_source, bool &r_ok) {
	r_ok = true;
	String source = p_source.strip_edges();
	if (source.is_empty()) {
		r_ok = false;
		return Variant();
	}
	if (source == "null") {
		return Variant();
	}
	if (source == "true") {
		return true;
	}
	if (source == "false") {
		return false;
	}
	if ((source.begins_with("\"") && source.ends_with("\"")) || (source.begins_with("'") && source.ends_with("'"))) {
		return source.substr(1, source.length() - 2);
	}
	bool is_number = true;
	for (int i = 0; i < source.length(); i++) {
		char32_t c = source[i];
		if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E' || c == 'x' || c == 'X' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			is_number = false;
			break;
		}
	}
	if (is_number) {
		if (source.contains(".") || source.contains("e") || source.contains("E")) {
			return source.to_float();
		}
		return (int64_t)source.to_int();
	}

	// Constructor literals for the common math types.
	struct Constructor {
		const char *name;
		Variant::Type type;
	};
	static const Constructor constructors[] = {
		{ "Vector2", Variant::VECTOR2 },
		{ "Vector2i", Variant::VECTOR2I },
		{ "Vector3", Variant::VECTOR3 },
		{ "Vector3i", Variant::VECTOR3I },
		{ "Vector4", Variant::VECTOR4 },
		{ "Color", Variant::COLOR },
		{ "Rect2", Variant::RECT2 },
	};
	for (const Constructor &constructor : constructors) {
		String prefix = String(constructor.name) + "(";
		if (source.begins_with(prefix) && source.ends_with(")")) {
			String inner = source.substr(prefix.length() - 1 + 1, source.length() - prefix.length() - 1);
			Vector<String> parts = inner.split(",", false);
			Vector<double> numbers;
			for (const String &part : parts) {
				String value = part.strip_edges();
				if (value.is_empty()) {
					continue;
				}
				bool numeric = true;
				for (int i = 0; i < value.length(); i++) {
					char32_t c = value[i];
					if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
						numeric = false;
						break;
					}
				}
				if (!numeric) {
					r_ok = false;
					return Variant();
				}
				numbers.push_back(value.to_float());
			}
			switch (constructor.type) {
				case Variant::VECTOR2:
					if (numbers.size() == 2) {
						return Vector2((real_t)numbers[0], (real_t)numbers[1]);
					}
					break;
				case Variant::VECTOR2I:
					if (numbers.size() == 2) {
						return Vector2i((int)numbers[0], (int)numbers[1]);
					}
					break;
				case Variant::VECTOR3:
					if (numbers.size() == 3) {
						return Vector3((real_t)numbers[0], (real_t)numbers[1], (real_t)numbers[2]);
					}
					break;
				case Variant::VECTOR3I:
					if (numbers.size() == 3) {
						return Vector3i((int)numbers[0], (int)numbers[1], (int)numbers[2]);
					}
					break;
				case Variant::VECTOR4:
					if (numbers.size() == 4) {
						return Vector4((real_t)numbers[0], (real_t)numbers[1], (real_t)numbers[2], (real_t)numbers[3]);
					}
					break;
				case Variant::COLOR:
					if (numbers.size() == 3) {
						return Color((float)numbers[0], (float)numbers[1], (float)numbers[2]);
					}
					if (numbers.size() == 4) {
						return Color((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)numbers[3]);
					}
					break;
				case Variant::RECT2:
					if (numbers.size() == 4) {
						return Rect2((real_t)numbers[0], (real_t)numbers[1], (real_t)numbers[2], (real_t)numbers[3]);
					}
					break;
				default:
					break;
			}
		}
	}

	r_ok = false;
	return Variant();
}
