/**************************************************************************/
/*  haxe_completion.cpp                                                   */
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

#include "haxe_completion.h"

#include "haxe_api_index.h"
#include "haxe_project.h"
#include "haxe_runtime.h"
#include "haxe_source.h"
#include "haxe_stdlib.h"

#include "core/io/file_access.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

namespace {

const char32_t CARET_MARKER = 0xFFFF;

bool is_ident_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') ||
			(p_char >= '0' && p_char <= '9') || p_char == '_';
}

bool is_blank(char32_t p_char) {
	return p_char == ' ' || p_char == '\t' || p_char == '\r';
}

/* One completion request: the text without the caret, where the caret is and
 * what surrounds it. */
struct CompletionContext {
	String text;
	int caret = 0;
	int line = 0;
	int column = 0;
	String prefix;
	String line_prefix;
	bool member_access = false;
	bool type_position = false;
	bool import_position = false;
	/* On an `import`/`using` line, the dotted path already typed before the
	 * identifier under the caret, including its trailing dot (for example
	 * "godot." for `import godot.No|`). Empty when nothing has been typed
	 * after the keyword yet. */
	String import_prefix;
	Vector<String> receiver;
	/* Inside a `@:` metadata annotation that is being typed. */
	bool metadata_position = false;
	bool metadata_has_colon = false;
};

String strip_marker(const String &p_code, int &r_caret, int &r_line, int &r_column) {
	int marker = p_code.find(String::chr(CARET_MARKER));
	String text = p_code;
	int caret = marker;
	if (marker < 0) {
		caret = p_code.length();
	} else {
		text = p_code.substr(0, marker) + p_code.substr(marker + 1);
	}
	r_caret = caret;
	String before = p_code.substr(0, caret);
	r_line = before.count("\n");
	int newline = before.rfind("\n");
	r_column = caret - (newline < 0 ? -1 : newline) - 1;
	return text;
}

bool analyze_code(const String &p_code, CompletionContext &r_context) {
	if (p_code.is_empty()) {
		return false;
	}
	r_context.text = strip_marker(p_code, r_context.caret, r_context.line, r_context.column);

	int prefix_start = r_context.caret;
	while (prefix_start > 0 && is_ident_char(p_code[prefix_start - 1])) {
		prefix_start--;
	}
	r_context.prefix = p_code.substr(prefix_start, r_context.caret - prefix_start);

	int newline = p_code.rfind("\n", prefix_start);
	int line_start = newline < 0 ? 0 : newline + 1;
	r_context.line_prefix = p_code.substr(line_start, prefix_start - line_start);

	// A metadata annotation being typed (`@:ex|`, or `@|`). Metadata only
	// starts with `@`, so no other construct can produce this prefix.
	String metadata_head = r_context.line_prefix.rstrip(" \t");
	if (metadata_head.ends_with("@:")) {
		r_context.metadata_position = true;
		r_context.metadata_has_colon = true;
		return true;
	}
	if (metadata_head.ends_with("@")) {
		r_context.metadata_position = true;
		return true;
	}

	// An `import` or `using` (or `package`) line is completed from package and
	// type names, not as member access, even though it contains dots.
	String line_so_far = (r_context.line_prefix + r_context.prefix).strip_edges();
	bool is_import_line = line_so_far == "import" || line_so_far.begins_with("import ") || line_so_far.begins_with("import\t");
	bool is_using_line = line_so_far == "using" || line_so_far.begins_with("using ") || line_so_far.begins_with("using\t");
	bool is_package_line = line_so_far == "package" || line_so_far.begins_with("package ") || line_so_far.begins_with("package\t");
	if (is_import_line || is_using_line || is_package_line) {
		r_context.import_position = true;
		String typed = r_context.line_prefix.strip_edges();
		int keyword_length = is_import_line ? 6 : (is_using_line ? 5 : 7);
		if (typed.length() >= keyword_length) {
			typed = typed.substr(keyword_length);
		} else {
			typed = String();
		}
		r_context.import_prefix = typed.strip_edges();
		return true;
	}

	String head = p_code.substr(0, prefix_start);
	int end = head.length();
	while (end > 0 && is_blank(head[end - 1])) {
		end--;
	}

	if (end > 0 && head[end - 1] == '.') {
		r_context.member_access = true;
		int i = end - 1;
		int guard = 0;
		while (i > 0 && guard++ < 64) {
			int j = i - 1;
			while (j >= 0 && is_blank(head[j])) {
				j--;
			}
			if (j < 0) {
				break;
			}
			if (head[j] == ')') {
				// The receiver is a call result (`get_parent()`,
				// `get_node("X")`). Capture the whole call expression as one
				// receiver segment; `resolve_receiver` looks the called
				// member's return type up, so the members of the produced
				// value complete. An expression with no named callee (a
				// parenthesized group such as `(a + b)`) stays unknown.
				int open = j;
				int paren_depth = 0;
				bool matched = false;
				while (open >= 0) {
					if (head[open] == ')') {
						paren_depth++;
					} else if (head[open] == '(') {
						paren_depth--;
						if (paren_depth == 0) {
							matched = true;
							break;
						}
					}
					open--;
				}
				if (!matched) {
					r_context.receiver.clear();
					break;
				}
				int callee_end = open - 1;
				while (callee_end >= 0 && is_blank(head[callee_end])) {
					callee_end--;
				}
				int callee_start = callee_end;
				while (callee_start >= 0 && is_ident_char(head[callee_start])) {
					callee_start--;
				}
				callee_start++;
				if (callee_start > callee_end) {
					r_context.receiver.clear();
					break;
				}
				r_context.receiver.insert(0, head.substr(callee_start, j - callee_start + 1));
				i = callee_start;
				int k = i;
				while (k > 0 && is_blank(head[k - 1])) {
					k--;
				}
				if (k > 0 && head[k - 1] == '.') {
					i = k - 1;
					continue;
				}
				break;
			}
			if (!is_ident_char(head[j])) {
				r_context.receiver.clear();
				break;
			}
			int start = j;
			while (start > 0 && is_ident_char(head[start - 1])) {
				start--;
			}
			r_context.receiver.insert(0, head.substr(start, j - start + 1));
			i = start;
			int k = i;
			while (k > 0 && is_blank(head[k - 1])) {
				k--;
			}
			if (k > 0 && head[k - 1] == '.') {
				i = k - 1;
				continue;
			}
			break;
		}
		if (r_context.receiver.is_empty()) {
			r_context.member_access = false;
		}
	} else {
		String tail = r_context.line_prefix.strip_edges();
		if (tail.ends_with(":")) {
			r_context.type_position = true;
		} else if (tail.ends_with("->") || tail.ends_with("extends") || tail.ends_with("implements")) {
			r_context.type_position = true;
		} else if (tail == "new" || tail.ends_with(" new") || tail.ends_with("\tnew") || tail.ends_with("(new")) {
			r_context.type_position = true;
		}
	}
	return true;
}

