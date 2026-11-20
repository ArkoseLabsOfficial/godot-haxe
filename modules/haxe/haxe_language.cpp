/**************************************************************************/
/*  haxe_language.cpp                                                     */
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

#include "haxe_language.h"

#include "haxe_completion.h"
#include "haxe_project.h"
#include "haxe_script.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/string_builder.h"

HaxeLanguage *HaxeLanguage::singleton = nullptr;

namespace {

struct BuiltInTemplate {
	const char *inherit;
	const char *description;
};

// Templates are intentionally small: the interesting part is that the class
// extends the requested engine type and imports it from the generated
// bindings, which is what makes the script immediately valid.
const BuiltInTemplate BUILT_IN_TEMPLATES[] = {
	{ "Object", "Empty Haxe script extending Object." },
	{ "RefCounted", "Haxe script extending RefCounted." },
	{ "Resource", "Haxe resource extending Resource." },
	{ "Node", "Haxe script extending Node." },
	{ "Node2D", "Haxe 2D script extending Node2D." },
	{ "Node3D", "Haxe 3D script extending Node3D." },
	{ "Control", "Haxe UI script extending Control." },
};

String indent_for_line(int p_indent) {
	String result;
	for (int i = 0; i < p_indent; i++) {
		result += "\t";
	}
	return result;
}

} // namespace

HaxeLanguage::HaxeLanguage() {
	singleton = this;
}

HaxeLanguage::~HaxeLanguage() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

String HaxeLanguage::get_name() const {
	return "Haxe";
}

void HaxeLanguage::init() {
	HaxeRuntime::get_singleton();
	HaxeProject::register_project_settings();
}

String HaxeLanguage::get_type() const {
	return "Haxe";
}

String HaxeLanguage::get_extension() const {
	return "hx";
}

void HaxeLanguage::finish() {
	MutexLock lock_guard(lock);
	if (build_session != nullptr) {
		if (build_session->is_running()) {
			build_session->cancel();
		}
		memdelete(build_session);
		build_session = nullptr;
	}
	if (cached_library != nullptr) {
		cached_library->release_reference();
		cached_library = nullptr;
	}
	HaxeRuntime::get_singleton()->unload_all();
}

Vector<String> HaxeLanguage::get_reserved_words() const {
	static const char *words[] = {
		"abstract", "break", "case", "cast", "catch", "class", "continue",
		"default", "do", "dynamic", "else", "enum", "extends", "extern",
		"false", "final", "for", "function", "if", "implements", "import",
		"in", "inline", "interface", "is", "macro", "new", "null", "operator",
		"overload", "override", "package", "private", "public", "return",
		"static", "super", "switch", "this", "throw", "trace", "true", "try",
		"typedef", "untyped", "using", "var", "while"
	};
	Vector<String> result;
	for (const char *word : words) {
		result.push_back(word);
	}
	return result;
}

bool HaxeLanguage::is_control_flow_keyword(const String &p_string) const {
	static const char *keywords[] = {
		"break", "case", "continue", "default", "do", "else", "for", "if",
		"return", "switch", "throw", "try", "catch", "while"
	};
	for (const char *keyword : keywords) {
		if (p_string == keyword) {
			return true;
		}
	}
	return false;
}

Vector<String> HaxeLanguage::get_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("//");
	delimiters.push_back("/* */");
	return delimiters;
}

Vector<String> HaxeLanguage::get_doc_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("/** */");
	return delimiters;
}

Vector<String> HaxeLanguage::get_string_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("\" \"");
	delimiters.push_back("' '");
	return delimiters;
}

