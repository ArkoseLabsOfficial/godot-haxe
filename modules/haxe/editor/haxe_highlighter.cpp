/**************************************************************************/
/*  haxe_highlighter.cpp                                                  */
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

#include "haxe_highlighter.h"

#include "../haxe_language.h"
#include "../haxe_project.h"

#include "core/core_constants.h"
#include "core/object/class_db.h"
#include "core/string/char_utils.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/text_edit.h"

void HaxeSyntaxHighlighter::add_color_region(ColorRegion::Type p_type, const String &p_start_key, const String &p_end_key, const Color &p_color, bool p_line_only) {
	ColorRegion region;
	region.type = p_type;
	region.start_key = p_start_key;
	region.end_key = p_end_key;
	region.color = p_color;
	region.line_only = p_line_only;
	region.is_string = p_type == ColorRegion::TYPE_STRING;
	region.is_line_comment = p_line_only && p_end_key.is_empty();
	color_regions.push_back(region);
}

Dictionary HaxeSyntaxHighlighter::_get_line_syntax_highlighting_impl(int p_line) {
	Dictionary color_map;

	Type next_type = NONE;
	Type current_type = NONE;
	Type prev_type = NONE;

	String prev_text = "";
	int prev_column = 0;
	bool prev_is_char = false;
	bool prev_is_digit = false;
	bool prev_is_binary_op = false;

	bool in_keyword = false;
	bool in_word = false;
	bool in_number = false;
	bool in_annotation = false;
	bool in_member_variable = false;
	bool in_function_name = false;
	bool in_function_declaration = false;
	bool in_var_declaration = false;
	bool after_function_params = false;
	bool expect_type = false;
	bool is_hex_notation = false;

	int in_type_params = 0;
	int in_function_params = 0;

	Color keyword_color;
	Color color;

	color_region_cache[p_line] = -1;
	int in_region = -1;
	if (p_line != 0) {
		int prev_region_line = p_line - 1;
		while (prev_region_line > 0 && !color_region_cache.has(prev_region_line)) {
			prev_region_line--;
		}
		for (int i = prev_region_line; i < p_line - 1; i++) {
			get_line_syntax_highlighting(i);
		}
		if (!color_region_cache.has(p_line - 1)) {
			get_line_syntax_highlighting(p_line - 1);
		}
		in_region = color_region_cache[p_line - 1];
	}

	const String &str = text_edit->get_line_with_ime(p_line);
	const int line_length = str.length();
	Color prev_color;

	if (in_region != -1 && line_length == 0) {
		color_region_cache[p_line] = in_region;
	}

	// `import`, `using` and `package` lines carry a dotted type path. The
	// generic scanner below treats each dot as member access and resets the
	// type colour after it, which leaves the path mostly uncoloured, so those
	// lines get a dedicated pass.
	if (in_region == -1 && line_length > 0) {
		String stripped = str.strip_edges();
		int word_end = 0;
		while (word_end < stripped.length() && !is_symbol(stripped[word_end])) {
			word_end++;
		}
		String first_word = stripped.substr(0, word_end);
		bool is_path_line = first_word == "import" || first_word == "using" || first_word == "package";
		if (is_path_line && !str.contains("/*") && !str.contains("\"") && !str.contains("'")) {
			return _highlight_import_line(str);
		}

		// Conditional compilation: `#if`, `#elseif`, `#else`, `#end`, ... The
		// whole line is grayed out, exactly like a comment line.
		int directive_start = 0;
		while (directive_start < line_length && is_whitespace(str[directive_start])) {
			directive_start++;
		}
		if (directive_start < line_length && str[directive_start] == '#') {
			return _highlight_directive_line(directive_start);
		}
	}

	for (int j = 0; j < line_length; j++) {
		Dictionary highlighter_info;

		color = font_color;
		bool is_char = !is_symbol(str[j]);
		bool is_a_symbol = is_symbol(str[j]);
		bool is_a_digit = is_digit(str[j]);
		bool is_binary_op = false;

		/* Color regions: comments and string literals, which can span lines. */
		if (is_a_symbol || in_region != -1) {
			int from = j;

			if (in_region == -1) {
				for (; from < line_length; from++) {
					if (str[from] == '\\') {
						from++;
						continue;
					}
					break;
				}
			}

			if (from != line_length) {
				// Check whether a region starts here.
				if (in_region == -1) {
					for (int c = 0; c < color_regions.size(); c++) {
						int chars_left = line_length - from;
						int start_key_length = color_regions[c].start_key.length();
						int end_key_length = color_regions[c].end_key.length();
						if (chars_left < start_key_length) {
							continue;
						}

						bool match = true;
						const char32_t *start_key = color_regions[c].start_key.get_data();
						for (int k = 0; k < start_key_length; k++) {
							if (start_key[k] != str[from + k]) {
								match = false;
								break;
							}
						}
						if (!match) {
							continue;
						}

						in_region = c;
						from += start_key_length;

						// Check if the region covers the rest of the line.
						if (end_key_length == 0 || color_regions[c].line_only || from + end_key_length > line_length) {
							// Line comments run to the end of the line, but the
							// text is still scanned for comment markers below.
							if (color_regions[in_region].is_line_comment) {
								break;
							}
							if (from + end_key_length > line_length) {
								// Keep scanning when there is an escape or an
								// interpolation sequence to highlight.
								if (str.find_char('\\', from) >= 0 || (color_regions[in_region].is_string && str.find_char('$', from) >= 0)) {
									break;
								}
							}
							prev_color = color_regions[in_region].color;
							highlighter_info["color"] = color_regions[c].color;
							color_map[j] = highlighter_info;

							j = line_length;
							if (!color_regions[c].line_only) {
								color_region_cache[p_line] = c;
							}
						}
						break;
					}

					if (in_region != -1 && j == line_length && !color_regions[in_region].is_line_comment) {
						continue;
					}
				}

				// Already inside a region: find its end.
				if (in_region != -1) {
					Color region_color = color_regions[in_region].color;
					prev_color = region_color;
					highlighter_info["color"] = region_color;
					color_map[j] = highlighter_info;

					if (color_regions[in_region].is_line_comment) {
						from = line_length - 1;
						j = from;
					} else {
						const bool is_string = color_regions[in_region].is_string;
						int region_end_index = -1;
						int end_key_length = color_regions[in_region].end_key.length();
						const char32_t *end_key = color_regions[in_region].end_key.get_data();
						int placeholder_end = from;
						for (; from < line_length; from++) {
							if (line_length - from < end_key_length) {
								// Don't break while there is an escape or an
								// interpolation sequence left to highlight.
								if (str.find_char('\\', from) == -1 && (!is_string || str.find_char('$', from) == -1)) {
									break;
								}
							}

							if (!is_symbol(str[from]) && !(is_string && str[from] == '$')) {
								continue;
							}

							if (is_string && str[from] == '$') {
								int placeholder_start = from;
								from++;
								if (from < line_length && str[from] == '{') {
									from++;
									int depth = 1;
									while (from < line_length && depth > 0) {
										if (str[from] == '{') {
											depth++;
										} else if (str[from] == '}') {
											depth--;
										}
										from++;
									}
									placeholder_end = from;
								} else {
									while (from < line_length && (is_ascii_identifier_char(str[from]) || str[from] == '.')) {
										from++;
									}
									placeholder_end = from;
								}
								if (placeholder_end > placeholder_start) {
									Dictionary placeholder_highlighter_info;
									placeholder_highlighter_info["color"] = placeholder_color;
									color_map[placeholder_start] = placeholder_highlighter_info;
									Dictionary region_continue_highlighter_info;
									region_continue_highlighter_info["color"] = region_color;
									color_map[placeholder_end] = region_continue_highlighter_info;
								}
								from--;
								continue;
							}

							if (str[from] == '\\') {
								Dictionary escape_char_highlighter_info;
								escape_char_highlighter_info["color"] = symbol_color;
								color_map[from] = escape_char_highlighter_info;

								from++;
								if (from >= line_length) {
									break;
								}

								int esc_len = 0;
								if (str[from] == 'u') {
									esc_len = 4;
								} else if (str[from] == 'U') {
									esc_len = 6;
								}
								for (int k = 0; k < esc_len && from < line_length - 1; k++) {
									if (!is_hex_digit(str[from + 1])) {
										break;
									}
									from++;
								}

								Dictionary region_continue_highlighter_info;
								region_continue_highlighter_info["color"] = from < placeholder_end ? placeholder_color : region_color;
								color_map[from + 1] = region_continue_highlighter_info;
								continue;
							}

							region_end_index = from;
							for (int k = 0; k < end_key_length; k++) {
								if (end_key[k] != str[from + k]) {
									region_end_index = -1;
									break;
								}
							}

							if (region_end_index != -1) {
								break;
							}
						}
						j = from + (end_key_length - 1);
						if (region_end_index == -1) {
							color_region_cache[p_line] = in_region;
						}
					}

					prev_type = REGION;
					prev_text = "";
					prev_column = j;

					in_region = -1;
					prev_is_char = false;
					prev_is_digit = false;
					prev_is_binary_op = false;
					continue;
				}
				color_map.sort(); // Escape sequences must not override placeholders.
			}
		}

		// Determine whether `.`, `+`, `-` or `~` is an operator or part of a
		// member access or number literal.
		if (j > 0 && (str[j] == '.' || str[j] == '+' || str[j] == '-' || str[j] == '~')) {
			int to = j - 1;
			while (to > 0 && is_whitespace(str[to])) {
				to--;
			}
			int from = to;
			while (from > 0 && !is_symbol(str[from])) {
				from--;
			}
			String word = str.substr(from + 1, to - from);
			if (word == "true" || word == "false" || word == "null" || word == "this" || word == "super" || !reserved_keywords.has(word)) {
				if (!is_symbol(str[to]) || str[to] == '"' || str[to] == '\'' || str[to] == ')' || str[to] == ']' || str[to] == '}') {
					is_binary_op = true;
				}
			}
		}

		if (!is_char) {
			in_keyword = false;
		}

		// Allow A-F in hex literals.
		if (is_hex_notation && (is_hex_digit(str[j]) || is_a_digit)) {
			is_a_digit = true;
		} else if (str[j] != '_') {
			is_hex_notation = false;
		}

		if (!in_number && !in_word && is_a_digit) {
			in_number = true;
		}

		// Special cases for numbers.
		if (in_number && !is_a_digit) {
			if ((str[j] == 'x' || str[j] == 'X') && str[j - 1] == '0') {
				is_hex_notation = true;
			} else if (!((str[j] == '-' || str[j] == '+') && (str[j - 1] == 'e' || str[j - 1] == 'E') && !prev_is_digit) &&
					!(str[j] == '_' && (prev_is_digit || str[j - 1] == 'x' || str[j - 1] == 'X' || str[j - 1] == '.')) &&
					!((str[j] == 'e' || str[j] == 'E') && (prev_is_digit || str[j - 1] == '_')) &&
					!((str[j] == 'i' || str[j] == 'u') && (prev_is_digit || str[j - 1] == '_')) &&
					!(str[j] == '.' && (prev_is_digit || (!prev_is_binary_op && (j > 0 && (str[j - 1] == '_' || str[j - 1] == '-' || str[j - 1] == '+'))))) &&
					!((str[j] == '-' || str[j] == '+') && !is_binary_op && !prev_is_binary_op && str[j - 1] != 'e' && str[j - 1] != 'E')) {
				in_number = false;
			}
		} else if (str[j] == '.' && !is_binary_op && is_digit(str[j + 1]) && (j == 0 || (j > 0 && str[j - 1] != '.'))) {
			in_number = true;
		} else if ((str[j] == '-' || str[j] == '+' || str[j] == '~') && !is_binary_op) {
			int non_op = j + 1;
			while (non_op < line_length && (str[non_op] == '-' || str[non_op] == '+' || str[non_op] == '~')) {
				non_op++;
			}
			if (non_op < line_length && (is_digit(str[non_op]) || (str[non_op] == '.' && non_op < line_length - 1 && is_digit(str[non_op + 1])))) {
				in_number = true;
			}
		}

		if (!in_word && is_unicode_identifier_start(str[j]) && !in_number) {
			in_word = true;
		}

		if (is_a_symbol && str[j] != '.' && in_word) {
			in_word = false;
		}

		bool is_type_word = false;
		if (!in_keyword && is_char && !prev_is_char) {
			int to = j;
			while (to < line_length && !is_symbol(str[to])) {
				to++;
			}

			String word = str.substr(j, to - j);
			Color col;
			if (class_names.has(word)) {
				col = class_names[word];
				is_type_word = true;
			} else if (reserved_keywords.has(word)) {
				col = reserved_keywords[word];
				// A reserved word closes a pending type (`for (x in list)`).
				expect_type = false;
			} else if (member_keywords.has(word)) {
				col = member_keywords[word];
				in_member_variable = true;
			}

			if (col != Color()) {
				for (int k = j - 1; k >= 0; k--) {
					if (str[k] == '.') {
						// Keywords and types are not reachable through member
						// access, but don't reset the color of e.g. `5.0`.
						col = (prev_type == NUMBER) ? col : Color();
						break;
					} else if (str[k] > 32) {
						break;
					}
				}

				if (!in_member_variable && col != Color()) {
					in_keyword = true;
					keyword_color = col;
				}
			}

			if (word == "new" || word == "extends" || word == "implements") {
				expect_type = true;
				in_type_params = 0;
			}
		}

		if (!in_function_name && in_word && !in_keyword && !is_type_word) {
			if (prev_text == "function") {
				in_function_declaration = true;
			}
			int k = j;
			while (k < line_length && !is_symbol(str[k]) && !is_whitespace(str[k])) {
				k++;
			}
			while (k < line_length && is_whitespace(str[k])) {
				k++;
			}
			if (k < line_length && str[k] == '(') {
				in_function_name = true;
			} else if (prev_text == "var" || prev_text == "final") {
				in_var_declaration = true;
			}
		}

		if (!in_function_name && !in_member_variable && !in_keyword && !in_number && in_word && !is_type_word) {
			int k = j;
			while (k > 0 && !is_symbol(str[k]) && !is_whitespace(str[k])) {
				k--;
			}
			if (str[k] == '.' && (k < 1 || str[k - 1] != '.')) {
				in_member_variable = true;
			}
		}

		if (is_a_symbol) {
			if (in_function_params > 0) {
				if (str[j] == '(') {
					in_function_params++;
				} else if (str[j] == ')') {
					in_function_params--;
					if (in_function_params == 0 && in_function_declaration) {
						after_function_params = true;
					}
				}
			} else if (in_function_name && str[j] == '(') {
				in_function_params = 1;
			}

			if (str[j] == ';') {
				in_var_declaration = false;
				in_function_declaration = false;
				after_function_params = false;
				expect_type = false;
				in_type_params = 0;
			}

			if (str[j] == '{') {
				in_function_declaration = false;
				in_var_declaration = false;
			}

			if (expect_type) {
				switch (str[j]) {
					case '<':
						in_type_params++;
						break;
					case '>':
						if (in_type_params > 0) {
							in_type_params--;
						}
						break;
					case ',':
						if (in_type_params <= 0) {
							expect_type = false;
						}
						break;
					case ' ':
					case '\t':
					case '.':
					case '?':
						break;
					default:
						expect_type = false;
						break;
				}
			} else {
				if (j > 0 && str[j - 1] == '-' && str[j] == '>') {
					expect_type = true;
					in_type_params = 0;
				} else if (str[j] == ':' && (in_var_declaration || in_function_params == 1 || after_function_params)) {
					expect_type = true;
					in_type_params = 0;
					after_function_params = false;
				}
			}

			in_function_name = false;
			in_member_variable = false;

			if (!is_whitespace(str[j]) && str[j] != ')' && str[j] != ':') {
				after_function_params = false;
			}
		}

		// Metadata, e.g. `@:tool` or `@:export`.
		if (!in_annotation && in_region == -1 && str[j] == '@' && j + 1 < line_length && str[j + 1] == ':') {
			in_annotation = true;
		} else if (in_region != -1 || (is_a_symbol && !(in_annotation && str[j] == ':'))) {
			in_annotation = false;
		}

		// Conditional compilation, e.g. `#if target` or `#end`, is handled in
		// the dedicated line pass above.

		if (in_annotation) {
			next_type = ANNOTATION;
			color = annotation_color;
		} else if (in_keyword) {
			next_type = KEYWORD;
			color = keyword_color;
		} else if (in_function_name) {
			next_type = FUNCTION;
			color = in_function_declaration ? function_definition_color : function_color;
		} else if (in_number) {
			next_type = NUMBER;
			color = number_color;
		} else if (is_a_symbol) {
			next_type = SYMBOL;
			color = symbol_color;
		} else if (expect_type) {
			next_type = TYPE;
			color = type_color;
		} else if (in_member_variable) {
			next_type = MEMBER;
			color = member_variable_color;
		} else {
			next_type = IDENTIFIER;
		}

		if (next_type != current_type) {
			if (current_type == NONE) {
				current_type = next_type;
			} else {
				prev_type = current_type;
				current_type = next_type;

				// Regions are not remembered.
				if (prev_type == REGION) {
					prev_text = "";
					prev_column = j;
				} else {
					String text = str.substr(prev_column, j - prev_column).strip_edges();
					prev_column = j;

					// Ignore if just whitespace.
					if (!text.is_empty()) {
						prev_text = text;
					}
				}
			}
		}

		prev_is_char = is_char;
		prev_is_digit = is_a_digit;
		prev_is_binary_op = is_binary_op;

		if (color != prev_color) {
			prev_color = color;
			highlighter_info["color"] = color;
			color_map[j] = highlighter_info;
		}
	}
	return color_map;
}