/* ------------------------------------------------------------ type model -- */

struct ResolvedType {
	String name;
	bool project = false;
	bool bridge = false;
	bool std = false;
	bool valid = false;
};

/* Strips generic arguments and the `?` nullability marker from a type
 * expression: `?Array<Int>` becomes `Array`. */
String clean_type_name(const String &p_type) {
	String name = p_type.strip_edges();
	while (name.begins_with("?")) {
		name = name.substr(1).strip_edges();
	}
	int generic = name.find("<");
	if (generic >= 0) {
		name = name.substr(0, generic).strip_edges();
	}
	while (name.ends_with("?")) {
		name = name.substr(0, name.length() - 1).strip_edges();
	}
	return name;
}

/* ------------------------------------------------------- bridge classes -- */

/* Hand written classes shipped next to the generated bindings: the global
 * scope (`godot.GD`), the binding registry (`godot.Registry`) and the hxgodot
 * runtime (`hxgodot.Native`, `hxgodot.NativeObject`, `hxgodot.VariantOps`).
 * They are not engine classes, so api.json has no record of them; parsing the
 * support files once gives completion and lookup their real members instead
 * of an empty list. */
const HashMap<String, HaxeClassInfo> &bridge_classes() {
	static HashMap<String, HaxeClassInfo> classes;
	static bool initialized = false;
	if (initialized) {
		return classes;
	}
	initialized = true;
	String stdlib = HaxeRuntime::get_singleton()->get_stdlib_dir();
	if (stdlib.is_empty()) {
		return classes;
	}
	static const char *FILES[] = {
		"godot/GD.hx",
		"godot/Registry.hx",
		"hxgodot/Native.hx",
		"hxgodot/NativeObject.hx",
		"hxgodot/VariantOps.hx",
		nullptr,
	};
	for (int i = 0; FILES[i] != nullptr; i++) {
		String path = stdlib.path_join(FILES[i]);
		Error error = OK;
		String source = FileAccess::get_file_as_string(path, &error);
		if (error != OK || source.is_empty()) {
			continue;
		}
		HaxeClassInfo info;
		if (parse_haxe_source(source, path, info) != OK || !info.valid || info.name.is_empty()) {
			continue;
		}
		classes[info.name] = info;
		if (!info.fq_name.is_empty()) {
			classes[info.fq_name] = info;
		}
	}
	return classes;
}

bool find_bridge_class(const String &p_name, HaxeClassInfo &r_info) {
	if (p_name.is_empty()) {
		return false;
	}
	const HashMap<String, HaxeClassInfo> &classes = bridge_classes();
	if (classes.has(p_name)) {
		r_info = classes[p_name];
		return true;
	}
	if (p_name.contains(".")) {
		String short_name = p_name.get_slice(".", p_name.get_slice_count(".") - 1);
		if (classes.has(short_name)) {
			r_info = classes[short_name];
			return true;
		}
	}
	return false;
}

/* Bridge classes of one package, by their short name. */
Vector<String> bridge_class_names(const String &p_package) {
	Vector<String> names;
	for (const KeyValue<String, HaxeClassInfo> &entry : bridge_classes()) {
		const HaxeClassInfo &info = entry.value;
		if (entry.key != info.name || info.package != p_package) {
			continue;
		}
		names.push_back(info.name);
	}
	names.sort();
	return names;
}

ResolvedType classify_type(const String &p_type) {
	ResolvedType result;
	String name = clean_type_name(p_type);
	if (name.is_empty()) {
		return result;
	}
	String short_name = name;
	if (short_name.contains(".")) {
		short_name = short_name.get_slice(".", short_name.get_slice_count(".") - 1);
	}
	if (short_name.is_empty()) {
		return result;
	}
	HaxeClassInfo info;
	if (HaxeProject::get_class_info_by_name(name, info) || HaxeProject::get_class_info_by_name(short_name, info)) {
		result.name = info.fq_name.is_empty() ? short_name : info.fq_name;
		result.project = true;
		result.valid = true;
		return result;
	}
	if (find_bridge_class(name, info) || find_bridge_class(short_name, info)) {
		result.name = info.fq_name.is_empty() ? info.name : info.fq_name;
		result.bridge = true;
		result.valid = true;
		return result;
	}
	/* A qualified standard library type (`haxe.io.Bytes`) is matched by its
	 * full name. A bare name only matches a top level type, so `haxe.Resource`
	 * cannot take the place of the engine's `Resource`. */
	HaxeStdLibrary *stdlib = HaxeStdLibrary::get_singleton();
	if (name.contains(".") && stdlib->has_type(name)) {
		result.name = name;
		result.std = true;
		result.valid = true;
		return result;
	}
	if (HaxeApiIndex::get_singleton()->has_class(short_name)) {
		result.name = short_name;
		result.valid = true;
		return result;
	}
	if (stdlib->has_type(short_name)) {
		result.name = short_name;
		result.std = true;
		result.valid = true;
	}
	return result;
}

/* Local variables and parameters, resolved by a light scan of the source.
 * Only declarations with an explicit type are recorded; this is deliberately
 * best effort and stays within the current file. */