Ref<Script> HaxeLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<HaxeScript> script;
	script.instantiate();
	String class_name = p_class_name.to_pascal_case().validate_unicode_identifier();
	if (class_name.is_empty()) {
		class_name = "NewScript";
	}

	// The import line depends on what the base is: an engine class lives in
	// `godot`, while a project class (or a script path chosen from the "New
	// script" dialog for a custom node type) has its own package.
	String base = p_base_class_name;
	String base_reference = base;
	String import_line;
	HaxeClassInfo base_info;
	String lookup = base.is_quoted() ? base.unquote() : base;
	if (HaxeProject::get_class_info(lookup, base_info) || HaxeProject::get_class_info_by_name(lookup, base_info)) {
		base_reference = base_info.name.is_empty() ? lookup : base_info.name;
		import_line = base_info.fq_name.is_empty() ? String() : "import " + base_info.fq_name + ";";
	} else if (base.is_quoted()) {
		// A script path that is not a known project class: use its file name
		// as the type so the generated code still names a type.
		base_reference = lookup.get_file().get_basename().to_pascal_case();
	} else {
		import_line = "import godot." + base + ";";
	}

	String processed = p_template.replace("_IMPORT_", import_line)
							   .replace("_BASE_", base_reference)
							   .replace("_CLASS_", class_name)
							   .replace("_TS_", "\t");
	if (import_line.is_empty()) {
		// No import to add: keep the package line next to the class instead of
		// leaving the placeholder's blank lines behind.
		processed = processed.replace("\n\n\n\n", "\n\n");
	}
	script->set_source_code(processed);
	// The package is not known until the dialog picks a path; `set_path()`
	// replaces the placeholder package with the one the path implies.
	script->set_template_package_pending(true);
	return script;
}

Vector<ScriptLanguage::ScriptTemplate> HaxeLanguage::get_built_in_templates(const StringName &p_object) {
	Vector<ScriptTemplate> templates;
	for (const BuiltInTemplate &entry : BUILT_IN_TEMPLATES) {
		if (StringName(entry.inherit) != p_object) {
			continue;
		}
		ScriptTemplate script_template;
		script_template.inherit = entry.inherit;
		script_template.name = vformat("Haxe %s", entry.inherit);
		script_template.description = entry.description;

		String content =
				"package;\n\n"
				"_IMPORT_\n\n"
				"class _CLASS_ extends _BASE_ {\n";
		// Only Node derived classes receive `_ready` and `_process`. Object,
		// RefCounted and Resource have no such callbacks, so their template
		// keeps an empty class body.
		if (String(entry.inherit) != "Object" && String(entry.inherit) != "RefCounted" &&
				String(entry.inherit) != "Resource") {
			content +=
					"_TS_/** Called once when the node and its children enter the scene tree. */\n"
					"_TS_override public function _ready():Void {\n"
					"_TS_}\n"
					"\n"
					"_TS_/** Called every frame with the time elapsed since the previous frame. */\n"
					"_TS_override public function _process(delta:Float):Void {\n"
					"_TS_}\n";
		}
		content += "}\n";
		script_template.content = content;
		templates.push_back(script_template);
	}
	return templates;
}

bool HaxeLanguage::validate(const String &p_script, const String &p_path, List<String> *r_functions, List<ScriptError> *r_errors, List<Warning> *r_warnings, HashSet<int> *r_safe_lines) const {
	(void)r_warnings;
	(void)r_safe_lines;

	HaxeClassInfo info;
	Error error = parse_haxe_source(p_script, p_path, info);
	if (error != OK) {
		if (r_errors != nullptr) {
			ScriptError script_error;
			script_error.path = p_path;
			script_error.line = info.class_line > 0 ? info.class_line : 1;
			script_error.column = 1;
			script_error.message = info.error.is_empty() ? String("Invalid Haxe source.") : info.error;
			r_errors->push_back(script_error);
		}
		return false;
	}

	// A `.hx` file is a Haxe *module*, not necessarily a script class. It may
	// declare only helper classes, enums, typedefs or abstracts, and a class may
	// extend a type that only the Haxe compiler knows (a library type, a type
	// from another module, a macro type). The engine therefore accepts any file
	// that parses and leaves inheritance to the compiler: a class that does not
	// end up extending a Godot class simply cannot be attached to an object
	// (`HaxeScript::can_instantiate()` reports that when it is attempted),
	// which is not a source error.
	//
	// A built-in (embedded) script is a different matter: its source lives
	// inside a scene or resource, and the Haxe compiler only reads files, so
	// there is nothing it could compile.
	if (p_path.contains("::")) {
		if (r_errors != nullptr) {
			ScriptError script_error;
			script_error.path = p_path;
			script_error.line = MAX(info.class_line, 1);
			script_error.column = 1;
			script_error.message = "Haxe scripts cannot be embedded in a scene or resource. Save the script as its own .hx file: the Haxe compiler builds files, not editor-embedded sources.";
			r_errors->push_back(script_error);
		}
		return false;
	}

	if (r_functions != nullptr) {
		for (const HaxeFunctionInfo &function : info.functions) {
			if (!function.is_signal) {
				// The editor's function list is the "name:line" form GDScript
				// uses; the connection gutter and the quick-open list rely on
				// the line number, which must be one based and never zero.
				r_functions->push_back(vformat("%s:%d", function.name, MAX(function.line, 1)));
			}
		}
	}
	return true;
}