/* Colors one `import` / `using` / `package` line.
 *
 * The line is a keyword followed by a dotted type path, optionally ending in a
 * `//` comment. Every path segment is colored as a type reference (engine and
 * project types get their own color, everything else the base type color),
 * the dots as symbols, and the keyword as a reserved word. */
Dictionary HaxeSyntaxHighlighter::_highlight_import_line(const String &p_line) {
	Dictionary color_map;
	const int length = p_line.length();
	Color last_color = font_color;

	// The colour map stores one entry at the first column of every run of the
	// same colour; the value must be a fresh Dictionary because Dictionary is
	// reference counted and would otherwise be shared between entries.
	auto emit = [&color_map, &last_color](int p_column, const Color &p_color) {
		if (p_color == last_color) {
			return;
		}
		Dictionary info;
		info["color"] = p_color;
		color_map[p_column] = info;
		last_color = p_color;
	};

	int i = 0;

	// Leading whitespace is left at the default colour.
	while (i < length && is_whitespace(p_line[i])) {
		i++;
	}

	// Leading keyword.
	int keyword_end = i;
	while (keyword_end < length && is_ascii_identifier_char(p_line[keyword_end])) {
		keyword_end++;
	}
	String keyword = p_line.substr(i, keyword_end - i);
	if (reserved_keywords.has(StringName(keyword))) {
		emit(i, reserved_keywords[StringName(keyword)]);
	}
	i = keyword_end;

	while (i < length) {
		char32_t c = p_line[i];
		if (c == '/' && i + 1 < length && p_line[i + 1] == '/') {
			emit(i, comment_color);
			break;
		}
		if (is_symbol(c)) {
			// Whitespace between path segments keeps the colour of the
			// preceding segment.
			if (!is_whitespace(c)) {
				emit(i, symbol_color);
			}
			i++;
			continue;
		}
		int segment_end = i;
		while (segment_end < length && !is_symbol(p_line[segment_end])) {
			segment_end++;
		}
		String segment = p_line.substr(i, segment_end - i);
		StringName segment_name(segment);
		Color color = class_names.has(segment_name) ? class_names[segment_name] : type_color;
		emit(i, color);
		i = segment_end;
	}

	return color_map;
}