void collect_declared_types(const String &p_text, HashMap<String, String> &r_types) {
	const int length = p_text.length();

	auto read_type = [&](int p_from) {
		int i = p_from;
		while (i < length && (p_text[i] == ' ' || p_text[i] == '\t')) {
			i++;
		}
		if (i < length && p_text[i] == '?') {
			i++;
			while (i < length && (p_text[i] == ' ' || p_text[i] == '\t')) {
				i++;
			}
		}
		int start = i;
		int depth = 0;
		while (i < length) {
			char32_t c = p_text[i];
			if (c == '<') {
				depth++;
			} else if (c == '>') {
				depth--;
			} else if (depth <= 0 && !(is_ident_char(c) || c == '.')) {
				break;
			}
			i++;
		}
		if (i <= start) {
			return String();
		}
		return p_text.substr(start, i - start);
	};

	auto read_name = [&](int p_from) {
		int i = p_from;
		int start = i;
		while (i < length && is_ident_char(p_text[i])) {
			i++;
		}
		if (i <= start) {
			return String();
		}
		return p_text.substr(start, i - start);
	};

	int i = 0;
	while (i < length) {
		char32_t c = p_text[i];
		// Skip comments and string literals so declarations inside them are
		// not picked up.
		if (c == '/' && i + 1 < length && p_text[i + 1] == '/') {
			while (i < length && p_text[i] != '\n') {
				i++;
			}
			continue;
		}
		if (c == '/' && i + 1 < length && p_text[i + 1] == '*') {
			i += 2;
			while (i + 1 < length && !(p_text[i] == '*' && p_text[i + 1] == '/')) {
				i++;
			}
			i += 2;
			continue;
		}
		if (c == '"' || c == '\'') {
			char32_t quote = c;
			i++;
			while (i < length && p_text[i] != quote) {
				if (p_text[i] == '\\') {
					i++;
				}
				i++;
			}
			i++;
			continue;
		}

		bool boundary = i == 0 || !is_ident_char(p_text[i - 1]);
		// `var` and `final` declare a local. The name may be separated from
		// the keyword by whitespace (`var v:Vector2`), so whitespace is
		// skipped before reading the name.
		bool is_var = boundary && c == 'v' && p_text.substr(i, 3) == "var" && (i + 3 >= length || !is_ident_char(p_text[i + 3]));
		bool is_final = boundary && c == 'f' && p_text.substr(i, 5) == "final" && (i + 5 >= length || !is_ident_char(p_text[i + 5]));
		if (is_var || is_final) {
			int after = i + (is_final ? 5 : 3);
			while (after < length && is_blank(p_text[after])) {
				after++;
			}
			if (after < length && (is_ident_char(p_text[after]) || p_text[after] == '?')) {
				if (p_text[after] == '?') {
					after++;
				}
				String name = read_name(after);
				if (!name.is_empty()) {
					int colon = after + name.length();
					while (colon < length && (p_text[colon] == ' ' || p_text[colon] == '\t')) {
						colon++;
					}
					if (colon < length && p_text[colon] == ':') {
						String type = read_type(colon + 1);
						if (!type.is_empty()) {
							r_types[name] = type;
						}
					} else if (colon < length && p_text[colon] == '=') {
						// No explicit `:Type`, but the initializer still
						// carries one for the common cases (`var n = new
						// Node()` completes Node members). The expression
						// runs to the end of the line at bracket depth zero.
						int value_start = colon + 1;
						int value_end = value_start;
						int depth = 0;
						while (value_end < length) {
							char32_t vc = p_text[value_end];
							if (vc == '(' || vc == '[' || vc == '{') {
								depth++;
							} else if (vc == ')' || vc == ']' || vc == '}') {
								if (depth == 0) {
									break;
								}
								depth--;
							} else if (depth == 0 && (vc == ';' || vc == '\n')) {
								break;
							}
							value_end++;
						}
						String type = haxe_infer_initializer_type(p_text.substr(value_start, value_end - value_start));
						if (!type.is_empty()) {
							r_types[name] = type;
						}
					}
				}
			}
			i += is_final ? 5 : 3;
			continue;
		}

		if (c == '(') {
			// A parameter list follows an identifier that is not a control flow
			// keyword.
			int j = i - 1;
			while (j >= 0 && is_blank(p_text[j])) {
				j--;
			}
			int name_end = j + 1;
			int name_start = j;
			while (name_start >= 0 && is_ident_char(p_text[name_start])) {
				name_start--;
			}
			name_start++;
			String callee = name_start <= j ? p_text.substr(name_start, name_end - name_start) : String();
			bool control = callee == "if" || callee == "while" || callee == "for" || callee == "switch" ||
					callee == "catch" || callee == "return" || callee.is_empty();
			if (!control) {
				int depth = 0;
				int k = i;
				while (k < length) {
					if (p_text[k] == '(') {
						depth++;
					} else if (p_text[k] == ')') {
						depth--;
						if (depth == 0) {
							break;
						}
					}
					k++;
				}
				if (k < length) {
					String params = p_text.substr(i + 1, k - i - 1);
					int item_start = 0;
					int item_depth = 0;
					for (int p = 0; p <= params.length(); p++) {
						bool split = p == params.length();
						if (!split) {
							char32_t pc = params[p];
							if (pc == '<' || pc == '(' || pc == '[' || pc == '{') {
								item_depth++;
							} else if (pc == '>' || pc == ')' || pc == ']' || pc == '}') {
								item_depth--;
							} else if (pc == ',' && item_depth <= 0) {
								split = true;
							}
						}
						if (!split) {
							continue;
						}
						String item = params.substr(item_start, p - item_start).strip_edges();
						item_start = p + 1;
						if (item.is_empty()) {
							continue;
						}
						int colon = item.find(":");
						if (colon <= 0) {
							continue;
						}
						String parameter = item.substr(0, colon).strip_edges();
						while (parameter.contains(" ")) {
							parameter = parameter.get_slice(" ", parameter.get_slice_count(" ") - 1).strip_edges();
						}
						if (parameter.is_empty() || !is_ident_char(parameter[0])) {
							continue;
						}
						String type = item.substr(colon + 1).strip_edges();
						if (!type.is_empty()) {
							r_types[parameter] = type;
						}
					}
				}
			}
			i++;
			continue;
		}
		i++;
	}
}

