/**************************************************************************/
/*  haxe_gdscript.cpp                                                     */
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

#include "haxe_gdscript.h"

#include "haxe_api_index.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/script_language.h"
#include "core/templates/local_vector.h"

HaxeGDScriptLibrary *HaxeGDScriptLibrary::singleton = nullptr;

namespace {

bool gd_is_ident_start(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || p_char == '_';
}

bool gd_is_ident_char(char32_t p_char) {
	return gd_is_ident_start(p_char) || (p_char >= '0' && p_char <= '9');
}

/* Removes a trailing `#` comment, leaving `#` inside a string literal alone. */
String strip_gdscript_comment(const String &p_line) {
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_line.length(); i++) {
		char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '#') {
			return p_line.substr(0, i);
		}
	}
	return p_line;
}

int leading_indent(const String &p_line) {
	int i = 0;
	while (i < p_line.length() && (p_line[i] == ' ' || p_line[i] == '\t')) {
		i++;
	}
	return i;
}

/* Splits at top level separators (commas or `=`), ignoring the ones nested in
 * parentheses, brackets, braces or string literals. */
Vector<String> split_top_level(const String &p_text, char32_t p_separator) {
	Vector<String> parts;
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	int start = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth--;
		} else if (c == p_separator && depth == 0) {
			parts.push_back(p_text.substr(start, i - start));
			start = i + 1;
		}
	}
	parts.push_back(p_text.substr(start));
	return parts;
}

/* The index of the top level `=` of a default value, or -1. Used with `:=`,
 * where the type is inferred from the default. */
int find_default_separator(const String &p_text) {
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			depth--;
			continue;
		}
		if (c != '=' || depth != 0) {
			continue;
		}
		char32_t previous = i > 0 ? p_text[i - 1] : 0;
		char32_t next = i + 1 < p_text.length() ? p_text[i + 1] : 0;
		if (previous == '=' || previous == '!' || previous == '<' || previous == '>' || next == '=') {
			continue;
		}
		return i;
	}
	return -1;
}

/* Maps a GDScript type to the Haxe type the generated stub (and completion)
 * uses. Engine classes are qualified (`godot.Node2D`); a name that is not an
 * engine class is a project class and stays bare, since the stub for it lives
 * in the top level package and Haxe makes those visible everywhere. */
String gdscript_type_to_haxe(const String &p_type) {
	String type = p_type.strip_edges();
	if (type.is_empty()) {
		return "Dynamic";
	}
	if (type.contains("|")) {
		// Union types have no single Haxe type; keep the value dynamic.
		return "Dynamic";
	}
	int bracket = type.find("[");
	if (bracket > 0) {
		String head = type.substr(0, bracket).strip_edges();
		int close = type.rfind("]");
		String inner = close > bracket ? type.substr(bracket + 1, close - bracket - 1) : String();
		if (head == "Array") {
			String element = inner.strip_edges();
			return element.is_empty() ? String("Array<Dynamic>") : "Array<" + gdscript_type_to_haxe(element) + ">";
		}
		if (head == "Dictionary") {
			return "godot.Dictionary";
		}
		return gdscript_type_to_haxe(head);
	}
	if (type == "void") {
		return "Void";
	}
	if (type == "bool") {
		return "Bool";
	}
	if (type == "int") {
		return "Int";
	}
	if (type == "float") {
		return "Float";
	}
	if (type == "String") {
		return "String";
	}
	if (type == "StringName" || type == "NodePath") {
		// The generated bindings map both to Haxe `String`.
		return "String";
	}
	if (type == "Variant" || type == "Nil") {
		return "Dynamic";
	}
	if (type == "Array") {
		return "Array<Dynamic>";
	}
	if (type == "Dictionary") {
		return "godot.Dictionary";
	}
	if (type == "Callable") {
		return "godot.Callable";
	}
	if (type == "Signal") {
		return "godot.Signal";
	}
	if (HaxeApiIndex::get_singleton()->ensure_loaded() && HaxeApiIndex::get_singleton()->has_class(type)) {
		return "godot." + type;
	}
	if (!gd_is_ident_start(type[0])) {
		return "Dynamic";
	}
	return type;
}