bool HaxeLanguage::supports_builtin_mode() const {
	// The Haxe compiler reads `.hx` files from disk: an embedded (built-in)
	// script only exists inside a scene or resource, so it could never be
	// compiled and would fail at build time. The script creation dialog hides
	// the "Built-in Script" option for this language.
	return false;
}

int HaxeLanguage::find_function(const String &p_function, const String &p_code) const {
	// The tokenizer is shared with the metadata parser, but for jumping to a
	// function a line scan is enough and matches the editor's expectations.
	int line = 1;
	int index = 0;
	while (index < p_code.length()) {
		int line_end = p_code.find_char('\n', index);
		if (line_end < 0) {
			line_end = p_code.length();
		}
		String text = p_code.substr(index, line_end - index);
		int position = text.find("function");
		if (position >= 0) {
			int cursor = position + 8;
			while (cursor < text.length() && (text[cursor] == ' ' || text[cursor] == '\t')) {
				cursor++;
			}
			String name;
			while (cursor < text.length()) {
				char32_t c = text[cursor];
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
					name += c;
					cursor++;
				} else {
					break;
				}
			}
			if (name == p_function) {
				return line;
			}
		}
		index = line_end + 1;
		line++;
	}
	return -1;
}

String HaxeLanguage::make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const {
	(void)p_class;
	String arguments;
	for (int i = 0; i < p_args.size(); i++) {
		if (i > 0) {
			arguments += ", ";
		}
		arguments += p_args[i] + ":Dynamic";
	}
	return vformat("\n\npublic function %s(%s):Void {\n\t\n}\n", p_name, arguments);
}

String HaxeLanguage::validate_path(const String &p_path) const {
	if (p_path.get_extension().to_lower() != "hx") {
		return "Haxe scripts must use the .hx extension.";
	}
	String file_name = p_path.get_file().get_basename();
	if (file_name.is_empty()) {
		return "Haxe scripts must have a file name.";
	}
	return String();
}

Error HaxeLanguage::complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) {
	return haxe_complete_code(p_code, p_path, p_owner, r_options, r_force, r_call_hint);
}

Error HaxeLanguage::lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result) {
	return haxe_lookup_code(p_code, p_symbol, p_path, p_owner, r_result);
}