/* Colors one conditional compilation line (`#if DEBUG`, `#end`, ...) from its
 * `#` to the end of the line, using the comment colour. */
Dictionary HaxeSyntaxHighlighter::_highlight_directive_line(int p_start) {
	Dictionary color_map;
	Dictionary info;
	info["color"] = comment_color;
	color_map[p_start] = info;
	return color_map;
}

String HaxeSyntaxHighlighter::_get_name() const {
	return "Haxe";
}

PackedStringArray HaxeSyntaxHighlighter::_get_supported_languages() const {
	PackedStringArray languages;
	languages.push_back("Haxe");
	return languages;
}

void HaxeSyntaxHighlighter::_update_cache() {
	class_names.clear();
	reserved_keywords.clear();
	member_keywords.clear();
	color_regions.clear();
	color_region_cache.clear();

	font_color = text_edit->get_theme_color(SceneStringName(font_color));
	symbol_color = EDITOR_GET("text_editor/theme/highlighting/symbol_color");
	function_color = EDITOR_GET("text_editor/theme/highlighting/function_color");
	function_definition_color = EDITOR_GET("text_editor/theme/highlighting/gdscript/function_definition_color");
	number_color = EDITOR_GET("text_editor/theme/highlighting/number_color");
	member_variable_color = EDITOR_GET("text_editor/theme/highlighting/member_variable_color");
	string_color = EDITOR_GET("text_editor/theme/highlighting/string_color");
	placeholder_color = EDITOR_GET("text_editor/theme/highlighting/string_placeholder_color");
	annotation_color = EDITOR_GET("text_editor/theme/highlighting/gdscript/annotation_color");
	type_color = EDITOR_GET("text_editor/theme/highlighting/base_type_color");

	/* Engine types. */
	const Color engine_type_color = EDITOR_GET("text_editor/theme/highlighting/engine_type_color");
	LocalVector<StringName> types;
	ClassDB::get_class_list(types);
	for (const StringName &type : types) {
		if (ClassDB::is_class_exposed(type)) {
			class_names[type] = engine_type_color;
		}
	}

	/* Global enums. */
	List<StringName> global_enums;
	CoreConstants::get_global_enums(&global_enums);
	for (const StringName &enum_name : global_enums) {
		class_names[enum_name] = engine_type_color;
	}

	/* Project types, taken from the same source the language uses. */
	const Color user_type_color = EDITOR_GET("text_editor/theme/highlighting/user_type_color");
	for (const HaxeClassInfo &info : HaxeProject::get_all_classes()) {
		if (info.is_abstract || info.name.is_empty()) {
			continue;
		}
		class_names[StringName(info.name)] = user_type_color;
	}

	/* Haxe base types and commonly used standard library types. */
	const Color base_type_color = EDITOR_GET("text_editor/theme/highlighting/base_type_color");
	for (const char *type_name : { "Int", "Float", "Bool", "String", "Void", "Dynamic", "Any", "UInt", "Null",
				 "Array", "Map", "Iterator", "Class", "Enum", "EnumValue", "StringBuf", "StringTools",
				 "Math", "Std", "Date", "EReg", "Reflect", "Type", "Int64", "UInt64", "Single", "Function" }) {
		class_names[StringName(type_name)] = base_type_color;
	}

	/* Reserved words. */
	const Color keyword_color = EDITOR_GET("text_editor/theme/highlighting/keyword_color");
	const Color control_flow_keyword_color = EDITOR_GET("text_editor/theme/highlighting/control_flow_keyword_color");
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	if (language != nullptr) {
		for (const String &keyword : language->get_reserved_words()) {
			if (language->is_control_flow_keyword(keyword)) {
				reserved_keywords[StringName(keyword)] = control_flow_keyword_color;
			} else {
				reserved_keywords[StringName(keyword)] = keyword_color;
			}
		}
	}

	/* Comments. The longer delimiters are registered first so they win over
	 * their own prefixes. */
	comment_color = EDITOR_GET("text_editor/theme/highlighting/comment_color");
	const Color doc_comment_color = EDITOR_GET("text_editor/theme/highlighting/doc_comment_color");
	add_color_region(ColorRegion::TYPE_DOC_COMMENT, "///", "", doc_comment_color, true);
	add_color_region(ColorRegion::TYPE_DOC_COMMENT, "/**", "*/", doc_comment_color);
	add_color_region(ColorRegion::TYPE_COMMENT, "//", "", comment_color, true);
	add_color_region(ColorRegion::TYPE_COMMENT, "/*", "*/", comment_color);

	/* Strings. */
	add_color_region(ColorRegion::TYPE_STRING, "\"", "\"", string_color);
	add_color_region(ColorRegion::TYPE_STRING, "'", "'", string_color);

	/* Members of the engine class the edited script extends. */
	Ref<Script> scr = _get_edited_resource();
	if (scr.is_valid()) {
		StringName instance_base = scr->get_instance_base_type();
		if (instance_base != StringName()) {
			List<PropertyInfo> property_list;
			ClassDB::get_property_list(instance_base, &property_list);
			for (const PropertyInfo &property : property_list) {
				if (property.usage & PROPERTY_USAGE_CATEGORY || property.usage & PROPERTY_USAGE_GROUP || property.usage & PROPERTY_USAGE_SUBGROUP) {
					continue;
				}
				if (String(property.name).contains_char('/')) {
					continue;
				}
				member_keywords[property.name] = member_variable_color;
			}

			List<MethodInfo> signal_list;
			ClassDB::get_signal_list(instance_base, &signal_list);
			for (const MethodInfo &signal : signal_list) {
				member_keywords[signal.name] = member_variable_color;
			}

			List<MethodInfo> method_list;
			ClassDB::get_method_list(instance_base, &method_list);
			for (const MethodInfo &method : method_list) {
				member_keywords[method.name] = member_variable_color;
			}
		}
	}
}

Ref<EditorSyntaxHighlighter> HaxeSyntaxHighlighter::_create() const {
	Ref<HaxeSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	return highlighter;
}