/* Best effort type of a GDScript default value, used for `var x := value`. */
String infer_gdscript_literal_type(const String &p_value) {
	String value = p_value.strip_edges();
	if (value.is_empty()) {
		return "Dynamic";
	}
	if (value == "true" || value == "false") {
		return "Bool";
	}
	if (value == "null") {
		return "Dynamic";
	}
	if (value.begins_with("\"") || value.begins_with("\'")) {
		return "String";
	}
	if (value.begins_with("[") || value.begins_with("{")) {
		return "Dynamic";
	}
	if (value == "Vector2" || value.begins_with("Vector2(")) {
		return "godot.Vector2";
	}
	if (value.begins_with("Vector2i(")) {
		return "godot.Vector2i";
	}
	if (value.begins_with("Vector3(")) {
		return "godot.Vector3";
	}
	if (value.begins_with("Vector3i(")) {
		return "godot.Vector3i";
	}
	if (value.begins_with("Color(")) {
		return "godot.Color";
	}
	bool digits = false;
	bool dot = false;
	for (int i = 0; i < value.length(); i++) {
		char32_t c = value[i];
		if (c == '_' || (c >= '0' && c <= '9')) {
			digits = true;
			continue;
		}
		if (c == '.') {
			dot = true;
			continue;
		}
		return "Dynamic";
	}
	if (digits) {
		return dot ? "Float" : "Int";
	}
	return "Dynamic";
}

/* Parses one argument of a GDScript signature: `name`, `name: type`,
 * `name = default`, `name: type = default` or `name := default`. */
HaxeArgumentInfo parse_gdscript_argument(const String &p_text) {
	HaxeArgumentInfo argument;
	String text = p_text.strip_edges();
	if (text.is_empty()) {
		return argument;
	}
	int separator = find_default_separator(text);
	String declaration = text;
	if (separator >= 0) {
		declaration = text.substr(0, separator).strip_edges();
		argument.optional = true;
		bool inferred = declaration.ends_with(":");
		if (inferred) {
			declaration = declaration.substr(0, declaration.length() - 1).strip_edges();
			argument.type = infer_gdscript_literal_type(text.substr(separator + 1));
		}
	}
	int colon = declaration.find(":");
	if (colon >= 0) {
		argument.name = declaration.substr(0, colon).strip_edges();
		if (argument.type.is_empty()) {
			argument.type = gdscript_type_to_haxe(declaration.substr(colon + 1));
		}
	} else {
		argument.name = declaration;
		if (argument.type.is_empty()) {
			argument.type = "Dynamic";
		}
	}
	return argument;
}

/* Splits a declaration line into its name and, when written, its type. */
void split_gdscript_declaration(const String &p_text, String &r_name, String &r_type) {
	String text = p_text.strip_edges();
	String default_value;
	int separator = find_default_separator(text);
	if (separator >= 0) {
		default_value = text.substr(separator + 1).strip_edges();
		String head = text.substr(0, separator).strip_edges();
		if (head.ends_with(":")) {
			// `var x := value`: the type is inferred from the initializer.
			head = head.substr(0, head.length() - 1).strip_edges();
			r_type = infer_gdscript_literal_type(default_value);
		}
		text = head;
	}
	int colon = text.find(":");
	if (colon >= 0) {
		r_name = text.substr(0, colon).strip_edges();
		if (r_type.is_empty()) {
			r_type = gdscript_type_to_haxe(text.substr(colon + 1));
		}
	} else {
		r_name = text;
		if (r_type.is_empty() && !default_value.is_empty()) {
			r_type = infer_gdscript_literal_type(default_value);
		}
		if (r_type.is_empty()) {
			r_type = "Dynamic";
		}
	}
}