/* Member type of a resolved owner, following project scripts into the engine
 * bindings. */
ResolvedType resolve_member_type(const ResolvedType &p_owner, const String &p_member, bool p_static) {
	ResolvedType result;
	if (!p_owner.valid) {
		return result;
	}
	if (p_owner.project || p_owner.bridge || p_owner.std) {
		HaxeClassInfo info;
		bool found = false;
		if (p_owner.project) {
			found = HaxeProject::get_class_info_by_name(p_owner.name, info);
		} else if (p_owner.bridge) {
			found = find_bridge_class(p_owner.name, info);
		} else {
			found = HaxeStdLibrary::get_singleton()->get_type_info(p_owner.name, info);
		}
		if (!found) {
			return result;
		}
		const HaxeMemberInfo *member = info.find_variable(p_member);
		if (member != nullptr) {
			return classify_type(member->type);
		}
		const HaxeFunctionInfo *function = info.find_function(p_member);
		if (function != nullptr) {
			return classify_type(function->return_type);
		}
		if (!info.base.is_empty()) {
			return resolve_member_type(classify_type(info.base), p_member, p_static);
		}
		return result;
	}
	Dictionary member;
	String owner;
	if (!HaxeApiIndex::get_singleton()->find_member(p_owner.name, p_member, member, owner)) {
		return result;
	}
	return classify_type(member.get("type", String()));
}

/* Joins the first `p_count` segments of a receiver with dots. */
String join_receiver(const Vector<String> &p_receiver, int p_count) {
	String joined;
	for (int i = 0; i < p_count; i++) {
		if (i > 0) {
			joined += ".";
		}
		joined += p_receiver[i];
	}
	return joined;
}

/* The member name of a receiver segment. A plain segment is already a name;
 * a call segment (`get_parent()`, `Vector2(1, 2)`) names the function or
 * constructor before its argument list. */
String receiver_segment_name(const String &p_segment) {
	int paren = p_segment.find("(");
	return paren < 0 ? p_segment : p_segment.substr(0, paren).strip_edges();
}

/* Resolves the receiver of a member access. Returns an invalid type when the
 * receiver is not understood.
 *
 * A receiver may start with a qualified type name (`godot.Node2D`,
 * `game.Main`) instead of a single identifier, so every dotted prefix is tried
 * from the longest down and the first one that names a type wins. The
 * remaining segments are member accesses. */
ResolvedType resolve_receiver(const CompletionContext &p_context, const String &p_script_path, HashMap<String, String> &r_locals, bool &r_is_static) {
	ResolvedType result;
	r_is_static = false;
	if (p_context.receiver.is_empty()) {
		return result;
	}
	collect_declared_types(p_context.text, r_locals);

	HaxeClassInfo script_info;
	bool has_script = !p_script_path.is_empty() && HaxeProject::get_class_info(p_script_path, script_info) && script_info.valid;

	String first = p_context.receiver[0];
	int index = 1;
	if (first == "this" || first == "self") {
		if (!has_script) {
			return result;
		}
		result.name = script_info.fq_name.is_empty() ? script_info.name : script_info.fq_name;
		result.project = true;
		result.valid = true;
	} else if (first == "super") {
		/* `super` is the base class instance, so its instance members (the
		 * engine virtuals such as `_process`, and everything inherited) are
		 * reachable through it. */
		if (!has_script) {
			return result;
		}
		result = classify_type(script_info.base);
		if (!result.valid) {
			return result;
		}
	} else if (r_locals.has(first)) {
		result = classify_type(r_locals[first]);
	} else if (first.contains("(")) {
		/* A call result at the head of the receiver (`get_parent().add_child`):
		 * the value is whatever the called member returns, and the call has to
		 * read as an implicit-`this` member of the edited class. */
		String callee = receiver_segment_name(first);
		if (has_script) {
			ResolvedType self_type;
			self_type.name = script_info.fq_name.is_empty() ? script_info.name : script_info.fq_name;
			self_type.project = true;
			self_type.valid = true;
			result = resolve_member_type(self_type, callee, false);
		}
		if (!result.valid) {
			// A constructor call (`Vector2(1, 2).x`) names a type directly
			// instead of a member of the edited class.
			result = classify_type(callee);
		}
		if (!result.valid) {
			return result;
		}
	} else {
		for (int length = p_context.receiver.size(); length >= 1; length--) {
			ResolvedType candidate = classify_type(join_receiver(p_context.receiver, length));
			if (candidate.valid) {
				result = candidate;
				index = length;
				/* Naming a type means the receiver is a class reference, not
				 * an instance. */
				r_is_static = true;
				break;
			}
		}
		/* Implicit `this`: `position.x` inside a method means
		 * `this.position.x`. Only tried when the first segment is not a type
		 * or a local, so an explicit name always wins. */
		if (!result.valid && has_script) {
			ResolvedType self_type;
			self_type.name = script_info.fq_name.is_empty() ? script_info.name : script_info.fq_name;
			self_type.project = true;
			self_type.valid = true;
			result = resolve_member_type(self_type, first, false);
		}
		if (!result.valid) {
			return result;
		}
	}
	for (int i = index; i < p_context.receiver.size(); i++) {
		// A call segment (`get_parent()`) resolves through the called member's
		// return type, so members of a call result complete.
		result = resolve_member_type(result, receiver_segment_name(p_context.receiver[i]), r_is_static);
		if (!result.valid) {
			return result;
		}
		/* Whatever the member is, the result is a value, not a type. */
		r_is_static = false;
	}
	return result;
}

/* -------------------------------------------------------------- listings -- */