void HaxeLanguage::auto_indent_code(String &p_code, int p_from_line, int p_to_line) const {
	PackedStringArray lines = p_code.split("\n");
	int indent = 0;
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty()) {
			lines.set(i, String());
			continue;
		}

		bool dedent = line.begins_with("}") || line.begins_with("case ") || line.begins_with("default:");
		if (dedent) {
			indent = MAX(0, indent - 1);
		}

		if (i >= p_from_line - 1 && i <= p_to_line - 1) {
			lines.set(i, indent_for_line(indent) + line);
		}

		int opens = 0;
		int closes = 0;
		bool in_string = false;
		char32_t quote = 0;
		for (int j = 0; j < line.length(); j++) {
			char32_t c = line[j];
			if (in_string) {
				if (c == '\\') {
					j++;
				} else if (c == quote) {
					in_string = false;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				in_string = true;
				quote = c;
			} else if (c == '/' && j + 1 < line.length() && (line[j + 1] == '/' || line[j + 1] == '*')) {
				break;
			} else if (c == '{' || c == '(' || c == '[') {
				opens++;
			} else if (c == '}' || c == ')' || c == ']') {
				closes++;
			}
		}
		// A line that starts with a closing brace already consumed one level.
		int delta = opens - closes;
		if (dedent && delta < 0) {
			delta = 0;
		}
		indent = MAX(0, indent + delta);
		if (line.ends_with("{")) {
			// The opening brace counted above already accounts for it.
		}
	}
	p_code = String("\n").join(lines);
}

void HaxeLanguage::add_global_constant(const StringName &p_variable, const Variant &p_value) {
	global_constants[p_variable] = p_value;
}

/* ---- Debugger ---------------------------------------------------------- */

void HaxeLanguage::refresh_debug_info() {
	debug_error = String();
	debug_stack.clear();
	if (cached_library == nullptr) {
		return;
	}
	debug_error = cached_library->get_last_error();
	String trace = cached_library->get_last_stack_trace();
	if (trace.is_empty()) {
		return;
	}
	for (const String &raw_line : trace.split("\n")) {
		String line = raw_line.strip_edges();
		if (line.is_empty()) {
			continue;
		}
		StackInfo info;
		info.file = String();
		info.line = 0;
		info.func = line;
		int colon = line.rfind(":");
		if (colon > 0) {
			String maybe_line = line.substr(colon + 1).strip_edges();
			if (maybe_line.is_valid_int()) {
				info.line = maybe_line.to_int();
				String prefix = line.substr(0, colon);
				int second = prefix.rfind(":");
				if (second > 0) {
					String maybe_column = prefix.substr(second + 1).strip_edges();
					if (maybe_column.is_valid_int()) {
						info.file = prefix.substr(0, second);
						info.func = String();
					}
				}
				if (info.file.is_empty()) {
					info.file = prefix;
				}
				int space = info.file.rfind(" ");
				if (space > 0 && info.file.substr(space + 1).ends_with(".hx")) {
					info.func = info.file.substr(0, space);
					info.file = info.file.substr(space + 1);
				}
			}
		}
		debug_stack.push_back(info);
	}
}

String HaxeLanguage::debug_get_error() const {
	return debug_error;
}

int HaxeLanguage::debug_get_stack_level_count() const {
	return debug_stack.size();
}

int HaxeLanguage::debug_get_stack_level_line(int p_level) const {
	if (p_level < 0 || p_level >= debug_stack.size()) {
		return 0;
	}
	return debug_stack[p_level].line;
}

String HaxeLanguage::debug_get_stack_level_function(int p_level) const {
	if (p_level < 0 || p_level >= debug_stack.size()) {
		return String();
	}
	return debug_stack[p_level].func;
}

String HaxeLanguage::debug_get_stack_level_source(int p_level) const {
	if (p_level < 0 || p_level >= debug_stack.size()) {
		return String();
	}
	return debug_stack[p_level].file;
}

void HaxeLanguage::debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	(void)p_level;
	(void)p_locals;
	(void)p_values;
	(void)p_max_subitems;
	(void)p_max_depth;
}

void HaxeLanguage::debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	(void)p_level;
	(void)p_members;
	(void)p_values;
	(void)p_max_subitems;
	(void)p_max_depth;
}

void HaxeLanguage::debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	(void)p_globals;
	(void)p_values;
	(void)p_max_subitems;
	(void)p_max_depth;
}