/* The text between the first `(` and its matching `)`, or an empty string. */
bool extract_parenthesized(const String &p_text, String &r_inner, int &r_close) {
	int open = p_text.find("(");
	if (open < 0) {
		return false;
	}
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = open; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				r_inner = p_text.substr(open + 1, i - open - 1);
				r_close = i;
				return true;
			}
		}
	}
	return false;
}

} // namespace

Error parse_gdscript_source(const String &p_source, const String &p_path, HaxeClassInfo &r_info) {
	r_info = HaxeClassInfo();
	r_info.path = p_path;
	r_info.valid = true;

	Vector<String> lines = p_source.split("\n");
	bool pending_export = false;
	bool saw_declaration = false;

	for (int index = 0; index < lines.size(); index++) {
		String raw = lines[index];
		if (raw.ends_with("\r")) {
			raw = raw.substr(0, raw.length() - 1);
		}
		String stripped = raw.strip_edges();
		if (stripped.is_empty() || stripped.begins_with("#")) {
			continue;
		}
		// Only class level declarations belong to the class: anything indented
		// is a member of a function body, a nested class, or a multiline
		// expression.
		if (leading_indent(raw) > 0) {
			continue;
		}
		String content = strip_gdscript_comment(stripped).strip_edges();
		if (content.is_empty()) {
			continue;
		}

		while (content.begins_with("@")) {
			int cursor = 1;
			while (cursor < content.length() && gd_is_ident_char(content[cursor])) {
				cursor++;
			}
			String annotation = content.substr(1, cursor - 1);
			// An annotation may carry an argument list (`@export_range(0, 10)`).
			if (cursor < content.length() && content[cursor] == '(') {
				int depth = 0;
				bool in_string = false;
				char32_t quote = 0;
				for (; cursor < content.length(); cursor++) {
					char32_t c = content[cursor];
					if (in_string) {
						if (c == '\\') {
							cursor++;
							continue;
						}
						if (c == quote) {
							in_string = false;
						}
						continue;
					}
					if (c == '"' || c == '\'') {
						in_string = true;
						quote = c;
						continue;
					}
					if (c == '(') {
						depth++;
					} else if (c == ')') {
						depth--;
						if (depth == 0) {
							cursor++;
							break;
						}
					}
				}
			}
			if (annotation.begins_with("export")) {
				pending_export = true;
			}
			if (annotation == "tool") {
				r_info.is_tool = true;
			}
			content = content.substr(cursor).strip_edges();
		}
		if (content.is_empty()) {
			continue;
		}
		// `@export var x` puts the annotation and the declaration on one line.
		bool exported = pending_export;
		pending_export = false;

		if (content.begins_with("class_name")) {
			r_info.name = content.substr(String("class_name").length()).strip_edges();
			saw_declaration = true;
			pending_export = false;
			continue;
		}
		if (content.begins_with("extends")) {
			String base = content.substr(String("extends").length()).strip_edges();
			if (base.begins_with("\"") || base.begins_with("\'")) {
				base = base.substr(1, base.length() - 2);
			}
			r_info.base = base;
			continue;
		}
		if (content.begins_with("static func ")) {
			HaxeFunctionInfo function;
			function.name = content.substr(String("static func ").length()).strip_edges();
			function.is_static = true;
			function.line = index;
			// Everything up to `(` is the name.
			int paren = function.name.find("(");
			bool ok = false;
			if (paren >= 0) {
				String signature = function.name;
				function.name = signature.substr(0, paren).strip_edges();
				String inner;
				int close = -1;
				if (extract_parenthesized(signature, inner, close)) {
					for (const String &part : split_top_level(inner, ',')) {
						HaxeArgumentInfo argument = parse_gdscript_argument(part);
						if (argument.name.is_empty()) {
							continue;
						}
						function.arguments.push_back(argument);
					}
					String tail = signature.substr(close + 1).strip_edges();
					if (tail.ends_with(":")) {
						tail = tail.substr(0, tail.length() - 1).strip_edges();
					}
					if (tail.begins_with("->")) {
						function.return_type = gdscript_type_to_haxe(tail.substr(2));
					} else {
						function.return_type = "Void";
					}
					ok = !function.name.is_empty();
				}
			}
			if (ok) {
				r_info.functions.push_back(function);
				saw_declaration = true;
			}
			pending_export = false;
			continue;
		}
		if (content.begins_with("func ")) {
			HaxeFunctionInfo function;
			String signature = content.substr(String("func ").length()).strip_edges();
			int paren = signature.find("(");
			if (paren >= 0) {
				function.name = signature.substr(0, paren).strip_edges();
				String inner;
				int close = -1;
				if (extract_parenthesized(signature, inner, close)) {
					for (const String &part : split_top_level(inner, ',')) {
						HaxeArgumentInfo argument = parse_gdscript_argument(part);
						if (argument.name.is_empty()) {
							continue;
						}
						function.arguments.push_back(argument);
					}
					String tail = signature.substr(close + 1).strip_edges();
					if (tail.ends_with(":")) {
						tail = tail.substr(0, tail.length() - 1).strip_edges();
					}
					if (tail.begins_with("->")) {
						function.return_type = gdscript_type_to_haxe(tail.substr(2));
					} else {
						function.return_type = "Void";
					}
				}
				if (!function.name.is_empty()) {
					function.line = index;
					r_info.functions.push_back(function);
					saw_declaration = true;
				}
			}
			pending_export = false;
			continue;
		}
		if (content.begins_with("signal ")) {
			String signature = content.substr(String("signal ").length()).strip_edges();
			String name = signature;
			int paren = name.find("(");
			if (paren >= 0) {
				name = name.substr(0, paren);
			}
			HaxeMemberInfo member;
			member.name = name.strip_edges();
			member.type = "godot.Signal";
			member.line = index;
			if (!member.name.is_empty()) {
				r_info.variables.push_back(member);
				saw_declaration = true;
			}
			pending_export = false;
			continue;
		}
		if (content.begins_with("static var ")) {
			HaxeMemberInfo member;
			member.is_static = true;
			member.line = index;
			split_gdscript_declaration(content.substr(String("static var ").length()), member.name, member.type);
			if (!member.name.is_empty()) {
				r_info.variables.push_back(member);
				saw_declaration = true;
			}
			pending_export = false;
			continue;
		}
		if (content.begins_with("var ")) {
			HaxeMemberInfo member;
			member.exported = exported;
			member.line = index;
			split_gdscript_declaration(content.substr(String("var ").length()), member.name, member.type);
			if (!member.name.is_empty()) {
				r_info.variables.push_back(member);
				if (member.exported) {
					r_info.members.push_back(member);
				}
				saw_declaration = true;
			}
			pending_export = false;
			continue;
		}
		if (content.begins_with("const ")) {
			HaxeMemberInfo member;
			member.is_static = true;
			member.constant = true;
			member.line = index;
			split_gdscript_declaration(content.substr(String("const ").length()), member.name, member.type);
			if (!member.name.is_empty()) {
				r_info.variables.push_back(member);
				saw_declaration = true;
			}
			pending_export = false;
			continue;
		}

		pending_export = false;
	}

	// A file without `class_name` is still a valid script; the completion only
	// offers it when it is registered as a global class.
	if (!saw_declaration && r_info.name.is_empty()) {
		r_info.valid = true;
	}
	return OK;
}