bool prefix_matches(const String &p_text, const String &p_prefix) {
	if (p_prefix.is_empty()) {
		return true;
	}
	return p_text.begins_with(p_prefix) || p_text.to_lower().begins_with(p_prefix.to_lower());
}

void add_option(List<ScriptLanguage::CodeCompletionOption> *r_options, const String &p_display, const String &p_insert, ScriptLanguage::CodeCompletionKind p_kind, int p_location) {
	ScriptLanguage::CodeCompletionOption option(p_display, p_kind, p_location);
	option.insert_text = p_insert;
	r_options->push_back(option);
}

void add_project_members(const CompletionContext &p_context, const HaxeClassInfo &p_info, bool p_include_static, bool p_include_instance, HashSet<String> &r_seen, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	/* Every declared variable is offered, not only the `@:export` ones: the
	 * inspector needs exports, but completion should list plain fields too. */
	for (const HaxeMemberInfo &member : p_info.variables) {
		if (member.is_static ? !p_include_static : !p_include_instance) {
			continue;
		}
		if (r_seen.has(member.name) || !prefix_matches(member.name, p_context.prefix)) {
			continue;
		}
		r_seen.insert(member.name);
		add_option(r_options, member.name, member.name, ScriptLanguage::CODE_COMPLETION_KIND_VARIABLE, ScriptLanguage::LOCATION_LOCAL);
	}
	for (const HaxeFunctionInfo &function : p_info.functions) {
		if (function.is_static ? !p_include_static : !p_include_instance) {
			continue;
		}
		if (r_seen.has(function.name) || !prefix_matches(function.name, p_context.prefix)) {
			continue;
		}
		r_seen.insert(function.name);
		add_option(r_options, function.name, function.name + "(", ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION, ScriptLanguage::LOCATION_LOCAL);
	}
}

void add_engine_members(const CompletionContext &p_context, const String &p_class, bool p_include_static, bool p_include_instance, HashSet<String> &r_seen, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	String current = p_class;
	int guard = 0;
	HaxeApiIndex *index = HaxeApiIndex::get_singleton();
	while (!current.is_empty() && guard++ < 64) {
		Array members = index->get_declared_members(current);
		for (const Variant &item : members) {
			Dictionary member = item;
			String name = member.get("name", String());
			String kind = member.get("kind", String());
			if (name.is_empty() || r_seen.has(name) || !prefix_matches(name, p_context.prefix)) {
				continue;
			}
			bool is_static = Variant(member.get("static", false)).operator bool();
			bool include = false;
			ScriptLanguage::CodeCompletionKind completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT;
			String insert = name;
			if (is_static) {
				if (!p_include_static) {
					continue;
				}
				if (kind == "static_method") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION;
					insert = name + "(";
				} else if (kind == "constant" || kind == "enum_value") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_CONSTANT;
				} else if (kind == "enum") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_ENUM;
				}
			} else {
				if (!p_include_instance) {
					continue;
				}
				if (kind == "property") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_MEMBER;
				} else if (kind == "method") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION;
					insert = name + "(";
				} else if (kind == "signal") {
					include = true;
					completion_kind = ScriptLanguage::CODE_COMPLETION_KIND_SIGNAL;
				}
			}
			if (!include) {
				continue;
			}
			r_seen.insert(name);
			add_option(r_options, name, insert, completion_kind, ScriptLanguage::LOCATION_OTHER);
		}
		current = index->get_base(current);
	}
}

void add_member_options(const CompletionContext &p_context, const ResolvedType &p_owner, bool p_include_static, bool p_include_instance, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	HashSet<String> seen;
	String current = p_owner.name;
	bool project = p_owner.project;
	bool std = p_owner.std;
	int guard = 0;
	while (!current.is_empty() && guard++ < 64) {
		if (project) {
			HaxeClassInfo info;
			if (!HaxeProject::get_class_info_by_name(current, info)) {
				break;
			}
			add_project_members(p_context, info, p_include_static, p_include_instance, seen, r_options);
			ResolvedType parent = classify_type(info.base);
			if (!parent.valid) {
				break;
			}
			current = parent.name;
			project = parent.project;
			std = parent.std;
			continue;
		}
		if (std) {
			HaxeClassInfo info;
			if (!HaxeStdLibrary::get_singleton()->get_type_info(current, info)) {
				break;
			}
			add_project_members(p_context, info, p_include_static, p_include_instance, seen, r_options);
			ResolvedType parent = classify_type(info.base);
			if (!parent.valid) {
				break;
			}
			current = parent.name;
			project = parent.project;
			std = parent.std;
			continue;
		}
		HaxeClassInfo bridge;
		if (find_bridge_class(current, bridge)) {
			add_project_members(p_context, bridge, p_include_static, p_include_instance, seen, r_options);
			ResolvedType parent = classify_type(bridge.base);
			if (!parent.valid) {
				break;
			}
			current = parent.name;
			project = parent.project;
			std = parent.std;
			continue;
		}
		add_engine_members(p_context, current, p_include_static, p_include_instance, seen, r_options);
		break;
	}
}

void add_class_options(const CompletionContext &p_context, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	HashSet<String> seen;
	HaxeApiIndex *index = HaxeApiIndex::get_singleton();
	for (const String &name : index->get_class_names()) {
		if (!prefix_matches(name, p_context.prefix)) {
			continue;
		}
		seen.insert(name);
		add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
	}
	for (const HaxeClassInfo &info : HaxeProject::get_all_classes()) {
		String name = info.fq_name.is_empty() ? info.name : info.fq_name;
		if (name.is_empty() || seen.has(name) || !prefix_matches(name, p_context.prefix)) {
			continue;
		}
		add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER_USER_CODE);
	}
	for (const String &name : index->get_global_enum_names()) {
		if (!prefix_matches(name, p_context.prefix)) {
			continue;
		}
		add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_ENUM, ScriptLanguage::LOCATION_OTHER);
	}
	for (const KeyValue<String, HaxeClassInfo> &entry : bridge_classes()) {
		if (entry.key != entry.value.name || seen.has(entry.key) || !prefix_matches(entry.key, p_context.prefix)) {
			continue;
		}
		seen.insert(entry.key);
		add_option(r_options, entry.key, entry.key, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
	}
	/* The Haxe standard library: top level names by their short name
	 * (`Array`, `String`, `StringTools`, ...) and the rest by their full name
	 * (`haxe.io.Bytes`). */
	HaxeStdLibrary *stdlib = HaxeStdLibrary::get_singleton();
	if (stdlib->ensure_indexed()) {
		for (const String &name : stdlib->get_type_names()) {
			if (seen.has(name) || !prefix_matches(name, p_context.prefix)) {
				continue;
			}
			seen.insert(name);
			add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
		}
	}
}