String HaxeLanguage::debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems, int p_max_depth) {
	(void)p_level;
	(void)p_max_subitems;
	(void)p_max_depth;
	return p_expression;
}

Vector<ScriptLanguage::StackInfo> HaxeLanguage::debug_get_current_stack_info() {
	return debug_stack;
}

/* ---- Reload ------------------------------------------------------------ */

void HaxeLanguage::reload_all_scripts() {
	Vector<HaxeScript *> all;
	{
		MutexLock lock_guard(lock);
		for (HaxeScript *script : scripts) {
			all.push_back(script);
		}
	}
	for (HaxeScript *script : all) {
		script->reload();
	}
	HaxeProject::invalidate();
}

void HaxeLanguage::reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	(void)p_soft_reload;
	for (int i = 0; i < p_scripts.size(); i++) {
		Ref<HaxeScript> script = p_scripts[i];
		if (script.is_valid()) {
			script->reload();
		}
	}
}

void HaxeLanguage::reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	(void)p_soft_reload;
	Ref<HaxeScript> script = p_script;
	if (script.is_valid()) {
		script->reload();
	}
}

void HaxeLanguage::reload_dependent_scripts(HaxeScript *p_script) {
	// A cascade is already refreshing this tree of scripts; let it finish so
	// the reload of a dependent does not start a second walk.
	if (p_script == nullptr || reloading_dependents) {
		return;
	}
	const Vector<HaxeScript *> candidates = collect_dependents(p_script);

	reloading_dependents = true;
	for (HaxeScript *script : candidates) {
		script->reload();
	}
	reloading_dependents = false;
}

void HaxeLanguage::refresh_dependent_exports(HaxeScript *p_script) {
	if (p_script == nullptr || refreshing_exports) {
		return;
	}
	const Vector<HaxeScript *> candidates = collect_dependents(p_script);

	refreshing_exports = true;
	for (HaxeScript *script : candidates) {
		script->refresh_exported_members();
	}
	refreshing_exports = false;
}

Vector<HaxeScript *> HaxeLanguage::collect_dependents(HaxeScript *p_script) {
	Vector<HaxeScript *> dependents;
	const HaxeClassInfo &changed_info = p_script->get_class_info();
	const String changed_fq = changed_info.fq_name;
	const String changed_short = changed_info.name;
	if (changed_fq.is_empty() && changed_short.is_empty()) {
		return dependents;
	}

	Vector<HaxeScript *> candidates;
	{
		MutexLock lock_guard(lock);
		for (HaxeScript *script : scripts) {
			if (script != p_script) {
				candidates.push_back(script);
			}
		}
	}

	for (HaxeScript *script : candidates) {
		HashSet<String> visited;
		String base = script->get_class_info().base;
		while (!base.is_empty() && !visited.has(base)) {
			if (base == changed_fq || base == changed_short) {
				dependents.push_back(script);
				break;
			}
			visited.insert(base);
			HaxeClassInfo parent;
			if (!HaxeProject::get_class_info_by_name(base, parent)) {
				break;
			}
			base = parent.base;
		}
	}
	return dependents;
}

void HaxeLanguage::register_script(HaxeScript *p_script) {
	MutexLock lock_guard(lock);
	scripts.insert(p_script);
}

void HaxeLanguage::unregister_script(HaxeScript *p_script) {
	MutexLock lock_guard(lock);
	scripts.erase(p_script);
}

/* ---- Loader ------------------------------------------------------------ */

void HaxeLanguage::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("hx");
}

void HaxeLanguage::get_public_functions(List<MethodInfo> *p_functions) const {
	(void)p_functions;
}

void HaxeLanguage::get_public_constants(List<Pair<String, Variant>> *p_constants) const {
	for (const KeyValue<StringName, Variant> &entry : global_constants) {
		p_constants->push_back(Pair<String, Variant>(entry.key, entry.value));
	}
}