HaxeGDScriptLibrary *HaxeGDScriptLibrary::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(HaxeGDScriptLibrary);
	}
	return singleton;
}

void HaxeGDScriptLibrary::index() {
	entries.clear();
	names.clear();

	LocalVector<StringName> globals;
	ScriptServer::get_global_class_list(globals);
	String new_signature;
	for (const StringName &global : globals) {
		String class_name = global;
		if (class_name.is_empty()) {
			continue;
		}
		if (String(ScriptServer::get_global_class_language(global)) != "GDScript") {
			continue;
		}
		String path = ScriptServer::get_global_class_path(class_name);
		if (path.is_empty()) {
			continue;
		}
		HaxeGDScriptEntry entry;
		entry.name = class_name;
		entry.path = path;
		entry.declared_base = ScriptServer::get_global_class_base(class_name);
		entry.native_base = ScriptServer::get_global_class_native_base(class_name);
		if (entry.native_base.is_empty()) {
			entry.native_base = "RefCounted";
		}
		entries[class_name] = entry;
		names.push_back(class_name);
		new_signature += class_name + ":" + path + ":" + itos((int64_t)FileAccess::get_modified_time(path)) + "\n";
	}
	names.sort();

	// Resolve the stub base once every class is known: a GDScript class extends
	// the stub of its GDScript base when there is one, and the engine binding
	// otherwise.
	for (KeyValue<String, HaxeGDScriptEntry> &item : entries) {
		HaxeGDScriptEntry &entry = item.value;
		if (!entry.declared_base.is_empty() && entries.has(entry.declared_base)) {
			entry.stub_base = entry.declared_base;
			continue;
		}
		// A path base (`extends "res://other.gd"`) names the class by file.
		String resolved;
		if (entry.declared_base.contains("/")) {
			for (const KeyValue<String, HaxeGDScriptEntry> &other : entries) {
				if (other.value.path == entry.declared_base) {
					resolved = other.key;
					break;
				}
			}
		}
		if (!resolved.is_empty()) {
			entry.stub_base = resolved;
		} else {
			entry.stub_base = "godot." + entry.native_base;
		}
	}

	signature = new_signature;
}