void add_import_options(const CompletionContext &p_context, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	HaxeApiIndex *index = HaxeApiIndex::get_singleton();
	// The dotted path typed before the identifier under the caret, with its
	// trailing dot removed ("godot" for `import godot.No|`).
	String package = p_context.import_prefix;
	if (package.ends_with(".")) {
		package = package.substr(0, package.length() - 1);
	}

	if (package.is_empty()) {
		// Nothing after the keyword yet: offer the top level packages and any
		// project class that has no package.
		HashSet<String> seen;
		if (prefix_matches("godot", p_context.prefix)) {
			seen.insert("godot");
			add_option(r_options, "godot", "godot", ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER);
		}
		if (prefix_matches("hxgodot", p_context.prefix)) {
			seen.insert("hxgodot");
			add_option(r_options, "hxgodot", "hxgodot", ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER);
		}
		for (const HaxeClassInfo &info : HaxeProject::get_all_classes()) {
			String name = info.fq_name.is_empty() ? info.name : info.fq_name;
			if (name.is_empty()) {
				continue;
			}
			String root = name.contains(".") ? name.get_slice(".", 0) : name;
			if (seen.has(root) || !prefix_matches(root, p_context.prefix)) {
				continue;
			}
			seen.insert(root);
			add_option(r_options, root, root, ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER_USER_CODE);
		}
		// Top level standard library packages (`haxe`, `sys`).
		HaxeStdLibrary *stdlib = HaxeStdLibrary::get_singleton();
		if (stdlib->ensure_indexed()) {
			for (const String &root : stdlib->get_subpackages(String())) {
				if (seen.has(root) || !prefix_matches(root, p_context.prefix)) {
					continue;
				}
				seen.insert(root);
				add_option(r_options, root, root, ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER);
			}
		}
		return;
	}

	if (package == "godot") {
		for (const String &name : index->get_class_names()) {
			if (prefix_matches(name, p_context.prefix)) {
				add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
			}
		}
		// The global scope (`GD`) and the binding registry are hand written
		// classes that api.json does not describe.
		for (const String &name : bridge_class_names("godot")) {
			if (prefix_matches(name, p_context.prefix)) {
				add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
			}
		}
		return;
	}

	if (package == "godot.GD") {
		for (const String &name : index->get_global_enum_names()) {
			if (prefix_matches(name, p_context.prefix)) {
				add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_ENUM, ScriptLanguage::LOCATION_OTHER);
			}
		}
		return;
	}

	if (package == "hxgodot") {
		for (const String &name : bridge_class_names("hxgodot")) {
			if (prefix_matches(name, p_context.prefix)) {
				add_option(r_options, name, name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
			}
		}
		return;
	}

	// A project (or library) package: offer the types declared in it.
	HashSet<String> seen;
	for (const HaxeClassInfo &info : HaxeProject::get_all_classes()) {
		String name = info.fq_name.is_empty() ? info.name : info.fq_name;
		int separator = name.rfind(".");
		if (separator > 0 && name.substr(0, separator) == package) {
			String short_name = name.substr(separator + 1);
			if (!seen.has(short_name) && prefix_matches(short_name, p_context.prefix)) {
				seen.insert(short_name);
				add_option(r_options, short_name, short_name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER_USER_CODE);
			}
		}

		// Secondary types declared in the same file are imported as
		// `import <package>.<FileName>.<Type>`.
		if (name == package) {
			for (const String &secondary : info.secondary_classes) {
				if (!seen.has(secondary) && prefix_matches(secondary, p_context.prefix)) {
					seen.insert(secondary);
					add_option(r_options, secondary, secondary, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER_USER_CODE);
				}
			}
		}

		// Sub-packages of the typed path.
		if (name.begins_with(package + ".")) {
			String remainder = name.substr(package.length() + 1);
			if (remainder.contains(".")) {
				String sub_package = remainder.get_slice(".", 0);
				if (!sub_package.is_empty() && !seen.has(sub_package) && prefix_matches(sub_package, p_context.prefix)) {
					seen.insert(sub_package);
					add_option(r_options, sub_package, sub_package, ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER_USER_CODE);
				}
			}
		}
	}

	// Standard library types and packages at the typed path (`import haxe.`).
	HaxeStdLibrary *stdlib = HaxeStdLibrary::get_singleton();
	if (stdlib->ensure_indexed()) {
		for (const String &short_name : stdlib->get_package_types(package)) {
			if (seen.has(short_name) || !prefix_matches(short_name, p_context.prefix)) {
				continue;
			}
			seen.insert(short_name);
			add_option(r_options, short_name, short_name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_OTHER);
		}
		for (const String &sub_package : stdlib->get_subpackages(package)) {
			if (seen.has(sub_package) || !prefix_matches(sub_package, p_context.prefix)) {
				continue;
			}
			seen.insert(sub_package);
			add_option(r_options, sub_package, sub_package, ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER);
		}
	}
}