void HaxeLanguage::get_public_annotations(List<MethodInfo> *p_annotations) const {
	// The annotations understood by the module. Exposed so the editor can
	// document them alongside built-in language keywords: `--doctool` turns
	// this list into `@Haxe.xml`. Keep it in sync with the completion list in
	// `haxe_completion.cpp` and with the argument handling in `haxe_source.cpp`
	// (`@:exportRange` reads up to three arguments, `@:exportEnum` reads one or
	// more, `@:exportFile` reads an optional filter).
	p_annotations->push_back(MethodInfo("@:export"));
	p_annotations->push_back(MethodInfo("@:exported"));
	p_annotations->push_back(MethodInfo("@:exportGroup", PropertyInfo(Variant::STRING, "name")));
	p_annotations->push_back(MethodInfo("@:exportSubgroup", PropertyInfo(Variant::STRING, "name")));

	MethodInfo export_range("@:exportRange", PropertyInfo(Variant::FLOAT, "min"), PropertyInfo(Variant::FLOAT, "max"), PropertyInfo(Variant::FLOAT, "step"));
	export_range.default_arguments.push_back(1.0);
	p_annotations->push_back(export_range);

	MethodInfo export_enum("@:exportEnum", PropertyInfo(Variant::STRING, "names"));
	export_enum.flags |= METHOD_FLAG_VARARG;
	p_annotations->push_back(export_enum);

	MethodInfo export_file("@:exportFile", PropertyInfo(Variant::STRING, "filter"));
	export_file.default_arguments.push_back(String(""));
	export_file.flags |= METHOD_FLAG_VARARG;
	p_annotations->push_back(export_file);

	p_annotations->push_back(MethodInfo("@:exportDir"));
	p_annotations->push_back(MethodInfo("@:exportMultiline"));
	p_annotations->push_back(MethodInfo("@:signal"));
	p_annotations->push_back(MethodInfo("@:gdSignal"));
	p_annotations->push_back(MethodInfo("@:tool"));
	p_annotations->push_back(MethodInfo("@:gdTool"));
	p_annotations->push_back(MethodInfo("@:editor"));
	p_annotations->push_back(MethodInfo("@:abstract"));
	p_annotations->push_back(MethodInfo("@:globalClass"));
	p_annotations->push_back(MethodInfo("@:gdGlobalClass"));
	p_annotations->push_back(MethodInfo("@:static"));
}

/* ---- Profiling --------------------------------------------------------- */

void HaxeLanguage::profiling_start() {
	profiling_frame = 0;
}

void HaxeLanguage::profiling_stop() {
}

void HaxeLanguage::profiling_set_save_native_calls(bool p_enable) {
	(void)p_enable;
}

int HaxeLanguage::profiling_get_accumulated_data(ProfilingInfo *p_info_arr, int p_info_max) {
	(void)p_info_arr;
	(void)p_info_max;
	// The hxcpp runtime does not expose per-function timing hooks without
	// instrumentation. Reporting zero entries is accurate; the profiler shows
	// no Haxe rows instead of fabricated data.
	return 0;
}

int HaxeLanguage::profiling_get_frame_data(ProfilingInfo *p_info_arr, int p_info_max) {
	(void)p_info_arr;
	(void)p_info_max;
	return 0;
}

/* ---- Class discovery --------------------------------------------------- */

bool HaxeLanguage::handles_global_class_type(const String &p_type) const {
	// The resource loader reports `HaxeScript`; `Haxe` is the language name and
	// is accepted for extensions that register global classes by language.
	return p_type == "Haxe" || p_type == "HaxeScript";
}

String HaxeLanguage::get_global_class_name(const String &p_path, String *r_base_type, String *r_icon_path, bool *r_is_abstract, bool *r_is_tool) const {
	return HaxeProject::get_global_class_name(p_path, r_base_type, r_icon_path, r_is_abstract, r_is_tool);
}

/* ---- Build and library ------------------------------------------------- */