bool HaxeGDScriptLibrary::ensure_indexed() {
	MutexLock lock(mutex);
	String current;
	LocalVector<StringName> globals;
	ScriptServer::get_global_class_list(globals);
	for (const StringName &global : globals) {
		String class_name = global;
		if (class_name.is_empty() || String(ScriptServer::get_global_class_language(global)) != "GDScript") {
			continue;
		}
		String path = ScriptServer::get_global_class_path(class_name);
		current += class_name + ":" + path + ":" + itos((int64_t)FileAccess::get_modified_time(path)) + "\n";
	}
	if (current != signature) {
		index();
	}
	return !entries.is_empty();
}

bool HaxeGDScriptLibrary::has_type(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	if (!ensure_indexed()) {
		return false;
	}
	MutexLock lock(mutex);
	return entries.has(p_name);
}

bool HaxeGDScriptLibrary::get_entry(const String &p_name, HaxeGDScriptEntry &r_entry) {
	if (p_name.is_empty() || !ensure_indexed()) {
		return false;
	}
	MutexLock lock(mutex);
	if (!entries.has(p_name)) {
		return false;
	}
	r_entry = entries[p_name];
	return true;
}

bool HaxeGDScriptLibrary::get_type_info(const String &p_name, HaxeClassInfo &r_info) {
	HaxeGDScriptEntry entry;
	if (!get_entry(p_name, entry)) {
		return false;
	}
	Error error = OK;
	String source = FileAccess::get_file_as_string(entry.path, &error);
	if (error != OK || source.is_empty()) {
		return false;
	}
	HaxeClassInfo info;
	if (parse_gdscript_source(source, entry.path, info) != OK) {
		return false;
	}
	if (info.name.is_empty()) {
		info.name = entry.name;
	}
	info.fq_name = entry.name;
	r_info = info;
	return true;
}

const Vector<String> &HaxeGDScriptLibrary::get_type_names() {
	ensure_indexed();
	return names;
}