void add_keyword_options(const CompletionContext &p_context, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	static const char *KEYWORDS[] = {
		"class", "function", "var", "final", "static", "public", "private", "override", "inline",
		"dynamic", "abstract", "enum", "typedef", "interface", "extends", "implements", "new",
		"return", "if", "else", "for", "while", "do", "switch", "case", "default", "break",
		"continue", "try", "catch", "throw", "this", "super", "null", "true", "false", "import",
		"package", "using", "cast", "is", "in", "extern", "macro", "operator", "untyped", "never",
		nullptr
	};
	for (int i = 0; KEYWORDS[i] != nullptr; i++) {
		String keyword = KEYWORDS[i];
		if (!prefix_matches(keyword, p_context.prefix)) {
			continue;
		}
		add_option(r_options, keyword, keyword, ScriptLanguage::CODE_COMPLETION_KIND_KEYWORD, ScriptLanguage::LOCATION_OTHER);
	}
}

/* The annotations the Haxe module interprets. Keep this list in sync with the
 * checks in haxe_source.cpp and with HAXE_LANGUAGE.md. */
void add_metadata_options(const CompletionContext &p_context, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	static const char *ANNOTATIONS[] = {
		"abstract",
		"editor",
		"export", "exported",
		"exportDir", "exportEnum", "exportFile", "exportGroup",
		"exportMultiline", "exportRange", "exportSubgroup",
		"gdGlobalClass", "gdSignal", "gdTool",
		"globalClass",
		"signal",
		"static",
		"tool",
		nullptr
	};
	for (int i = 0; ANNOTATIONS[i] != nullptr; i++) {
		String name = ANNOTATIONS[i];
		if (!prefix_matches(name, p_context.prefix)) {
			continue;
		}
		// The `@:` already typed stays in place; after a bare `@` the whole
		// annotation is inserted.
		String display = p_context.metadata_has_colon ? name : "@:" + name;
		String insert = p_context.metadata_has_colon ? name : ":" + name;
		add_option(r_options, display, insert, ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguage::LOCATION_OTHER);
	}
}

void add_scope_options(const CompletionContext &p_context, const String &p_script_path, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	// Members of the class the caret is in are directly accessible, including
	// the ones inherited from project base classes.
	HaxeClassInfo info;
	if (!p_script_path.is_empty() && HaxeProject::get_class_info(p_script_path, info) && info.valid) {
		HashSet<String> seen;
		String current = info.fq_name.is_empty() ? info.name : info.fq_name;
		int guard = 0;
		while (!current.is_empty() && guard++ < 64) {
			HaxeClassInfo current_info;
			if (!HaxeProject::get_class_info_by_name(current, current_info)) {
				break;
			}
			add_project_members(p_context, current_info, true, true, seen, r_options);
			if (current_info.base.is_empty()) {
				break;
			}
			ResolvedType parent = classify_type(current_info.base);
			if (!parent.valid) {
				break;
			}
			if (!parent.project) {
				add_engine_members(p_context, parent.name, true, true, seen, r_options);
				break;
			}
			current = parent.name;
		}
		// Types declared next to the edited class are directly visible here:
		// a file with `class Main { ... } class Helper { ... }` can use
		// `Helper` by name from `Main`.
		for (const HaxeSecondaryTypeInfo &type : info.secondary_types) {
			if (prefix_matches(type.name, p_context.prefix)) {
				add_option(r_options, type.name, type.name, ScriptLanguage::CODE_COMPLETION_KIND_CLASS, ScriptLanguage::LOCATION_LOCAL);
			}
		}
	}
	// Locals and parameters.
	HashMap<String, String> locals;
	collect_declared_types(p_context.text, locals);
	for (const KeyValue<String, String> &entry : locals) {
		if (!prefix_matches(entry.key, p_context.prefix)) {
			continue;
		}
		add_option(r_options, entry.key, entry.key, ScriptLanguage::CODE_COMPLETION_KIND_VARIABLE, ScriptLanguage::LOCATION_LOCAL);
	}
}

/* ------------------------------------------------------- lookup plumbing -- */

ScriptLanguage::LookupResultType lookup_kind(const String &p_kind) {
	if (p_kind == "property") {
		return ScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY;
	}
	if (p_kind == "method" || p_kind == "static_method") {
		return ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD;
	}
	if (p_kind == "signal") {
		return ScriptLanguage::LOOKUP_RESULT_CLASS_SIGNAL;
	}
	if (p_kind == "constant") {
		return ScriptLanguage::LOOKUP_RESULT_CLASS_CONSTANT;
	}
	if (p_kind == "enum" || p_kind == "enum_value") {
		return ScriptLanguage::LOOKUP_RESULT_CLASS_ENUM;
	}
	return ScriptLanguage::LOOKUP_RESULT_CLASS;
}

} // namespace

String haxe_resolve_member_type(const Vector<String> &p_path, const String &p_script_path) {
	if (p_path.is_empty()) {
		return String();
	}
	CompletionContext context;
	context.receiver = p_path;
	HashMap<String, String> locals;
	bool is_static = false;
	ResolvedType type = resolve_receiver(context, p_script_path, locals, is_static);
	return type.valid ? type.name : String();
}

Vector<String> haxe_split_expression(const String &p_expression) {
	Vector<String> segments;
	for (const String &part : p_expression.split(".")) {
		String segment = part.strip_edges();
		if (!segment.is_empty()) {
			segments.push_back(segment);
		}
	}
	return segments;
}

Error haxe_complete_code(const String &p_code, const String &p_path, Object *p_owner, List<ScriptLanguage::CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) {
	(void)p_owner;
	r_force = false;
	r_call_hint = String();
	ERR_FAIL_NULL_V(r_options, ERR_INVALID_PARAMETER);

	CompletionContext context;
	if (!analyze_code(p_code, context)) {
		return ERR_UNAVAILABLE;
	}

	if (context.metadata_position) {
		add_metadata_options(context, r_options);
		r_force = !r_options->is_empty();
		return OK;
	}

	if (context.member_access) {
		HashMap<String, String> locals;
		bool is_static = false;
		ResolvedType owner = resolve_receiver(context, p_path, locals, is_static);
		if (owner.valid) {
			/* A class reference (`godot.Object.`, `MyClass.`) only gets the
			 * members Haxe allows there: statics, constants and enum values.
			 * Instance members, including engine methods such as `call()` or
			 * `callv()`, only make sense after an instance. */
			add_member_options(context, owner, is_static, !is_static, r_options);
		}
		if (r_options->is_empty()) {
			// Unknown receiver: offering the class list would be wrong.
			return OK;
		}
		r_force = true;
		return OK;
	}

	if (context.import_position) {
		add_import_options(context, r_options);
		r_force = !r_options->is_empty();
		return OK;
	}

	if (context.type_position) {
		add_class_options(context, r_options);
		r_force = !r_options->is_empty();
		return OK;
	}

	add_scope_options(context, p_path, r_options);
	add_class_options(context, r_options);
	add_keyword_options(context, r_options);
	r_force = !r_options->is_empty();
	return OK;
}