HaxeBuild::Result HaxeLanguage::build_project(bool p_release, bool p_editor, bool p_force, String &r_error) {
	MutexLock lock_guard(lock);
	if (build_session != nullptr) {
		// A background compile started from the editor panel is still running.
		// Wait for it rather than starting a second compiler that would write
		// the same output file, then continue with the requested build.
		while (build_session->is_running()) {
			build_session->poll();
			OS::get_singleton()->delay_usec(2000);
		}
		HaxeBuild::Result previous = build_session->get_result();
		last_build_log = previous.raw_output;
		memdelete(build_session);
		build_session = nullptr;
		build_in_progress = false;
	}

	build_in_progress = true;
	HaxeBuild::Result result = HaxeBuild::build(p_release, p_editor, p_force);
	build_in_progress = false;
	last_build_log = result.raw_output;
	if (!result.success) {
		last_build_error = result.message;
		r_error = result.message;
		return result;
	}
	last_build_error = String();
	return result;
}

bool HaxeLanguage::start_build(bool p_release, bool p_editor, bool p_force) {
	MutexLock lock_guard(lock);
	if (build_session != nullptr) {
		return false;
	}
	build_session = memnew(HaxeBuildSession);
	build_session->start(p_release, p_editor, p_force);
	build_in_progress = build_session->is_running();
	return true;
}

bool HaxeLanguage::poll_build() {
	MutexLock lock_guard(lock);
	if (build_session == nullptr) {
		return false;
	}
	if (build_session->poll()) {
		return true;
	}
	// The compiler exited: publish its output immediately so the build log is
	// complete even before the panel picks the result up.
	const HaxeBuild::Result &result = build_session->get_result();
	last_build_log = result.raw_output;
	if (!result.success) {
		last_build_error = result.message;
	} else {
		last_build_error = String();
	}
	build_in_progress = false;
	return false;
}

bool HaxeLanguage::is_building() const {
	MutexLock lock_guard(lock);
	return build_session != nullptr && build_session->is_running();
}

bool HaxeLanguage::has_build_session() const {
	MutexLock lock_guard(lock);
	return build_session != nullptr;
}

String HaxeLanguage::get_build_output() const {
	MutexLock lock_guard(lock);
	return build_session != nullptr ? build_session->get_output() : last_build_log;
}

const HaxeBuild::Result &HaxeLanguage::get_build_result() const {
	static const HaxeBuild::Result empty;
	return build_session != nullptr ? build_session->get_result() : empty;
}

HaxeBuild::Result HaxeLanguage::take_build_result() {
	MutexLock lock_guard(lock);
	if (build_session == nullptr) {
		return HaxeBuild::Result();
	}
	HaxeBuild::Result result = build_session->get_result();
	memdelete(build_session);
	build_session = nullptr;
	build_in_progress = false;
	return result;
}

void HaxeLanguage::cancel_build() {
	MutexLock lock_guard(lock);
	if (build_session == nullptr) {
		return;
	}
	build_session->cancel();
	last_build_error = build_session->get_result().message;
	last_build_log = build_session->get_output();
	memdelete(build_session);
	build_session = nullptr;
	build_in_progress = false;
}