Error HaxeGDScriptLibrary::generate_stubs(const String &p_dir, String &r_error) {
	ensure_indexed();

	Error error = DirAccess::make_dir_recursive_absolute(p_dir);
	if (error != OK && error != ERR_ALREADY_EXISTS) {
		r_error = vformat("Cannot create the generated directory '%s'.", p_dir);
		return error;
	}

	MutexLock lock(mutex);
	// Parse every class once; the reserved set of a derived stub needs the
	// members of its GDScript ancestors.
	HashMap<String, HaxeClassInfo> infos;
	for (const KeyValue<String, HaxeGDScriptEntry> &item : entries) {
		Error read_error = OK;
		String source = FileAccess::get_file_as_string(item.value.path, &read_error);
		if (read_error != OK || source.is_empty()) {
			continue;
		}
		HaxeClassInfo info;
		if (parse_gdscript_source(source, item.value.path, info) != OK) {
			continue;
		}
		if (info.name.is_empty()) {
			info.name = item.key;
		}
		infos[item.key] = info;
	}

	HashSet<String> written;
	for (const KeyValue<String, HaxeGDScriptEntry> &item : entries) {
		const HaxeGDScriptEntry &entry = item.value;
		if (!infos.has(item.key)) {
			continue;
		}
		HashSet<String> reserved;
		// Engine members the base already declares.
		if (HaxeApiIndex::get_singleton()->ensure_loaded()) {
			for (const String &member : HaxeApiIndex::get_singleton()->get_member_names(entry.native_base)) {
				reserved.insert(member);
			}
		}
		// Members of the GDScript ancestors, so a redeclaration is skipped.
		String ancestor = entry.declared_base;
		int guard = 0;
		while (!ancestor.is_empty() && infos.has(ancestor) && guard++ < 64) {
			for (const HaxeMemberInfo &member : infos[ancestor].variables) {
				reserved.insert(member.name);
			}
			for (const HaxeFunctionInfo &function : infos[ancestor].functions) {
				reserved.insert(function.name);
			}
			ancestor = infos[ancestor].base;
		}

		String stub = generate_gdscript_stub(entry, infos[item.key], reserved);
		written.insert(item.key);
		const String path = p_dir.path_join(item.key + ".hx");
		Ref<FileAccess> existing = FileAccess::open(path, FileAccess::READ);
		if (existing.is_valid() && existing->get_as_utf8_string() == stub) {
			continue;
		}
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			r_error = vformat("Cannot write the GDScript stub '%s'.", path);
			return ERR_CANT_CREATE;
		}
		file->store_string(stub);
	}

	// Drop the stubs of classes that were removed (a renamed or deleted
	// `class_name`). Only files carrying the generated marker are touched.
	String marker = "// Generated by the Haxe Godot module from the GDScript class";
	for (const String &file : DirAccess::get_files_at(p_dir)) {
		if (!file.ends_with(".hx")) {
			continue;
		}
		String base = file.substr(0, file.length() - 3);
		if (written.has(base)) {
			continue;
		}
		const String path = p_dir.path_join(file);
		Ref<FileAccess> existing = FileAccess::open(path, FileAccess::READ);
		if (existing.is_null()) {
			continue;
		}
		if (!existing->get_as_utf8_string().begins_with(marker)) {
			continue;
		}
		existing->close();
		DirAccess::remove_absolute(path);
	}

	return OK;
}

void HaxeGDScriptLibrary::invalidate() {
	MutexLock lock(mutex);
	signature = String();
	entries.clear();
	names.clear();
}