Error haxe_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, ScriptLanguage::LookupResult &r_result) {
	(void)p_owner;
	if (p_symbol.is_empty()) {
		return ERR_UNAVAILABLE;
	}

	HaxeClassInfo script_info;
	bool has_script = !p_path.is_empty() && HaxeProject::get_class_info(p_path, script_info) && script_info.valid;

	// Members declared by the script being edited.
	if (has_script) {
		const HaxeMemberInfo *member = script_info.find_variable(p_symbol);
		if (member != nullptr) {
			r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY;
			r_result.class_name = script_info.fq_name.is_empty() ? script_info.name : script_info.fq_name;
			r_result.class_member = p_symbol;
			r_result.script_path = p_path;
			r_result.location = member->line + 1;
			return OK;
		}
		const HaxeFunctionInfo *function = script_info.find_function(p_symbol);
		if (function != nullptr) {
			r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD;
			r_result.class_name = script_info.fq_name.is_empty() ? script_info.name : script_info.fq_name;
			r_result.class_member = p_symbol;
			r_result.script_path = p_path;
			r_result.location = function->line + 1;
			return OK;
		}
	}

	// A project class.
	HaxeClassInfo project_class;
	if (HaxeProject::get_class_info_by_name(p_symbol, project_class)) {
		r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS;
		r_result.class_name = project_class.fq_name.is_empty() ? project_class.name : project_class.fq_name;
		r_result.script_path = project_class.path;
		r_result.location = project_class.class_line + 1;
		return OK;
	}

	// A bridge class (`GD`, `hxgodot.Native`, ...), which declares real
	// members in the support files.
	HaxeClassInfo bridge_class;
	if (find_bridge_class(p_symbol, bridge_class)) {
		r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS;
		r_result.class_name = bridge_class.fq_name.is_empty() ? bridge_class.name : bridge_class.fq_name;
		r_result.script_path = bridge_class.path;
		r_result.location = bridge_class.class_line + 1;
		return OK;
	}

	// A Haxe standard library type.
	HaxeClassInfo std_class;
	if (HaxeStdLibrary::get_singleton()->get_type_info(p_symbol, std_class)) {
		r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS;
		r_result.class_name = std_class.fq_name.is_empty() ? std_class.name : std_class.fq_name;
		r_result.script_path = std_class.path;
		r_result.location = std_class.class_line + 1;
		return OK;
	}

	// An engine class, global enum constant or singleton.
	HaxeApiIndex *index = HaxeApiIndex::get_singleton();
	if (index->has_class(p_symbol)) {
		r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS;
		r_result.class_name = p_symbol;
		return OK;
	}
	for (const String &enum_name : index->get_global_enum_names()) {
		if (enum_name != p_symbol) {
			continue;
		}
		r_result.type = ScriptLanguage::LOOKUP_RESULT_CLASS_ENUM;
		r_result.class_name = p_symbol;
		r_result.enumeration = p_symbol;
		return OK;
	}

	// A member reached through the receiver under the caret (`self.position.x`).
	CompletionContext context;
	if (analyze_code(p_code, context) && context.member_access) {
		HashMap<String, String> locals;
		bool is_static = false;
		ResolvedType owner = resolve_receiver(context, p_path, locals, is_static);
		// The caret is after the final dot, so the receiver stops at the
		// previous segment and `p_symbol` is the member being looked up.
		if (owner.valid) {
			if (owner.project || owner.bridge || owner.std) {
				HaxeClassInfo info;
				bool found = owner.project ? HaxeProject::get_class_info_by_name(owner.name, info)
						: owner.bridge	   ? find_bridge_class(owner.name, info)
										   : HaxeStdLibrary::get_singleton()->get_type_info(owner.name, info);
				if (found) {
					const HaxeMemberInfo *member = info.find_variable(p_symbol);
					const HaxeFunctionInfo *function = info.find_function(p_symbol);
					if (member != nullptr || function != nullptr) {
						r_result.type = member != nullptr ? ScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY : ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD;
						r_result.class_name = owner.name;
						r_result.class_member = p_symbol;
						r_result.script_path = info.path;
						r_result.location = (member != nullptr ? member->line : function->line) + 1;
						return OK;
					}
				}
			}
			Dictionary member;
			String member_owner;
			if (index->find_member(owner.name, p_symbol, member, member_owner)) {
				r_result.type = lookup_kind(member.get("kind", String()));
				r_result.class_name = member_owner;
				r_result.class_member = p_symbol;
				return OK;
			}
		}
	}

	// A member inherited from the engine base class of this script.
	if (has_script) {
		String base = HaxeProject::resolve_native_base(script_info);
		if (!base.is_empty()) {
			Dictionary member;
			String owner;
			if (index->find_member(base, p_symbol, member, owner)) {
				r_result.type = lookup_kind(member.get("kind", String()));
				r_result.class_name = owner;
				r_result.class_member = p_symbol;
				return OK;
			}
		}
	}

	// A local variable or parameter.
	HashMap<String, String> locals;
	collect_declared_types(analyze_code(p_code, context) ? context.text : p_code, locals);
	if (locals.has(p_symbol)) {
		r_result.type = ScriptLanguage::LOOKUP_RESULT_LOCAL_VARIABLE;
		r_result.description = vformat("var %s: %s", p_symbol, locals[p_symbol]);
		return OK;
	}

	return ERR_UNAVAILABLE;
}