HaxeLibrary *HaxeLanguage::get_script_library(HaxeScript *p_script) {
	if (!HaxeRuntime::get_singleton()->has_support_files()) {
		last_build_error = "Haxe support files are missing from this Godot build. Reinstall the engine or rebuild it with the Haxe module.";
		return nullptr;
	}

#ifdef DEBUG_ENABLED
	const bool release = false;
#else
	const bool release = true;
#endif
	const bool editor = Engine::get_singleton()->is_editor_hint();

	// An exported game has no build directory and no Haxe toolchain: the
	// compiled library travels inside the pck and is extracted to a real file
	// and loaded directly, instead of going through the build path below.
	if (!editor) {
		String packaged = HaxeProject::extract_packaged_library();
		if (!packaged.is_empty()) {
			MutexLock lock_guard(lock);
			if (cached_library != nullptr && cached_library->path == packaged) {
				return cached_library;
			}
			String error;
			HaxeLibrary *library = HaxeRuntime::get_singleton()->load_library(packaged, error);
			if (library == nullptr) {
				last_build_error = error;
				HaxeRuntime::report_error(error);
				return nullptr;
			}
			if (cached_library != nullptr) {
				cached_library->release_reference();
			}
			cached_library = library;
			cached_stamp = String();
			last_build_error = String();
			refresh_debug_info();
			return library;
		}
	}

	MutexLock lock_guard(lock);
	if (build_in_progress) {
		return cached_library;
	}

	String stamp = HaxeBuild::current_stamp(release, editor);
	if (cached_library != nullptr && cached_stamp == stamp) {
		return cached_library;
	}

	String versioned = HaxeProject::get_versioned_library_path(release, editor, stamp);

	if (!FileAccess::exists(versioned) || HaxeBuild::is_build_required(release, editor)) {
		if (!HaxeProject::build_on_run()) {
			last_build_error = vformat("The Haxe project has not been built for this configuration (expected %s). Enable 'haxe/build/build_on_run' or run Project > Build Haxe Project.", versioned);
			return nullptr;
		}
		build_in_progress = true;
		HaxeBuild::Result result = HaxeBuild::build(release, editor, false);
		build_in_progress = false;
		last_build_log = result.raw_output;
		if (!result.success) {
			last_build_error = result.message;
			// A compile failure reports a count in `message`; the parsed
			// diagnostics say which file and line are wrong, and this error
			// reaches the user without the build panel that normally lists
			// them (a Run, or a scene loading a script before the first build).
			const String details = HaxeBuild::format_errors(result);
			HaxeRuntime::report_error(vformat("Haxe build failed for '%s':\n%s%s", p_script ? p_script->get_path() : String("<unknown>"), result.message, details.is_empty() ? String() : "\n" + details));
			return nullptr;
		}
		versioned = result.library_path;
		stamp = HaxeBuild::current_stamp(release, editor);
	}

	if (cached_library != nullptr && cached_library->path == versioned) {
		cached_stamp = stamp;
		return cached_library;
	}

	String error;
	HaxeLibrary *library = HaxeRuntime::get_singleton()->load_library(versioned, error);
	if (library == nullptr) {
		last_build_error = error;
		HaxeRuntime::report_error(error);
		return nullptr;
	}

	if (cached_library != nullptr) {
		cached_library->release_reference();
	}
	cached_library = library;
	cached_stamp = stamp;
	last_build_error = String();
	refresh_debug_info();
	return library;
}

/* ---- Resource loader --------------------------------------------------- */

Ref<Resource> ResourceFormatLoaderHaxe::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	(void)p_use_sub_threads;
	(void)r_progress;
	(void)p_cache_mode;
	(void)p_original_path;

	Ref<HaxeScript> script;
	script.instantiate();
	Error error = script->load_source_code(p_path);
	if (error != OK) {
		if (r_error) {
			*r_error = error;
		}
		return Ref<Resource>();
	}
	script->set_path(p_path, true);
	if (r_error) {
		*r_error = OK;
	}
	return script;
}

void ResourceFormatLoaderHaxe::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("hx");
}

bool ResourceFormatLoaderHaxe::handles_type(const String &p_type) const {
	return p_type == "Script" || p_type == "HaxeScript";
}

String ResourceFormatLoaderHaxe::get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "hx") {
		return "HaxeScript";
	}
	return String();
}

/* ---- Resource saver ---------------------------------------------------- */

Error ResourceFormatSaverHaxe::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	(void)p_flags;
	Ref<HaxeScript> script = p_resource;
	if (script.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}
	file->store_string(script->get_source_code());
	return OK;
}

void ResourceFormatSaverHaxe::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<HaxeScript>(*p_resource)) {
		p_extensions->push_back("hx");
	}
}

bool ResourceFormatSaverHaxe::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<HaxeScript>(*p_resource) != nullptr;
}