String generate_gdscript_stub(const HaxeGDScriptEntry &p_entry, const HaxeClassInfo &p_info, const HashSet<String> &p_reserved) {
	HashSet<String> reserved(p_reserved);
	String source;
	source += "// Generated by the Haxe Godot module from the GDScript class `" + p_entry.name + "`.\n";
	source += "// Do not edit: it is regenerated from the project's GDScript sources.\n";
	source += "\n";
	source += "@:keep\n";
	source += "class " + p_entry.name + " extends " + p_entry.stub_base + " {\n";
	source += "\t/** Path of the GDScript source this class wraps. */\n";
	source += "\tpublic static inline var __hxgodotGdPath:String = " + String("\"") + p_entry.path + "\";\n";
	source += "\n";
	source += "\t/** The loaded GDScript resource, the receiver of static calls. */\n";
	source += "\t@:noCompletion public static function __hxgodotScript():Dynamic {\n";
	source += "\t\treturn godot.GD.load(__hxgodotGdPath);\n";
	source += "\t}\n";

	for (const HaxeFunctionInfo &function : p_info.functions) {
		String name = function.name;
		if (name.is_empty() || reserved.has(name)) {
			continue;
		}
		reserved.insert(name);
		reserved.insert("get_" + name);
		reserved.insert("set_" + name);

		String signature;
		bool has_optional = false;
		for (int i = 0; i < function.arguments.size(); i++) {
			const HaxeArgumentInfo &argument = function.arguments[i];
			String parameter = argument.name + ":" + argument.type;
			if (argument.optional) {
				parameter = "?" + parameter;
				has_optional = true;
			}
			signature += (i == 0 ? "" : ", ") + parameter;
		}
		String return_type = function.return_type.is_empty() ? String("Void") : function.return_type;

		// An omitted optional argument becomes `null` in Haxe; the call has to
		// drop the trailing ones so GDScript applies its own defaults.
		String arguments_expr = "[";
		for (int i = 0; i < function.arguments.size(); i++) {
			arguments_expr += (i == 0 ? "" : ", ") + function.arguments[i].name;
		}
		arguments_expr += "]";

		source += "\n\tpublic ";
		if (function.is_static) {
			source += "static ";
		}
		source += "function " + name + "(" + signature + "):" + return_type + " {\n";
		if (has_optional) {
			source += "\t\tvar __args:Array<Dynamic> = " + arguments_expr + ";\n";
			source += "\t\twhile (__args.length > 0 && __args[__args.length - 1] == null) {\n";
			source += "\t\t\t__args.pop();\n";
			source += "\t\t}\n";
			arguments_expr = "__args";
		}
		String receiver = function.is_static ? "__hxgodotScript().__hxgodotId" : "__hxgodotId";
		if (return_type == "Void") {
			source += "\t\thxgodot.Native.call(" + receiver + ", \"" + name + "\", " + arguments_expr + ");\n";
		} else {
			source += "\t\treturn cast hxgodot.Native.call(" + receiver + ", \"" + name + "\", " + arguments_expr + ");\n";
		}
		source += "\t}\n";
	}

	for (const HaxeMemberInfo &member : p_info.variables) {
		String name = member.name;
		if (name.is_empty() || reserved.has(name)) {
			continue;
		}
		reserved.insert(name);
		reserved.insert("get_" + name);
		reserved.insert("set_" + name);
		String type = member.type.is_empty() ? String("Dynamic") : member.type;

		if (member.is_static) {
			source += "\n\t/** `" + name + "` of the GDScript class. */\n";
			source += member.constant
					? "\tpublic static var " + name + "(get, never):" + type + ";\n"
					: "\tpublic static var " + name + "(get, set):" + type + ";\n";
			source += "\t@:noCompletion static function get_" + name + "():" + type + " {\n";
			source += "\t\treturn cast hxgodot.Native.getProperty(__hxgodotScript().__hxgodotId, \"" + name + "\");\n";
			source += "\t}\n";
			if (!member.constant) {
				source += "\t@:noCompletion static function set_" + name + "(value:" + type + "):" + type + " {\n";
				source += "\t\thxgodot.Native.setProperty(__hxgodotScript().__hxgodotId, \"" + name + "\", value);\n";
				source += "\t\treturn value;\n";
				source += "\t}\n";
			}
			continue;
		}

		source += "\n\t/** `" + name + "` of the GDScript instance. */\n";
		source += "\tpublic var " + name + "(get, set):" + type + ";\n";
		source += "\t@:noCompletion function get_" + name + "():" + type + " {\n";
		if (type == "godot.Signal") {
			source += "\t\treturn hxSignal(\"" + name + "\");\n";
		} else {
			source += "\t\treturn cast hxgodot.Native.getProperty(__hxgodotId, \"" + name + "\");\n";
		}
		source += "\t}\n";
		source += "\t@:noCompletion function set_" + name + "(value:" + type + "):" + type + " {\n";
		source += "\t\thxgodot.Native.setProperty(__hxgodotId, \"" + name + "\", value);\n";
		source += "\t\treturn value;\n";
		source += "\t}\n";
	}

	source += "}\n";
	return source;
}
