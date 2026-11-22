/**************************************************************************/
/*  test_haxe.h                                                           */
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

#pragma once

#include "../haxe_audio_stream.h"
#include "../haxe_build.h"
#include "../haxe_completion.h"
#include "../haxe_elf.h"
#include "../haxe_gdscript.h"
#include "../haxe_language.h"
#include "../haxe_library_index.h"
#include "../haxe_project.h"
#include "../haxe_runtime.h"
#include "../haxe_script.h"
#include "../haxe_source.h"
#include "../haxe_stdlib.h"
#include "../haxe_support.h"
#include "../haxe_value.h"

#ifdef TOOLS_ENABLED
#include "../editor/haxe_export_plugin.h"
#include "../editor/haxe_highlighter.h"

#include "core/io/zip_io.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/text_edit.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/script_language.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"
// `Node` is a base class of every scripted scene type the tests instantiate.
// Editor builds get it through `scene/gui/text_edit.h` below, template builds
// (which compile these tests with `tests=yes`) do not, so include it directly.
#include "scene/main/node.h"
#include "servers/audio/audio_stream.h"
#include "tests/test_macros.h"

// The helpers the cases below call are declared in `test_haxe_utils.h` and
// defined in `test_haxe.cpp`. That header is deliberately separate: this file
// holds the test cases and is compiled into `modules_tests.gen.h`, so if it
// also held the definitions' declarations, `test_haxe.cpp` including it would
// register every case a second time.
#include "test_haxe_utils.h"

namespace TestHaxe {

TEST_CASE("[Modules][Haxe] script metadata is derived from the source") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"\n"
			"import godot.Node2D;\n"
			"\n"
			"@:tool\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 120.0;\n"
			"\t@:export var health:Int = 100;\n"
			"\tvar hidden:Bool = false;\n"
			"\n"
			"\toverride public function _ready():Void {\n"
			"\t}\n"
			"\n"
			"\tstatic public function create(count:Int, label:String):Probe {\n"
			"\t\treturn null;\n"
			"\t}\n"
			"}\n");

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(path, info));
	CHECK(info.name == "Probe");
	CHECK(info.package == "game");
	CHECK(info.fq_name == "game.Probe");
	CHECK(info.base == "Node2D");
	CHECK(info.is_tool);
	CHECK_FALSE(info.is_abstract);

	// Only exported variables double as Godot properties.
	REQUIRE(info.members.size() == 2);
	CHECK(info.members[0].name == "speed");
	CHECK(info.members[0].type == "Float");
	CHECK(info.members[0].exported);
	CHECK(info.members[1].name == "health");
	CHECK(info.members[1].type == "Int");

	// Functions carry their signature, and `static` is a plain keyword.
	const HaxeFunctionInfo *ready = info.find_function("_ready");
	REQUIRE(ready != nullptr);
	CHECK_FALSE(ready->is_static);

	const HaxeFunctionInfo *create = info.find_function("create");
	REQUIRE(create != nullptr);
	CHECK(create->is_static);
	CHECK(create->return_type == "Probe");
	REQUIRE(create->arguments.size() == 2);
	CHECK(create->arguments[0].name == "count");
	CHECK(create->arguments[0].type == "Int");
	CHECK(create->arguments[1].name == "label");
	CHECK(create->arguments[1].type == "String");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] doc comments describe the declaration below them") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Documented.hx",
			"/** Moves the actor around the level. */\n"
			"class Documented extends Node2D {\n"
			"\t/** Pixels per second. */\n"
			"\t@:export var speed:Float = 120.0;\n"
			"\n"
			"\t/** Leaves the node untouched when it is already there. */\n"
			"\tpublic function arrive():Void {\n"
			"\t}\n"
			"\n"
			"\t@:export var undocumented:Int = 1;\n"
			"}\n");

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(path, info));
	CHECK(info.doc == "Moves the actor around the level.");
	REQUIRE(info.members.size() == 2);
	// The annotation line between the comment and `var` belongs to the
	// declaration, so the comment is still attached.
	CHECK(info.members[0].doc == "Pixels per second.");
	// No comment: the member keeps an empty description and the editor falls
	// back to a generated one.
	CHECK(info.members[1].doc.is_empty());

	const HaxeFunctionInfo *arrive = info.find_function("arrive");
	REQUIRE(arrive != nullptr);
	CHECK(arrive->doc == "Leaves the node untouched when it is already there.");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a helper file is not reported as a broken script") {
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);

	const String dir = make_scratch_dir();
	// A module of helper types: it declares no Godot class at all, which is
	// normal Haxe and must not be a script error.
	const String helpers_path = write_source(dir, "Helpers.hx",
			"package game;\n"
			"\n"
			"class Helpers {\n"
			"\tpublic static function clamp01(value:Float):Float {\n"
			"\t\treturn value < 0 ? 0 : (value > 1 ? 1 : value);\n"
			"\t}\n"
			"}\n"
			"\n"
			"enum Mode {\n"
			"\tIdle;\n"
			"\tRun;\n"
			"}\n");

	List<String> functions;
	List<ScriptLanguage::ScriptError> errors;
	const String helpers_source = FileAccess::get_file_as_string(helpers_path);
	CHECK(language->validate(helpers_source, helpers_path, &functions, &errors, nullptr, nullptr));
	CHECK(errors.is_empty());

	// A class may extend a type only the compiler knows; the engine accepts it
	// and lets the compiler report a real problem.
	const String library_path = write_source(dir, "Library.hx",
			"class Library extends hxbit.Serializable {\n"
			"}\n");
	List<ScriptLanguage::ScriptError> library_errors;
	CHECK(language->validate(FileAccess::get_file_as_string(library_path), library_path, &functions, &library_errors, nullptr, nullptr));
	CHECK(library_errors.is_empty());

	// Validation accepting the file is not enough: the editor loads it as a
	// `Script` resource, so `load_source_code` has to succeed too. A class that
	// does not reach a Godot type loads and can be edited, but it is not a
	// script that can be attached or instantiated.
	Ref<HaxeScript> helpers;
	helpers.instantiate();
	REQUIRE(helpers->load_source_code(helpers_path) == OK);
	CHECK_FALSE(helpers->is_valid());
	CHECK_FALSE(helpers->can_instantiate());

	Ref<HaxeScript> library;
	library.instantiate();
	REQUIRE(library->load_source_code(library_path) == OK);
	CHECK_FALSE(library->is_valid());
	CHECK_FALSE(library->can_instantiate());

	// An embedded script has no file to compile, so it is rejected with an
	// explanation instead of the removed missing-base error.
	List<ScriptLanguage::ScriptError> embedded_errors;
	CHECK_FALSE(language->validate("class Embedded {\n}\n", "res://scene.tscn::HaxeScript_1", &functions, &embedded_errors, nullptr, nullptr));
	REQUIRE_FALSE(embedded_errors.is_empty());
	CHECK(embedded_errors.front()->get().message.contains("cannot be embedded"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an empty .hx file is a valid, editable module") {
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);

	const String dir = make_scratch_dir();

	// A file created by `touch`, by the FileSystem dock or by an external
	// editor holds no type yet. It is a valid (if empty) Haxe module: the editor
	// must open and edit it instead of reporting "No type declaration found.".
	const String empty_path = write_source(dir, "Created.hx", "");
	List<String> functions;
	List<ScriptLanguage::ScriptError> empty_errors;
	CHECK(language->validate("", empty_path, &functions, &empty_errors, nullptr, nullptr));
	CHECK(empty_errors.is_empty());

	Ref<HaxeScript> empty;
	empty.instantiate();
	REQUIRE(empty->load_source_code(empty_path) == OK);
	CHECK(empty->is_valid());
	// Nothing is declared yet, so there is no class to attach.
	CHECK_FALSE(empty->can_instantiate());

	// A header is the same case: `package`/`import` without a type yet.
	const String header_path = write_source(dir, "Header.hx",
			"package game;\n"
			"import godot.Node2D;\n");
	List<ScriptLanguage::ScriptError> header_errors;
	CHECK(language->validate(FileAccess::get_file_as_string(header_path), header_path, &functions, &header_errors, nullptr, nullptr));
	CHECK(header_errors.is_empty());
	Ref<HaxeScript> header;
	header.instantiate();
	REQUIRE(header->load_source_code(header_path) == OK);
	CHECK(header->is_valid());

	// Real code without a type is still a source error, not an empty module.
	const String broken_path = write_source(dir, "Broken.hx", "var stray = 1;\n");
	List<ScriptLanguage::ScriptError> broken_errors;
	CHECK_FALSE(language->validate("var stray = 1;\n", broken_path, &functions, &broken_errors, nullptr, nullptr));
	REQUIRE_FALSE(broken_errors.is_empty());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] @:tool controls instantiation while scripting is off") {
	const String dir = make_scratch_dir();
	const String plain_path = write_source(dir, "Plain.hx",
			"import godot.Node2D;\n"
			"class Plain extends Node2D {\n"
			"\tstatic public function create():Plain {\n"
			"\t\treturn null;\n"
			"\t}\n"
			"}\n");
	const String tool_path = write_source(dir, "Tool.hx",
			"import godot.Node2D;\n"
			"@:tool\n"
			"class Tool extends Node2D {\n"
			"}\n");

	Ref<HaxeScript> plain;
	plain.instantiate();
	REQUIRE(plain->load_source_code(plain_path) == OK);
	Ref<HaxeScript> tool;
	tool.instantiate();
	REQUIRE(tool->load_source_code(tool_path) == OK);

	REQUIRE(plain->is_valid());
	REQUIRE(tool->is_valid());
	CHECK_FALSE(plain->is_tool());
	CHECK(tool->is_tool());
	CHECK(plain->has_static_method("create"));

#ifdef TOOLS_ENABLED
	const bool was_enabled = ScriptServer::is_scripting_enabled();

	// The editor turns scripting off; non-tool code must not be instantiable.
	ScriptServer::set_scripting_enabled(false);
	CHECK_FALSE(plain->can_instantiate());
	CHECK(tool->can_instantiate());

	// Running the game turns it back on: both kinds are instantiable.
	ScriptServer::set_scripting_enabled(true);
	CHECK(plain->can_instantiate());
	CHECK(tool->can_instantiate());

	ScriptServer::set_scripting_enabled(was_enabled);
#endif

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] completion lists engine members for a typed receiver") {
	// The editor inserts U+FFFF at the caret to mark the completion position.
	const String code = "import godot.Vector2;\n"
						"class Probe {\n"
						"\tfunction f() {\n"
						"\t\tvar v:Vector2;\n"
						"\t\tv." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n";

	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;
	REQUIRE(haxe_complete_code(code, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(force);

	HashSet<String> names;
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		names.insert(option.display);
	}
	CHECK(names.has("x"));
	CHECK(names.has("y"));
	CHECK(names.has("normalized"));
	CHECK(names.has("distance_to"));
}

TEST_CASE("[Modules][Haxe] a call hint names the arguments of a project function") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\tfunction play(file:String, volume:Float):Bool {\n"
			"\t\treturn true;\n"
			"\t}\n"
			"\tfunction run():Void {\n"
			"\t\tself.play(0);\n"
			"\t}\n"
			"}\n");
	SourceDirScope scope(dir);
	const String marker = String::chr(0xFFFF);

	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;

	// `self.play(` resolves through the edited script, and the argument the
	// caret is on (the first) is the one wrapped in U+FFFF.
	const String first = "package game;\nimport godot.Node2D;\nclass Probe extends Node2D {\n"
						 "\tfunction play(file:String, volume:Float):Bool {\n\t\treturn true;\n\t}\n"
						 "\tfunction run():Void {\n\t\tself.play(" +
			marker + ");\n\t}\n}\n";
	REQUIRE(haxe_complete_code(first, path, nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "Bool play(" + marker + "file: String" + marker + ", volume: Float)");

	// The second argument is marked instead once a comma precedes the caret.
	options.clear();
	const String second = "package game;\nimport godot.Node2D;\nclass Probe extends Node2D {\n"
						  "\tfunction play(file:String, volume:Float):Bool {\n\t\treturn true;\n\t}\n"
						  "\tfunction run():Void {\n\t\tself.play(\"x\", " +
			marker + ");\n\t}\n}\n";
	REQUIRE(haxe_complete_code(second, path, nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "Bool play(file: String, " + marker + "volume: Float" + marker + ")");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a call hint names the arguments of an engine method") {
	const String marker = String::chr(0xFFFF);
	const String header = "import godot.Vector2;\nclass Probe {\n\tfunction f() {\n\t\tvar v:Vector2;\n\t\t";
	const String footer = ";\n\t}\n}\n";

	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;

	// The names come from api.json, not from the receiver.
	REQUIRE(haxe_complete_code(header + "v.rotated(" + marker + ")" + footer, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "godot.Vector2 rotated(" + marker + "angle: Float" + marker + ")");

	options.clear();
	REQUIRE(haxe_complete_code(header + "v.move_toward(v, " + marker + ")" + footer, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "godot.Vector2 move_toward(to: godot.Vector2, " + marker + "delta: Float" + marker + ")");
}

TEST_CASE("[Modules][Haxe] a call hint marks a vararg rest argument") {
	const String marker = String::chr(0xFFFF);
	const String header = "import godot.Node;\nclass Probe {\n\tfunction f() {\n\t\tvar n:Node;\n\t\t";
	const String footer = ";\n\t}\n}\n";

	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;

	// A declared parameter is marked before the rest argument.
	REQUIRE(haxe_complete_code(header + "n.call(" + marker + ")" + footer, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "Dynamic call(" + marker + "method: String" + marker + ", ...args: Array)");

	// Past the declared parameters the rest argument itself is marked.
	options.clear();
	REQUIRE(haxe_complete_code(header + "n.call(\"x\", " + marker + ")" + footer, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint == "Dynamic call(method: String, " + marker + "...args: Array" + marker + ")");
}

TEST_CASE("[Modules][Haxe] no call hint outside a call") {
	const String marker = String::chr(0xFFFF);
	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;

	// Not inside a parenthesis at all.
	REQUIRE(haxe_complete_code("class Probe {\n\tvar x = " + marker + ";\n}\n", String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint.is_empty());

	// The only `(` is inside a string literal, which is not a call.
	options.clear();
	REQUIRE(haxe_complete_code("class Probe {\n\tvar s = \"(\";\n\tvar y = 1" + marker + ";\n}\n", String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint.is_empty());

	// A parameter list in a declaration is not a call either.
	options.clear();
	REQUIRE(haxe_complete_code("class Probe {\n\tfunction f(a:" + marker + "Int) {\n\t}\n}\n", String(), nullptr, &options, force, call_hint) == OK);
	CHECK(call_hint.is_empty());
}

TEST_CASE("[Modules][Haxe] lookup resolves an engine class member") {
	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("Vector2.", "distance_to", String(), nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD);
	CHECK(result.class_name == "Vector2");
	CHECK(result.class_member == "distance_to");
}

TEST_CASE("[Modules][Haxe] import lines complete engine types") {
	// `import godot.No` must offer the engine classes, not member access.
	const String code = "package game;\n"
						"import godot.No" +
			String::chr(0xFFFF) + ";\n"
								  "\n"
								  "class Probe extends Node {\n"
								  "}\n";

	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;
	REQUIRE(haxe_complete_code(code, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(force);

	HashSet<String> names;
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		names.insert(option.display);
	}
	CHECK(names.has("Node"));
	CHECK(names.has("Node2D"));
	CHECK(names.has("Node3D"));
	// Only the identifier before the caret is replaced, not the whole path.
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		CHECK_FALSE(option.insert_text.contains("."));
	}
}

TEST_CASE("[Modules][Haxe] a package import offers the package root first") {
	const String code = "import " + String::chr(0xFFFF) + "\n";
	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;
	REQUIRE(haxe_complete_code(code, String(), nullptr, &options, force, call_hint) == OK);

	HashSet<String> names;
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		names.insert(option.display);
	}
	CHECK(names.has("godot"));
	CHECK(names.has("hxgodot"));
}

TEST_CASE("[Modules][Haxe] extraParams.hxml becomes compiler arguments") {
	// Several options, one per line, with a comment and the options that the
	// module provides itself.
	const Vector<String> arguments = HaxeBuild::parse_extra_params(
			"# a comment\n"
			"--macro keep('IntIterator')\n"
			"-lib oldlib\n"
			"-cp relative/path\n"
			"-D feature=on\n"
			"--macro hscript.macros.AbstractHandler.init()\n");
	REQUIRE(arguments.size() == 6);
	CHECK(arguments[0] == "--macro");
	CHECK(arguments[1] == "keep('IntIterator')");
	CHECK(arguments[2] == "-D");
	CHECK(arguments[3] == "feature=on");
	CHECK(arguments[4] == "--macro");
	CHECK(arguments[5] == "hscript.macros.AbstractHandler.init()");

	// Everything after the first space is the option's value. A surrounding
	// pair of quotes is removed, the quotes of a macro call are kept.
	const Vector<String> quoted = HaxeBuild::parse_extra_params("-D \"greeting=hello world\"\n--macro keep('IntIterator')\n");
	REQUIRE(quoted.size() == 4);
	CHECK(quoted[0] == "-D");
	CHECK(quoted[1] == "greeting=hello world");
	CHECK(quoted[2] == "--macro");
	CHECK(quoted[3] == "keep('IntIterator')");
}

TEST_CASE("[Modules][Haxe] extra arguments split the option from its value") {
	// The project setting is a list, but entries are often typed the way an
	// hxml line is written. Passing `--macro keep('IntIterator')` through as a
	// single argument makes the compiler fail with
	//   Error: unknown option '--macro keep('IntIterator')'
	const Vector<String> normalized = HaxeBuild::normalize_extra_arguments({
			"--macro keep('IntIterator')",
			"  -D feature=on  ",
			"-D ",
			"--macro hscript.macros.AbstractHandler.init()",
			"--macro \"x('quoted', 1)\"",
			"   ",
	});
	REQUIRE(normalized.size() == 9);
	CHECK(normalized[0] == "--macro");
	CHECK(normalized[1] == "keep('IntIterator')");
	CHECK(normalized[2] == "-D");
	CHECK(normalized[3] == "feature=on");
	CHECK(normalized[4] == "-D");
	CHECK(normalized[5] == "--macro");
	CHECK(normalized[6] == "hscript.macros.AbstractHandler.init()");
	CHECK(normalized[7] == "--macro");
	CHECK(normalized[8] == "x('quoted', 1)");
}

TEST_CASE("[Modules][Haxe] compiler arguments round-trip through an hxml file") {
	// The Windows build hands its options to the compiler through an `.hxml`
	// file, because the process API cannot pass an argument that contains a
	// double quote. The file has to read back as the same list the build built.
	const Vector<String> original = {
		"-cp",
		"/project/source",
		"-main",
		"game.Main",
		"-D",
		"hxgodot_editor",
		"--macro",
		"haxe.macro.Compiler.define(\"libhxcpp\",\"6.0.0\")",
		"--macro",
		"addGlobalMetadata('mylib.BaseThing', '@:keep', true, false, true)",
		"-D",
		"feature=on",
	};
	List<String> arguments;
	for (const String &argument : original) {
		arguments.push_back(argument);
	}

	const String hxml = HaxeBuild::serialize_hxml_arguments(arguments);
	// A token with no whitespace is a line of its own (the loader reads it as
	// one argument, quotes and all); the option and a value that contains
	// spaces stay on one line.
	CHECK(hxml.contains("haxe.macro.Compiler.define(\"libhxcpp\",\"6.0.0\")\n"));
	CHECK(hxml.contains("--macro addGlobalMetadata('mylib.BaseThing', '@:keep', true, false, true)\n"));

	// Read it back the way the compiler's hxml loader does: one argument per
	// line, and a line holding a space yields the option plus everything after
	// the first space as its value.
	Vector<String> read_back;
	for (const String &line : hxml.split("\n")) {
		if (line.is_empty()) {
			continue;
		}
		const int space = line.find_char(' ');
		if (space < 0) {
			read_back.push_back(line);
		} else {
			read_back.push_back(line.substr(0, space));
			read_back.push_back(line.substr(space + 1));
		}
	}
	REQUIRE(read_back.size() == original.size());
	for (int i = 0; i < original.size(); i++) {
		CHECK(read_back[i] == original[i]);
	}
}

TEST_CASE("[Modules][Haxe] the boot class routes trace to the engine log") {
	// hxcpp sends `trace()` to stdout. Rebinding `haxe.Log.trace` in the
	// generated entry point makes a `trace()` reach the engine's output system
	// (the editor Output panel, the game log) as well.
	const String boot = HaxeBuild::generate_boot_class({});
	CHECK(boot.contains("haxe.Log.trace = function"));
	CHECK(boot.contains("hxgodot.Native.log(0, haxe.Log.formatOutput(value, infos))"));
}

TEST_CASE("[Modules][Haxe] the boot class references the attachable library classes") {
	// A library `@:globalClass` a scene attaches is never named by project code,
	// so `-dce full` would drop it: `@:keep` metadata only applies to a type
	// that is typed, and an unreferenced type is not. The boot module is what
	// forces it into the compilation; a project class of the same name stays the
	// single reference, and an abstract project class is skipped.
	HaxeClassInfo project_class;
	project_class.valid = true;
	project_class.name = "Thing";
	project_class.fq_name = "game.Thing";
	HaxeClassInfo abstract_class;
	abstract_class.valid = true;
	abstract_class.name = "Shape";
	abstract_class.fq_name = "game.Shape";
	abstract_class.is_abstract = true;

	const Vector<String> library_classes = { "mylib.Thing", "hxvlc.godot.Video" };
	const String boot = HaxeBuild::generate_boot_class({ project_class, abstract_class }, library_classes);
	CHECK(boot.contains("\t\tgame.Thing,\n"));
	// The library class of the same short name is kept and is the only entry for
	// that fully qualified name.
	CHECK(boot.count("mylib.Thing") == 1);
	CHECK(boot.contains("\t\thxvlc.godot.Video,\n"));
	CHECK_FALSE(boot.contains("game.Shape"));
	CHECK_FALSE(boot.contains("mylib.BaseThing"));
}

TEST_CASE("[Modules][Haxe] the toolchain host API version is read from its header") {
	// The engine refuses a library whose glue was compiled for another host API
	// version, so `build()` compares the version the toolchain header declares
	// with the engine's before compiling. A missing or malformed header has to
	// read as "unknown" rather than as a wrong version.
	const String dir = make_scratch_dir();
	CHECK(HaxeBuild::read_toolchain_abi_version(dir.path_join("missing")) == -1);

	(void)write_source(dir, "hxgodot_abi.h",
			"/* a comment mentioning HXGODOT_ABI_VERSION 99 */\n"
			"#define HXGODOT_ABI_VERSION 7\n"
			"#define HXGODOT_ABI_VERSION_EXTRA\n");
	CHECK(HaxeBuild::read_toolchain_abi_version(dir) == 7);

	// A header without the define is unknown, not version 0.
	(void)write_source(dir, "hxgodot_abi.h", "#define SOMETHING_ELSE 1\n");
	CHECK(HaxeBuild::read_toolchain_abi_version(dir) == -1);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a library reports the host API version it was built for") {
	// The version is named in the failure message so a stale library or a
	// mismatched export template is obvious, instead of only "incompatible
	// host API version".
	CHECK(HaxeRuntime::library_abi_from_info("abi=4\nruntime=hxcpp\nbuilt=Nov 21 2026\n") == "4");
	CHECK(HaxeRuntime::library_abi_from_info("runtime=hxcpp\nabi=12\n") == "12");
	// A library built before the field existed reports nothing.
	CHECK(HaxeRuntime::library_abi_from_info("runtime=hxcpp\n") == "");
	CHECK(HaxeRuntime::library_abi_from_info("") == "");
}

TEST_CASE("[Modules][Haxe] the host API streams files through the engine file system") {
	// A project library cannot link the engine, so a decoder that brings its
	// own I/O (LibVLC) reads a packed res:// entry in an exported game through
	// these host functions. They have to behave like FileAccess: report the
	// size, read and seek at byte offsets, and refuse handles that were
	// released or never existed.
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "stream.bin", "0123456789");

	int64_t size = -1;
	const int64_t handle = HaxeRuntime::host_file_open(path.utf8().get_data(), &size);
	REQUIRE(handle > 0);
	CHECK(size == 10);
	CHECK(HaxeRuntime::host_file_size(handle) == 10);

	char buffer[8] = {};
	CHECK(HaxeRuntime::host_file_read(handle, buffer, 4) == 4);
	CHECK(String::utf8(buffer, 4) == "0123");

	// The position advanced, and reading again continues from there.
	CHECK(HaxeRuntime::host_file_read(handle, buffer, 2) == 2);
	CHECK(String::utf8(buffer, 2) == "45");

	// A seek repositions the stream; reading to the end reports the remaining
	// bytes first and end of file afterwards.
	CHECK(HaxeRuntime::host_file_seek(handle, 8) == 0);
	CHECK(HaxeRuntime::host_file_read(handle, buffer, 8) == 2);
	CHECK(String::utf8(buffer, 2) == "89");
	CHECK(HaxeRuntime::host_file_read(handle, buffer, 4) == 0);

	HaxeRuntime::host_file_close(handle);
	CHECK(HaxeRuntime::host_file_size(handle) == -1);
	CHECK(HaxeRuntime::host_file_read(handle, buffer, 4) < 0);
	CHECK(HaxeRuntime::host_file_seek(handle, 0) < 0);

	// A path the engine cannot open yields no handle, and bad arguments are
	// rejected instead of dereferencing a null buffer.
	int64_t missing_size = 0;
	CHECK(HaxeRuntime::host_file_open(dir.path_join("missing.bin").utf8().get_data(), &missing_size) == 0);
	CHECK(HaxeRuntime::host_file_open(nullptr, nullptr) == 0);
	CHECK(HaxeRuntime::host_file_read(handle, nullptr, 4) < 0);
	CHECK(HaxeRuntime::host_file_seek(handle, -1) < 0);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] qualified receivers complete project and engine members") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Probe.hx",
			"package game;\n"
			"\n"
			"import godot.Node2D;\n"
			"\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 1.0;\n"
			"\tvar hidden:Bool = false;\n"
			"\tstatic var instances:Int = 0;\n"
			"\n"
			"\tpublic function run():Void {\n"
			"\t}\n"
			"\n"
			"\tstatic public function create():Probe {\n"
			"\t\treturn null;\n"
			"\t}\n"
			"}\n");
	SourceDirScope scope(dir);

	// `package.Class.` is a class reference: only the members Haxe accepts
	// there are offered (statistics and constants), not instance members such as
	// `run()`.
	const HashSet<String> project = completion_names(
			"class Other {\n"
			"\tfunction f() {\n"
			"\t\tgame.Probe." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n");
	CHECK(project.has("instances"));
	CHECK(project.has("create"));
	CHECK_FALSE(project.has("speed"));
	CHECK_FALSE(project.has("hidden"));
	CHECK_FALSE(project.has("run"));

	// The same for a fully qualified engine class: instance members like
	// `position` and `add_child` are not statistics.
	const HashSet<String> engine = completion_names(
			"class Other {\n"
			"\tfunction f() {\n"
			"\t\tgodot.Node2D." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n");
	CHECK_FALSE(engine.has("position"));
	CHECK_FALSE(engine.has("add_child"));
	CHECK(engine.has("print_orphan_nodes"));

	// An instance value, in contrast, gets the instance members.
	const HashSet<String> instance = completion_names(
			"class Other {\n"
			"\tfunction f() {\n"
			"\t\tself." +
					String::chr(0xFFFF) + "\n"
										  "\t}\n"
										  "}\n",
			path);
	CHECK(instance.has("position"));
	CHECK(instance.has("add_child"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] completion offers declared variables and project packages") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Probe.hx",
			"package game;\n"
			"\n"
			"import godot.Node2D;\n"
			"\n"
			"class Probe extends Node2D {\n"
			"\tvar hidden:Bool = false;\n"
			"\n"
			"\tfunction f() {\n"
			"\t}\n"
			"}\n");
	SourceDirScope scope(dir);

	// A variable that is not exported is still offered inside the class body.
	const HashSet<String> names = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\tvar hidden:Bool = false;\n"
			"\tfunction f() {\n"
			"\t\thi" +
					String::chr(0xFFFF) + "\n"
										  "\t}\n"
										  "}\n",
			path);
	CHECK(names.has("hidden"));

	// Project packages are offered on an import line.
	const HashSet<String> imports = completion_names("import " + String::chr(0xFFFF) + "\n");
	CHECK(imports.has("game"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] imports offer secondary types and sub-packages") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n"
			"class Maina {\n"
			"\tstatic public function salam():Void {\n"
			"\t}\n"
			"}\n");
	(void)write_source(dir.path_join("game").path_join("sub"), "Thing.hx",
			"package game.sub;\n"
			"class Thing {\n"
			"}\n");
	SourceDirScope scope(dir);

	// A type declared in the same file is imported through its file.
	const HashSet<String> secondary = completion_names("import game.Main.Ma" + String::chr(0xFFFF) + "\n");
	CHECK(secondary.has("Maina"));

	// A sub-package of the typed path is offered as well.
	const HashSet<String> sub = completion_names("import game." + String::chr(0xFFFF) + "\n");
	CHECK(sub.has("sub"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] secondary types complete their own members") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.GD;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\toverride public function _ready():Void {\n"
			"\t\tMaina.salam();\n"
			"\t}\n"
			"}\n"
			"class Maina {\n"
			"\tpublic static var greeting:String = \"hi\";\n"
			"\tpublic static function salam():Void {\n"
			"\t}\n"
			"\tpublic function whisper():Void {\n"
			"\t}\n"
			"}\n");
	SourceDirScope scope(dir);

	// `Maina.` inside the file that declares it: this is a class reference, so
	// only the members Haxe allows there (statistics) are offered, not the
	// instance method `whisper`.
	const HashSet<String> members = completion_names(
			"package game;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tMaina." +
					String::chr(0xFFFF) + "\n"
										  "\t}\n"
										  "}\n",
			path);
	CHECK(members.has("salam"));
	CHECK(members.has("greeting"));
	CHECK_FALSE(members.has("whisper"));

	// The same through the import path the compiler expects.
	const HashSet<String> qualified = completion_names(
			"package game;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tgame.Main.Maina." +
					String::chr(0xFFFF) + "\n"
										  "\t}\n"
										  "}\n",
			path);
	CHECK(qualified.has("salam"));

	// Lookup reaches the declaration inside the declaring file.
	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("Maina", "Maina", path, nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS);
	CHECK(result.script_path == path);
	CHECK(result.location >= 9);

	// The type is offered by name in the scope of the declaring file.
	const HashSet<String> scope_names = completion_names(
			"package game;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tMai" +
					String::chr(0xFFFF) + "\n"
										  "\t}\n"
										  "}\n",
			path);
	CHECK(scope_names.has("Maina"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] exported members are inherited from base scripts") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Base.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"}\n");
	const String child_path = write_source(dir.path_join("game"), "Child.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"import game.Base;\n"
			"class Child extends Base {\n"
			"\t@:export var speed:Float = 3.0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> child;
	child.instantiate();
	REQUIRE(child->load_source_code(child_path) == OK);

	List<PropertyInfo> properties;
	child->get_script_property_list(&properties);
	HashSet<String> names;
	for (const PropertyInfo &property : properties) {
		names.insert(property.name);
	}
	CHECK(names.has("speed"));
	CHECK(names.has("health"));

	Variant value;
	CHECK(child->get_property_default_value("health", value));
	CHECK((int)value == 25);
	CHECK(child->get_member_line("health") > 0);

	remove_scratch_dir(dir);
}

#ifdef TOOLS_ENABLED
/* The placeholder instance is what the Inspector shows for a script whose
 * library has not been compiled yet; `placeholder_instance_create()` returns
 * `nullptr` outside an editor build, so the refresh-through-placeholder
 * behavior can only be exercised here. */
TEST_CASE("[Modules][Haxe] a base script change refreshes dependent scripts") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Base.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"}\n");
	const String base_path = dir.path_join("game").path_join("Base.hx");
	const String child_path = write_source(dir.path_join("game"), "Child.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"import game.Base;\n"
			"class Child extends Base {\n"
			"\t@:export var speed:Float = 3.0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> base;
	base.instantiate();
	base->set_path(base_path, true);
	REQUIRE(base->is_valid());

	Ref<HaxeScript> child;
	child.instantiate();
	child->set_path(child_path, true);
	REQUIRE(child->is_valid());

	// The Inspector holds a placeholder instance, whose property list is only
	// refreshed when its script is reloaded.
	Node *node = memnew(Node);
	PlaceHolderScriptInstance *placeholder = child->placeholder_instance_create(node);
	REQUIRE(placeholder != nullptr);
	node->set_script_instance(placeholder);

	auto placeholder_names = [&]() {
		List<PropertyInfo> properties;
		placeholder->get_property_list(&properties);
		HashSet<String> names;
		for (const PropertyInfo &property : properties) {
			names.insert(property.name);
		}
		return names;
	};

	const HashSet<String> before = placeholder_names();
	CHECK(before.has("speed"));
	CHECK(before.has("health"));
	CHECK_FALSE(before.has("armor"));

	// Saving the base class adds an exported member. Reloading it the way the
	// editor does must also refresh the subclass, or its Inspector keeps
	// showing the previous inherited properties.
	(void)write_source(dir.path_join("game"), "Base.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"\t@:export var armor:Int = 5;\n"
			"}\n");
	REQUIRE(base->reload() == OK);

	const HashSet<String> after = placeholder_names();
	CHECK(after.has("speed"));
	CHECK(after.has("health"));
	CHECK(after.has("armor"));

	memdelete(node);
	remove_scratch_dir(dir);
}
#endif

TEST_CASE("[Modules][Haxe] exports update when the source changes") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 1.0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(path) == OK);

	{
		List<PropertyInfo> properties;
		script->get_script_property_list(&properties);
		HashSet<String> names;
		for (const PropertyInfo &property : properties) {
			names.insert(property.name);
		}
		CHECK(names.has("speed"));
		CHECK_FALSE(names.has("armor"));
	}

	// The editor parses the edited text and calls `update_exports()`, so the
	// inspector sees the new member without reloading the resource.
	script->set_source_code(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 1.0;\n"
			"\t@:export var armor:Int = 5;\n"
			"}\n");
	script->update_exports();

	{
		List<PropertyInfo> properties;
		script->get_script_property_list(&properties);
		HashSet<String> names;
		for (const PropertyInfo &property : properties) {
			names.insert(property.name);
		}
		CHECK(names.has("speed"));
		CHECK(names.has("armor"));
	}

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] @:globalClass controls the global class name") {
	const String dir = make_scratch_dir();
	const String global_path = write_source(dir, "Global.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"@:globalClass\n"
			"class Global extends Node2D {\n"
			"}\n");
	const String plain_path = write_source(dir, "Plain.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Plain extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);

	String base;
	String icon;
	CHECK(HaxeProject::get_global_class_name(global_path, &base, &icon) == "Global");
	CHECK(base == "Node2D");
	// No icon override: the global class keeps the icon of its native base
	// type, so only the name is registered with the editor.
	CHECK(icon.is_empty());

	// Without the annotation the script stays an ordinary script, but the
	// editor still learns its native base class.
	base = String();
	icon = String();
	CHECK(HaxeProject::get_global_class_name(plain_path, &base, &icon).is_empty());
	CHECK(base == "Node2D");
	CHECK(icon.is_empty());

	// The language must accept the type the resource loader reports for a
	// `.hx` file; `Haxe` alone never matched it.
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);
	CHECK(language->handles_global_class_type("Haxe"));
	CHECK(language->handles_global_class_type("HaxeScript"));
	CHECK_FALSE(language->handles_global_class_type("GDScript"));

	Ref<HaxeScript> global_script;
	global_script.instantiate();
	REQUIRE(global_script->load_source_code(global_path) == OK);
	CHECK(global_script->get_global_name() == StringName("Global"));

	Ref<HaxeScript> plain_script;
	plain_script.instantiate();
	REQUIRE(plain_script->load_source_code(plain_path) == OK);
	CHECK(plain_script->get_global_name() == StringName());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] completion lists the members of the hand written support classes") {
	// `godot.GD` and `hxgodot.Native` are support files, not generated engine
	// bindings, so their members exist only after the module parses them.
	const String qualified_code = "class Probe {\n"
								  "\tfunction f() {\n"
								  "\t\tgodot.GD." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n";
	{
		HashSet<String> names = completion_names(qualified_code);
		CHECK(names.has("print"));
		CHECK(names.has("print_rich"));
		CHECK(names.has("push_warning"));
	}

	// The same class through a short import.
	const String imported_code = "import godot.GD;\n"
								 "class Probe {\n"
								 "\tfunction f() {\n"
								 "\t\tGD." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n";
	{
		HashSet<String> names = completion_names(imported_code);
		CHECK(names.has("print"));
		CHECK(names.has("sin"));
	}
}

TEST_CASE("[Modules][Haxe] @: completion offers the annotations of this module") {
	// After `@:` only the annotation name is inserted.
	const String annotated_code = "class Probe {\n"
								  "\t@:" +
			String::chr(0xFFFF) + "\n"
								  "\tfunction f() {}\n"
								  "}\n";
	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;
	REQUIRE(haxe_complete_code(annotated_code, String(), nullptr, &options, force, call_hint) == OK);
	CHECK(force);

	HashSet<String> names;
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		names.insert(option.display);
	}
	CHECK(names.has("export"));
	CHECK(names.has("signal"));
	CHECK(names.has("tool"));
	CHECK(names.has("globalClass"));
	CHECK(names.has("exportRange"));
	// The Haxe compiler's own metadata is offered too, so a user coming from
	// the Haxe ecosystem finds `@:inline`, `@:keep`, ... where they expect it.
	CHECK(names.has("inline"));
	CHECK(names.has("deprecated"));
	CHECK(names.has("keep"));
	// GDScript annotations are not Haxe metadata.
	CHECK_FALSE(names.has("onready"));
	CHECK_FALSE(names.has("warning_ignore"));

	// After a bare `@` the inserted text has to supply the colon.
	const String bare_code = "class Probe {\n"
							 "\t@" +
			String::chr(0xFFFF) + "\n"
								  "\tfunction f() {}\n"
								  "}\n";
	options.clear();
	REQUIRE(haxe_complete_code(bare_code, String(), nullptr, &options, force, call_hint) == OK);
	bool found = false;
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		if (option.display == "@:export") {
			CHECK(option.insert_text == ":export");
			found = true;
		}
	}
	CHECK(found);
}

TEST_CASE("[Modules][Haxe] built-in templates use the Haxe script shape") {
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);

	Vector<ScriptLanguage::ScriptTemplate> node_templates = language->get_built_in_templates("Node");
	REQUIRE(node_templates.size() == 1);
	const String node_content = node_templates[0].content;
	CHECK(node_content.contains("package;"));
	CHECK(node_content.contains("_IMPORT_"));
	CHECK(node_content.contains("class _CLASS_ extends _BASE_"));
	// Node classes receive the real lifecycle callbacks.
	CHECK(node_content.contains("override public function _ready():Void {"));
	CHECK(node_content.contains("override public function _process(delta:Float):Void {"));

	// `Object` has no such callbacks, so its template keeps an empty body.
	Vector<ScriptLanguage::ScriptTemplate> object_templates = language->get_built_in_templates("Object");
	REQUIRE(object_templates.size() == 1);
	CHECK_FALSE(object_templates[0].content.contains("_ready"));
	CHECK_FALSE(object_templates[0].content.contains("_process"));

	Ref<Script> script = language->make_template(node_content, "my_player", "Node");
	REQUIRE(script.is_valid());
	const String source = script->get_source_code();
	CHECK(source.contains("import godot.Node;"));
	// The class name is pascal cased and the placeholders are gone.
	CHECK(source.contains("class MyPlayer extends Node {"));
	CHECK_FALSE(source.contains("_CLASS_"));
	CHECK_FALSE(source.contains("_BASE_"));
}

TEST_CASE("[Modules][Haxe] a template whose parent is a project class imports that class") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Enemy.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Enemy extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);

	Vector<ScriptLanguage::ScriptTemplate> templates = language->get_built_in_templates("Node");
	REQUIRE(templates.size() == 1);
	Ref<Script> script = language->make_template(templates[0].content, "boss", "Enemy");
	REQUIRE(script.is_valid());
	const String source = script->get_source_code();
	// A project class is imported by its own package, not from `godot`.
	CHECK(source.contains("import game.Enemy;"));
	CHECK(source.contains("class Boss extends Enemy {"));
	CHECK_FALSE(source.contains("import godot.Enemy;"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a templated script gets the package of its path") {
	const String dir = make_scratch_dir();
	SourceDirScope scope(dir);
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	REQUIRE(language != nullptr);

	Ref<Script> script = language->make_template("package;\n\n_IMPORT_\n\nclass _CLASS_ extends _BASE_ {\n}\n", "coin", "Node");
	REQUIRE(script.is_valid());
	CHECK(script->get_source_code().begins_with("package;"));

	// The dialog picks the destination after the template was created, so the
	// placeholder package is replaced once the path is known.
	const String path = dir.path_join("pickups").path_join("Coin.hx");
	CHECK(HaxeProject::get_package_for_path(path) == "pickups");
	script->set_path(path);
	CHECK(script->get_source_code().begins_with("package pickups;"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] static members are never exported properties") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 1.0;\n"
			// A static has no per-instance storage in the engine, so exporting
			// it would produce a property that can never be read back.
			"\t@:export static var total:Int = 0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(path) == OK);

	List<PropertyInfo> properties;
	script->get_script_property_list(&properties);
	HashSet<String> names;
	for (const PropertyInfo &property : properties) {
		names.insert(property.name);
	}
	CHECK(names.has("speed"));
	CHECK_FALSE(names.has("total"));
	CHECK(script->get_class_info().find_member("total") == nullptr);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an empty dependency list leaves no dependencies key") {
	Vector<HaxeProject::Dependency> dependencies;
	CHECK_FALSE(HaxeProject::serialize_manifest(dependencies).contains("dependencies"));

	HaxeProject::Dependency hxcpp;
	hxcpp.name = "hxcpp";
	hxcpp.version = "4.3.2";
	dependencies.push_back(hxcpp);
	const String text = HaxeProject::serialize_manifest(dependencies);
	CHECK(text.contains("\"dependencies\""));
	CHECK(text.contains("\"hxcpp\""));
	CHECK(text.contains("4.3.2"));
}

TEST_CASE("[Modules][Haxe] haxelib progress is read from the rewritten line") {
	CHECK(HaxeBuild::parse_progress("Downloading haxelib...\n 42%\r") == 42);
	CHECK(HaxeBuild::parse_progress("  7%\r") == 7);
	CHECK(HaxeBuild::parse_progress("100%\r") == 100);
	// haxelib rewrites one line, so only the last segment holds the value.
	CHECK(HaxeBuild::parse_progress("  12%\r  63%\r") == 63);
	CHECK(HaxeBuild::parse_progress("Library installed\n") == -1);
	CHECK(HaxeBuild::parse_progress(String()) == -1);
	// A line that carries no percentage at all must not invent one.
	CHECK(HaxeBuild::parse_progress("Installing hxcpp 4.3.2\n") == -1);
}

TEST_CASE("[Modules][Haxe] export groups become inspector group entries") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:exportGroup(\"Movement\") @:exportSubgroup(\"Ground\") @:export var speed:Float = 1.0;\n"
			"\t@:exportGroup(\"Movement\") @:export var jump:Float = 2.0;\n"
			"\t@:export var armor:Int = 3;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(path) == OK);

	List<PropertyInfo> properties;
	script->get_script_property_list(&properties);

	// The inspector reads groups as properties with a group usage; the
	// heading must precede the members it applies to.
	Vector<String> order;
	String group_name;
	String subgroup_name;
	for (const PropertyInfo &property : properties) {
		if (property.usage & PROPERTY_USAGE_GROUP) {
			group_name = property.name;
			order.push_back("group:" + String(property.name));
		} else if (property.usage & PROPERTY_USAGE_SUBGROUP) {
			subgroup_name = property.name;
			order.push_back("subgroup:" + String(property.name));
		} else {
			order.push_back(String(property.name));
		}
	}
	CHECK(group_name == "Movement");
	CHECK(subgroup_name == "Ground");
	REQUIRE(order.size() == 5);
	CHECK(order[0] == "group:Movement");
	CHECK(order[1] == "subgroup:Ground");
	CHECK(order[2] == "speed");
	CHECK(order[3] == "jump");
	CHECK(order[4] == "armor");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] @:signal registers a signal instead of a method") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node;\n"
			"class Probe extends Node {\n"
			"\t@:signal function died(value:Int):Void {}\n"
			"\tfunction ping():Void {}\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(path) == OK);

	List<MethodInfo> signals;
	script->get_script_signal_list(&signals);
	REQUIRE(signals.size() == 1);
	const MethodInfo &signal = signals.front()->get();
	CHECK(signal.name == StringName("died"));
	REQUIRE(signal.arguments.size() == 1);
	CHECK(signal.arguments[0].name == StringName("value"));
	CHECK(script->has_script_signal("died"));
	CHECK_FALSE(script->has_script_signal("ping"));

	// A signal is not callable, so it must not appear in the method list.
	List<MethodInfo> methods;
	script->get_script_method_list(&methods);
	bool signal_listed_as_method = false;
	for (const MethodInfo &method : methods) {
		if (method.name == StringName("died")) {
			signal_listed_as_method = true;
		}
	}
	CHECK_FALSE(signal_listed_as_method);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an export hint exports the property on its own") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			// Every hint implies `@:export`, so requiring both annotations made
			// the hint silently do nothing.
			"\t@:exportRange(0, 500, 1) var health:Float = 100.0;\n"
			"\t@:exportEnum(\"Idle,Run\") var state:String = \"Idle\";\n"
			// One quoted value per entry is the same hint, like GDScript's
			// `@export_enum("Idle", "Run")`.
			"\t@:exportEnum(\"On\", \"Off\") var mode:Int = 0;\n"
			"\t@:exportFile(\"*.png\") var icon:String = \"\";\n"
			"\t@:exportDir var folder:String = \"\";\n"
			"\t@:exportMultiline var notes:String = \"\";\n"
			// A group only labels the members that follow it; on its own it is
			// not an export request.
			"\t@:exportGroup(\"Only a heading\") var plain:Int = 0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(path) == OK);

	List<PropertyInfo> properties;
	script->get_script_property_list(&properties);
	HashMap<String, PropertyInfo> by_name;
	for (const PropertyInfo &property : properties) {
		by_name[property.name] = property;
	}

	REQUIRE(by_name.has("health"));
	CHECK(by_name["health"].hint == PROPERTY_HINT_RANGE);
	CHECK(by_name["health"].hint_string == "0,500,1");

	REQUIRE(by_name.has("state"));
	CHECK(by_name["state"].hint == PROPERTY_HINT_ENUM);
	CHECK(by_name["state"].hint_string == "Idle,Run");

	REQUIRE(by_name.has("mode"));
	CHECK(by_name["mode"].hint == PROPERTY_HINT_ENUM);
	CHECK(by_name["mode"].hint_string == "On,Off");

	REQUIRE(by_name.has("icon"));
	CHECK(by_name["icon"].hint == PROPERTY_HINT_FILE);
	CHECK(by_name["icon"].hint_string == "*.png");

	REQUIRE(by_name.has("folder"));
	CHECK(by_name["folder"].hint == PROPERTY_HINT_DIR);

	REQUIRE(by_name.has("notes"));
	CHECK(by_name["notes"].hint == PROPERTY_HINT_MULTILINE_TEXT);

	CHECK_FALSE(by_name.has("plain"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the library list shows installed libraries haxe.json omits") {
	const String dir = make_scratch_dir();
	// A regular haxelib install: <lib>/<version>/haxelib.json + .current.
	const String hxcpp_root = dir.path_join("hxcpp").path_join("4.3.2");
	(void)write_source(hxcpp_root, "haxelib.json", "{\"name\": \"hxcpp\", \"version\": \"4.3.2\"}\n");
	(void)write_source(dir.path_join("hxcpp"), ".current", "4.3.2\n");
	// A library unpacked by hand: haxelib.json in the library directory and a
	// class path below it.
	const String addons_root = dir.path_join("addons");
	(void)write_source(addons_root, "haxelib.json", "{\"name\": \"addons\", \"classPath\": \"src\"}\n");
	DirAccess::make_dir_recursive_absolute(addons_root.path_join("src"));
	// Neither a version directory nor haxelib.json: not a library.
	DirAccess::make_dir_recursive_absolute(dir.path_join("notalib"));

	ProjectSettings *settings = ProjectSettings::get_singleton();
	const bool had_setting = settings->has_setting(HaxeProject::SETTING_HAXELIB_DIR);
	Variant previous;
	if (had_setting) {
		previous = settings->get(HaxeProject::SETTING_HAXELIB_DIR);
	}
	settings->set(HaxeProject::SETTING_HAXELIB_DIR, dir);

	Vector<HaxeBuild::Library> libraries;
	HaxeBuild::list_installed_libraries(libraries);

	if (had_setting) {
		settings->set(HaxeProject::SETTING_HAXELIB_DIR, previous);
	} else {
		settings->clear(HaxeProject::SETTING_HAXELIB_DIR);
	}

	// The panel lists what is on disk, so a library that is missing from
	// haxe.json still appears; nothing is declared here.
	REQUIRE(libraries.size() == 2);
	CHECK(libraries[0].name == "addons");
	CHECK(libraries[0].version == "dev");
	CHECK(libraries[0].path == addons_root);
	CHECK(libraries[0].class_path == addons_root.path_join("src"));
	CHECK_FALSE(libraries[0].declared);
	CHECK(libraries[1].name == "hxcpp");
	CHECK(libraries[1].version == "4.3.2");
	CHECK(libraries[1].path == hxcpp_root);
	CHECK_FALSE(libraries[1].declared);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a library with a comma encoded name and version resolves") {
	const String dir = make_scratch_dir();
	// haxelib replaces the dots of a library name with commas, so `thx.core`
	// is installed as `thx,core/`, and encodes its version directory the same
	// way (`0.44.0` becomes `0,44,0/`). `.current` and `haxelib.json` keep the
	// dotted form. Without converting both directions at the filesystem
	// boundary the library looks for `thx.core/` and the compiler gets no
	// class path.
	const String library_root = dir.path_join("thx,core");
	const String version_root = library_root.path_join("0,44,0");
	(void)write_source(version_root, "haxelib.json", "{\"name\": \"thx.core\", \"classPath\": \"src\"}\n");
	(void)write_source(library_root, ".current", "0.44.0\n");

	HaxelibDirScope scope(dir);

	Vector<HaxeBuild::Library> libraries;
	HaxeBuild::list_installed_libraries(libraries);
	REQUIRE(libraries.size() == 1);
	// The dotted name and version are reported, the form the manifest, the
	// compiler define and the editor use.
	CHECK(libraries[0].name == "thx.core");
	CHECK(libraries[0].version == "0.44.0");
	CHECK(libraries[0].path == version_root);

	Vector<String> versions;
	HaxeBuild::list_library_versions("thx.core", versions);
	REQUIRE(versions.size() == 1);
	CHECK(versions[0] == "0.44.0");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a library with a dotted version directory still resolves") {
	// A library dropped into the repository by hand can use the dotted layout
	// (`mylib/1.0.0/`) instead of haxelib's comma encoding. Both encodings
	// have to resolve, otherwise the library is reported as missing.
	const String dir = make_scratch_dir();
	const String version_root = dir.path_join("mylib").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json", "{\"name\": \"mylib\"}\n");

	HaxelibDirScope scope(dir);

	Vector<HaxeBuild::Library> libraries;
	HaxeBuild::list_installed_libraries(libraries);
	REQUIRE(libraries.size() == 1);
	CHECK(libraries[0].name == "mylib");
	CHECK(libraries[0].version == "1.0.0");
	CHECK(libraries[0].path == version_root);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the version list is dotted, newest first, and names dev") {
	const String dir = make_scratch_dir();
	// One library with two comma encoded versions, one with a single dotted
	// version, and a hand-dropped library with no version directory.
	const String multi = dir.path_join("multi");
	(void)write_source(multi.path_join("1,0,0"), "haxelib.json", "{\"name\": \"multi\"}\n");
	(void)write_source(multi.path_join("2,3,1"), "haxelib.json", "{\"name\": \"multi\"}\n");
	(void)write_source(dir.path_join("dotted").path_join("0.9.0"), "haxelib.json", "{\"name\": \"dotted\"}\n");
	(void)write_source(dir.path_join("hand"), "haxelib.json", "{\"name\": \"hand\"}\n");

	HaxelibDirScope scope(dir);

	Vector<String> versions;
	HaxeBuild::list_library_versions("multi", versions);
	REQUIRE(versions.size() == 2);
	CHECK(versions[0] == "2.3.1");
	CHECK(versions[1] == "1.0.0");

	HaxeBuild::list_library_versions("dotted", versions);
	REQUIRE(versions.size() == 1);
	CHECK(versions[0] == "0.9.0");

	HaxeBuild::list_library_versions("hand", versions);
	REQUIRE(versions.size() == 1);
	CHECK(versions[0] == "dev");

	// A library that is not installed at all has no versions.
	HaxeBuild::list_library_versions("missing", versions);
	CHECK(versions.is_empty());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] released versions are parsed from haxelib info") {
	const String output =
			"Name: thx.core\n"
			"Tags: cross, utility\n"
			"Releases:\n"
			"  2014-08-24 04:29:15 0.5.0 : first formal release\n"
			"  2019-11-02 21:03:00 0.44.0 : latest\n"
			"Description:\n"
			"  A collection of utility classes.\n";
	const Vector<String> versions = HaxeBuild::parse_available_versions(output);
	REQUIRE(versions.size() == 2);
	CHECK(versions[0] == "0.5.0");
	CHECK(versions[1] == "0.44.0");

	// A library with no releases (or output that never reaches the section)
	// yields nothing rather than a bogus entry.
	CHECK(HaxeBuild::parse_available_versions("Name: x\n").is_empty());
	CHECK(HaxeBuild::parse_available_versions("").is_empty());
}

TEST_CASE("[Modules][Haxe] export custom features become Haxe defines for a game build") {
	// A project declares custom feature tags (an export preset's custom
	// features, or the Run Instances list) and GDScript reads them with
	// `OS.has_feature`. A Haxe `#if` has to see the same names, so every tag
	// becomes a `-D` define for a game build. An editor build gets none of
	// them: the tags describe the exported game, not the editor.
	Vector<String> features;
	features.push_back("MOBILE_BUTTONS");
	features.push_back("my_feature");
	HaxeBuild::set_runtime_features(features);

	if (!HaxeSupport::is_available()) {
		HaxeBuild::set_runtime_features(Vector<String>());
		return;
	}

	const String dir = make_scratch_dir();
	(void)write_source(dir, "Main.hx",
			"package;\n"
			"import godot.Node;\n"
			"class Main extends Node {\n"
			"}\n");
	SourceDirScope scope(dir);

	const auto has_define = [](const Vector<String> &p_arguments, const String &p_define) {
		for (int i = 0; i + 1 < p_arguments.size(); i++) {
			if (p_arguments[i] == "-D" && p_arguments[i + 1] == p_define) {
				return true;
			}
		}
		return false;
	};

	Vector<HaxeBuild::Library> libraries;
	Vector<HaxeClassInfo> classes;
	Vector<Pair<String, String>> mappings;
	const Vector<String> game = HaxeBuild::collect_arguments(true, false, libraries, classes, mappings);
	CHECK(has_define(game, "MOBILE_BUTTONS"));
	CHECK(has_define(game, "my_feature"));

	Vector<HaxeClassInfo> editor_classes;
	Vector<Pair<String, String>> editor_mappings;
	const Vector<String> editor = HaxeBuild::collect_arguments(true, true, libraries, editor_classes, editor_mappings);
	CHECK_FALSE(has_define(editor, "MOBILE_BUTTONS"));
	CHECK_FALSE(has_define(editor, "my_feature"));

	// A run started from the editor ("Play" with a preset's or the Run
	// Instances dialog's features) builds the game library in a *separate*
	// process, so the setter above is not what it sees. Godot passes the
	// features to the game process through `GODOT_EDITOR_CUSTOM_FEATURES`, and
	// the compiler of that process has to turn them into defines as well.
	OS::get_singleton()->set_environment("GODOT_EDITOR_CUSTOM_FEATURES", "MOBILE_BUTTONS, editor_run");
	Vector<HaxeClassInfo> run_classes;
	Vector<Pair<String, String>> run_mappings;
	const Vector<String> run = HaxeBuild::collect_arguments(true, false, libraries, run_classes, run_mappings);
	CHECK(has_define(run, "MOBILE_BUTTONS"));
	CHECK(has_define(run, "editor_run"));

	// An editor build never gets them: the tags describe the game, not the
	// editor that is compiling it.
	const Vector<String> run_editor = HaxeBuild::collect_arguments(true, true, libraries, run_classes, run_mappings);
	CHECK_FALSE(has_define(run_editor, "editor_run"));

	OS::get_singleton()->unset_environment("GODOT_EDITOR_CUSTOM_FEATURES");
	const Vector<String> run_after = HaxeBuild::collect_arguments(true, false, libraries, run_classes, run_mappings);
	CHECK_FALSE(has_define(run_after, "editor_run"));

	HaxeBuild::set_runtime_features(Vector<String>());
	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an export preset's compiler options join the build arguments") {
	// The project settings are the baseline for every build; an export preset
	// may override the dead-code mode and add defines and raw arguments. The
	// preset's tokens have to reach the compiler *after* the project's, so they
	// can override a project default, and they have to be part of the build
	// stamp, or a preset change would silently reuse the library the previous
	// options produced.
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const Variant previous_dce = settings->get(HaxeProject::SETTING_DCE);

	settings->set(HaxeProject::SETTING_DCE, "std");
	CHECK(HaxeProject::get_dce() == "std");

	// The project setting alone: the empty `Options` resolves to it.
	const Vector<String> baseline_tokens = HaxeBuild::options_arguments(HaxeBuild::Options());
	REQUIRE(baseline_tokens.size() == 2);
	CHECK(baseline_tokens[0] == "-dce");
	CHECK(baseline_tokens[1] == "std");

	HaxeBuild::Options overrides;
	overrides.dce = "no";
	overrides.defines.push_back("RELEASE");
	overrides.defines.push_back("FEATURE=1");
	overrides.arguments.push_back("--no-opt");

	const Vector<String> tokens = HaxeBuild::options_arguments(overrides);
	REQUIRE(tokens.size() == 7);
	CHECK(tokens[0] == "-dce");
	CHECK(tokens[1] == "no");
	CHECK(tokens[2] == "-D");
	CHECK(tokens[3] == "RELEASE");
	CHECK(tokens[4] == "-D");
	CHECK(tokens[5] == "FEATURE=1");
	CHECK(tokens[6] == "--no-opt");

	// An unknown mode falls back to the project setting instead of being
	// passed to the compiler, and empty entries are dropped.
	HaxeBuild::Options bogus;
	bogus.dce = "sometimes";
	CHECK(bogus.resolved().dce == "std");
	HaxeBuild::Options padded;
	padded.defines.push_back("  A  ");
	padded.defines.push_back("   ");
	REQUIRE(padded.resolved().defines.size() == 1);
	CHECK(padded.resolved().defines[0] == "A");

	// `is_default()` compares against the project baseline, not against an
	// empty string, so an explicit mode equal to the project one is still the
	// default while a different one is not.
	CHECK(HaxeBuild::Options().is_default());
	HaxeBuild::Options same_mode;
	same_mode.dce = "std";
	CHECK(same_mode.is_default());
	CHECK_FALSE(overrides.is_default());

	// The tokens end the real argument list, after every project token.
	if (HaxeSupport::is_available()) {
		const String dir = make_scratch_dir();
		(void)write_source(dir, "Main.hx",
				"package;\n"
				"import godot.Node;\n"
				"class Main extends Node {\n"
				"}\n");
		SourceDirScope scope(dir);

		Vector<HaxeBuild::Library> libraries;
		Vector<HaxeClassInfo> classes;
		Vector<Pair<String, String>> mappings;
		const Vector<String> arguments = HaxeBuild::collect_arguments(true, false, libraries, classes, mappings, String(), String(), overrides);
		REQUIRE(arguments.size() >= tokens.size());
		const int start = arguments.size() - tokens.size();
		for (int i = 0; i < tokens.size(); i++) {
			CHECK_MESSAGE(arguments[start + i] == tokens[i], vformat("token %d of the preset options", i));
		}
		remove_scratch_dir(dir);
	}

	// The stamp is a function of the options, so a preset that changes them
	// rebuilds.
	const String stamp_default = HaxeBuild::current_stamp(false, false);
	const String stamp_override = HaxeBuild::current_stamp(false, false, String(), overrides);
	CHECK(stamp_default != stamp_override);
	CHECK(stamp_override == HaxeBuild::current_stamp(false, false, String(), overrides));

	settings->set(HaxeProject::SETTING_DCE, previous_dce);
}

TEST_CASE("[Editor][Modules][Haxe] the export preset exposes the Haxe compiler options") {
	// An ordinary Godot project must not grow a Haxe section in its export
	// dialog. A project with a Haxe script class does, and the three options
	// have to be registered with defaults that mean "leave it to the project",
	// so a preset nobody edited compiles exactly like the editor did.
	Ref<HaxeExportPlugin> plugin;
	plugin.instantiate();
	const String dir = make_scratch_dir();
	const String libs = make_scratch_dir();

	{
		SourceDirScope sources(dir);
		HaxelibDirScope libraries(libs);
		List<EditorExportPlatform::ExportOption> options;
		plugin->_get_export_options(Ref<EditorExportPlatform>(), &options);
		CHECK(options.is_empty());
	}

	(void)write_source(dir, "Main.hx",
			"package;\n"
			"import godot.Node;\n"
			"class Main extends Node {\n"
			"}\n");
	{
		SourceDirScope sources(dir);
		HaxelibDirScope libraries(libs);
		List<EditorExportPlatform::ExportOption> options;
		plugin->_get_export_options(Ref<EditorExportPlatform>(), &options);

		bool has_dce = false;
		bool has_defines = false;
		bool has_arguments = false;
		for (const EditorExportPlatform::ExportOption &option : options) {
			const StringName name = option.option.name;
			if (name == "haxe/dce") {
				has_dce = true;
				CHECK(option.option.type == Variant::INT);
				// 0 is "Project default": the index of the first enum entry.
				CHECK((int)option.default_value == 0);
			} else if (name == "haxe/defines") {
				has_defines = true;
				CHECK(option.option.type == Variant::PACKED_STRING_ARRAY);
				CHECK(PackedStringArray(option.default_value).is_empty());
			} else if (name == "haxe/extra_arguments") {
				has_arguments = true;
				CHECK(option.option.type == Variant::PACKED_STRING_ARRAY);
				CHECK(PackedStringArray(option.default_value).is_empty());
			}
		}
		CHECK(has_dce);
		CHECK(has_defines);
		CHECK(has_arguments);
	}

	remove_scratch_dir(dir);
	remove_scratch_dir(libs);
}

TEST_CASE("[Modules][Haxe] build errors are formatted with the file and position") {
	HaxeBuild::Result result;
	ScriptLanguage::ScriptError located;
	located.path = "res://source/Main.hx";
	located.line = 12;
	located.column = 4;
	located.message = "Unknown identifier : foo\n";
	result.errors.push_back(located);
	// A failure the parser could not attribute to a file (hxcpp's own output)
	// has no position and is reported as the bare message.
	ScriptLanguage::ScriptError unlocated;
	unlocated.message = "hxcpp failed to link";
	result.errors.push_back(unlocated);

	CHECK(HaxeBuild::format_errors(result) ==
			"res://source/Main.hx:12:4: Unknown identifier : foo\nhxcpp failed to link");
	CHECK(HaxeBuild::format_errors(HaxeBuild::Result()).is_empty());
}

TEST_CASE("[Modules][Haxe] a project path with spaces is aliased for hxcpp") {
	if (!HaxeSupport::is_available()) {
		return;
	}
	const String dir = make_scratch_dir();
	// Haxe passes every class path to hxcpp as an unquoted `-I<path>`
	// argument, so a path with a space is split and hxcpp treats the
	// remainder as a build target ("Could not find build target ...").
	const String spaced = dir.path_join("my game");
	(void)write_source(spaced, "Main.hx",
			"package;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n");
	SourceDirScope scope(spaced);

	Vector<HaxeBuild::Library> libraries;
	Vector<HaxeClassInfo> classes;
	Vector<Pair<String, String>> mappings;
	const Vector<String> arguments = HaxeBuild::collect_arguments(true, false, libraries, classes, mappings);

	String aliased_source;
	for (const Pair<String, String> &mapping : mappings) {
		CHECK_FALSE(mapping.first.contains(" "));
		if (mapping.second == spaced) {
			aliased_source = mapping.first;
		}
	}
	REQUIRE_FALSE(aliased_source.is_empty());
	REQUIRE(arguments.size() >= 2);
	CHECK(arguments[0] == "-cp");
	CHECK(arguments[1] == aliased_source);

	// The alias has to live on a path without spaces itself: an alias inside
	// the spaced project would be split exactly like the original path.
	const String state = HaxeProject::get_space_free_state_dir("haxe_class_path");
	CHECK_FALSE(state.is_empty());
	CHECK_FALSE(state.contains(" "));
	if (!state.is_empty()) {
		// `collect_arguments()` created the project's alias directory; drop it
		// so running the test suite leaves nothing behind.
		(void)HaxeProject::remove_directory(state.path_join(HaxeProject::get_project_root().md5_text().substr(0, 12)));
	}

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] cache cleanup removes a nested directory tree") {
	const String dir = make_scratch_dir();
	// The raw directory listing also yields "." and ".."; recursing into them
	// used to walk up the tree without ever terminating.
	const String tree = dir.path_join("cache").path_join("a").path_join("b");
	(void)write_source(tree, "notes.txt", "content\n");
	CHECK(HaxeProject::remove_directory(dir.path_join("cache")) == OK);
	CHECK_FALSE(DirAccess::dir_exists_absolute(dir.path_join("cache")));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] cache cleanup removes a symbolic link without following it") {
	const String dir = make_scratch_dir();
	const String target = dir.path_join("source");
	(void)write_source(target, "Main.hx", "class Main {}\n");
	const String cache = dir.path_join("cache");
	DirAccess::make_dir_recursive_absolute(cache);
	Ref<DirAccess> links = DirAccess::open(cache);
	REQUIRE(links.is_valid());
	// A class-path alias is a symlink to the project's source directory.
	if (links->create_link(target, "cp0") != OK) {
		// Creating links needs a privilege on some platforms; nothing to test.
		remove_scratch_dir(dir);
		return;
	}
	CHECK(links->is_link("cp0"));

	CHECK(HaxeProject::remove_directory(cache) == OK);
	CHECK_FALSE(DirAccess::dir_exists_absolute(cache));
	// Erasing the cache entry must delete the link, never its target: the
	// target is user code.
	CHECK(FileAccess::exists(target.path_join("Main.hx")));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the toolchain glue includes hxcpp before its own header") {
	if (!HaxeSupport::is_available()) {
		return;
	}
	const String dir = HaxeSupport::get_dir();
	REQUIRE_FALSE(dir.is_empty());
	Ref<FileAccess> file = FileAccess::open(dir.path_join("toolchain").path_join("glue.cpp"), FileAccess::READ);
	REQUIRE(file.is_valid());
	const String source = file->get_as_utf8_string();

	const int hxcpp = source.find("#include <hxcpp.h>");
	const int glue_header = source.find("#include \"hxgodot_glue.h\"");
	REQUIRE(hxcpp >= 0);
	REQUIRE(glue_header >= 0);
	// `hxgodot_glue.h` declares functions with hxcpp's Dynamic/String/Array
	// types and fails to compile when included first. A formatter that sorts
	// the quoted includes above the system include breaks every project build,
	// so the order is part of the contract.
	CHECK_MESSAGE(hxcpp < glue_header, "glue.cpp must include <hxcpp.h> before hxgodot_glue.h");
}

TEST_CASE("[Modules][Haxe] the glue clears the value binding only on a value type") {
	if (!HaxeSupport::is_available()) {
		return;
	}
	const String dir = HaxeSupport::get_dir();
	REQUIRE_FALSE(dir.is_empty());
	Ref<FileAccess> file = FileAccess::open(dir.path_join("toolchain").path_join("glue.cpp"), FileAccess::READ);
	REQUIRE(file.is_valid());
	const String source = file->get_as_utf8_string();

	// `make_builtin` runs `ConstructEmpty`, which skips the Haxe constructor,
	// so a value type's `__hxowner` binding keeps a stale pointer and has to be
	// cleared before the value is handed out. Only the value-type wrappers
	// declare that member: `__SetField` throws `Invalid field:` for one a class
	// does not have, which failed every packed array, `Dictionary` and `RID`
	// returned by an engine call. The guard is what lets those returns work.
	CHECK(source.find("bool class_has_value_binding(hx::Class p_class)") >= 0);
	CHECK(source.find("if (class_has_value_binding(cls)) {") >= 0);
	CHECK(source.find("g_value_binding_cache") >= 0);
	// A class that declares the binding still gets it cleared.
	CHECK(source.find("value.mPtr->__SetField(String(\"__hxowner\"), null(), HX_PROP_DYNAMIC);") >= 0);
}

TEST_CASE("[Modules][Haxe] a packed byte array crosses the wire one byte per element") {
	// The engine and the compiled glue share one tagged blob format. A byte
	// array is the only packed type that does not travel eight bytes per
	// element: it is written as raw bytes. The engine's encoder always wrote
	// it that way and the glue's `encode_container` agrees, but the decoder on
	// the glue side read a `u64` per element. A byte array returned by an
	// engine call (`FileAccess.get_file_as_bytes`) therefore came back with
	// every element shifted and the stream consumed eight times too fast.
	PackedByteArray array;
	array.resize(4);
	array.set(0, 0x00);
	array.set(1, 0x20);
	array.set(2, 0xFF);
	array.set(3, 0x41);

	Vector<uint8_t> blob;
	hx_value_encode_variant(array, blob);

	REQUIRE(blob.size() == 1 + 4 + 4);
	CHECK(blob[0] == HXG_PACKED_BYTE_ARRAY);
	// A little-endian count of four, then the four bytes unchanged.
	CHECK(blob[1] == 4);
	CHECK(blob[2] == 0);
	CHECK(blob[3] == 0);
	CHECK(blob[4] == 0);
	CHECK(blob[5] == 0x00);
	CHECK(blob[6] == 0x20);
	CHECK(blob[7] == 0xFF);
	CHECK(blob[8] == 0x41);

	// The reader on the other side has to consume one byte per element, so the
	// two ends cannot drift apart again without a test failing.
	if (!HaxeSupport::is_available()) {
		return;
	}
	const String dir = HaxeSupport::get_dir();
	Ref<FileAccess> file = FileAccess::open(dir.path_join("toolchain").path_join("glue.cpp"), FileAccess::READ);
	REQUIRE(file.is_valid());
	const String source = file->get_as_utf8_string();

	const int decoder = source.find("Dynamic decode_container(Reader &p_reader, int p_kind) {");
	REQUIRE(decoder >= 0);
	const int byte_case = source.find("case HXG_PACKED_BYTE_ARRAY: {", decoder);
	REQUIRE(byte_case >= 0);
	const int int_case = source.find("case HXG_PACKED_INT32_ARRAY:", byte_case);
	REQUIRE(int_case > byte_case);
	const String byte_block = source.substr(byte_case, int_case - byte_case);
	CHECK(byte_block.find("read_u8(value)") >= 0);
	CHECK_MESSAGE(byte_block.find("read_u64(value)") < 0, "a byte array must read one byte per element");
}

TEST_CASE("[Modules][Haxe] a script constructor binds its node before the body runs") {
	if (!HaxeSupport::is_available()) {
		return;
	}
	const String dir = HaxeSupport::get_dir();
	REQUIRE_FALSE(dir.is_empty());

	// A node placed in the editor already exists when its Haxe script is
	// constructed. The generated `super()` chain has to bind that node to the
	// wrapper before the constructor body runs, or engine calls made from
	// `new()` (the hxvlc Video constructor writes `centered` and
	// `texture_filter`) would target an unbound wrapper and report "the object
	// no longer exists".
	Ref<FileAccess> object_file = FileAccess::open(dir.path_join("std").path_join("hxgodot").path_join("NativeObject.hx"), FileAccess::READ);
	REQUIRE(object_file.is_valid());
	const String object_source = object_file->get_as_utf8_string();
	const int self_lookup = object_source.find("Native.scriptSelfId(this)");
	const int create_call = object_source.find("Native.newObject(__engineClass)");
	REQUIRE(self_lookup >= 0);
	REQUIRE(create_call >= 0);
	CHECK_MESSAGE(self_lookup < create_call, "the wrapper must ask for the script object before creating a new one");
	CHECK(object_source.find("__hxgodotId = selfId") >= 0);
	CHECK(object_source.find("Native.bindWrapper(this, selfId)") < 0);

	Ref<FileAccess> native_file = FileAccess::open(dir.path_join("std").path_join("hxgodot").path_join("Native.hx"), FileAccess::READ);
	REQUIRE(native_file.is_valid());
	const String native_source = native_file->get_as_utf8_string();
	CHECK(native_source.find("\"hxgodot_haxe_script_self_id\"") >= 0);

	Ref<FileAccess> header_file = FileAccess::open(dir.path_join("toolchain").path_join("hxgodot_glue.h"), FileAccess::READ);
	REQUIRE(header_file.is_valid());
	CHECK(header_file->get_as_utf8_string().find("String hxgodot_haxe_script_self_id(Dynamic p_wrapper);") >= 0);

	Ref<FileAccess> glue_file = FileAccess::open(dir.path_join("toolchain").path_join("glue.cpp"), FileAccess::READ);
	REQUIRE(glue_file.is_valid());
	const String glue_source = glue_file->get_as_utf8_string();
	CHECK(glue_source.find("String hxgodot_haxe_script_self_id(Dynamic p_wrapper)") >= 0);
	// The id belongs to the object whose runtime class is being constructed.
	// Matching the class is what keeps a field initializer that builds another
	// engine object from stealing the id.
	CHECK(glue_source.find("__GetClass().mPtr != t_script_class") >= 0);
	// Nested construction (a constructor that attaches another script) has to
	// restore the outer object instead of clearing the flag.
	CHECK(glue_source.find("t_script_object_id = previous_id;") >= 0);
	CHECK(glue_source.find("t_script_class = previous_class;") >= 0);
}

TEST_CASE("[Modules][Haxe] the web toolchain finds emcc through the setting, EMSDK or PATH") {
	const String dir = make_scratch_dir();
	// An emsdk layout: <root>/upstream/emscripten/emcc plus the markers that
	// identify the root.
	const String emsdk = dir.path_join("emsdk");
	const String emscripten = emsdk.path_join("upstream/emscripten");
	DirAccess::make_dir_recursive_absolute(emscripten);
	(void)write_source(emscripten, "emcc", "");
	(void)write_source(emsdk, "emsdk_env.sh", "");

	String emcc;
	String error;

	// The setting names the executable itself.
	CHECK(HaxeBuild::resolve_web_emcc(emscripten.path_join("emcc"), String(), String(), emcc, error));
	CHECK(emcc == emscripten.path_join("emcc"));
	CHECK(error.is_empty());

	// A directory is accepted as the emsdk root or as its Emscripten
	// directory, which is how a user who thinks in terms of the NDK-style
	// "installation directory" would fill the setting in.
	CHECK(HaxeBuild::resolve_web_emcc(emsdk, String(), String(), emcc, error));
	CHECK(emcc == emscripten.path_join("emcc"));
	CHECK(HaxeBuild::resolve_web_emcc(emscripten, String(), String(), emcc, error));
	CHECK(emcc == emscripten.path_join("emcc"));

	// A setting that points at nothing is reported instead of silently falling
	// back, so a typo cannot look like a missing installation.
	CHECK_FALSE(HaxeBuild::resolve_web_emcc(dir.path_join("missing"), emsdk, emscripten, emcc, error));
	CHECK(emcc.is_empty());
	CHECK(error.contains("haxe/web/emcc_path"));
	CHECK(error.contains("upstream/emscripten"));

	// Without a setting, EMSDK is used before PATH. This is the case that
	// matters when the editor was launched from a desktop icon: the variable
	// is inherited, the PATH change of `emsdk_env.sh` is not.
	CHECK(HaxeBuild::resolve_web_emcc(String(), emsdk, "/nowhere", emcc, error));
	CHECK(emcc == emscripten.path_join("emcc"));

	// PATH is the last resort.
	const String bin = dir.path_join("bin");
	DirAccess::make_dir_recursive_absolute(bin);
	(void)write_source(bin, "emcc", "");
	CHECK(HaxeBuild::resolve_web_emcc(String(), "", bin + HaxeBuild::path_list_separator() + "/usr/bin:/bin", emcc, error));
	CHECK(emcc == bin.path_join("emcc"));

	// Nothing found: the message names every source the user can configure.
	CHECK_FALSE(HaxeBuild::resolve_web_emcc(String(), String(), dir.path_join("empty"), emcc, error));
	CHECK(emcc.is_empty());
	CHECK(error.contains("haxe/web/emcc_path"));
	CHECK(error.contains("EMSDK"));
	CHECK(error.contains("4.0.11"));

	// The build environment prepends the selected directory to the child's
	// PATH and forces EMSDK to the emsdk the executable belongs to, so an
	// inherited EMSDK cannot select another Emscripten version.
	Vector<Pair<String, String>> environment = HaxeBuild::web_toolchain_environment(emscripten.path_join("emcc"));
	REQUIRE(environment.size() == 2);
	CHECK(environment[0].first == "PATH");
	CHECK(environment[0].second.contains(emscripten));
	CHECK(environment[1].first == "EMSDK");
	CHECK(environment[1].second == emsdk);

	// An emcc outside an emsdk installation only contributes the PATH entry.
	environment = HaxeBuild::web_toolchain_environment(bin.path_join("emcc"));
	REQUIRE(environment.size() == 1);
	CHECK(environment[0].first == "PATH");
	CHECK(environment[0].second.contains(bin));

	remove_scratch_dir(dir);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[Modules][Haxe] the web toolchain identity names the compiler file") {
	const String dir = make_scratch_dir();
	const String emcc = write_source(dir, "emcc", "");
	const String other = write_source(dir.path_join("other"), "emcc", "");
	REQUIRE_FALSE(emcc.is_empty());

	// The identity is part of the build stamp, so it has to be stable across
	// runs, and it has to distinguish two installations: two Emscripten
	// releases in different directories are not interchangeable even when the
	// version string happens to match.
	CHECK(HaxeBuild::web_toolchain_identity(emcc) == HaxeBuild::web_toolchain_identity(emcc));
	CHECK(HaxeBuild::web_toolchain_identity(emcc) != HaxeBuild::web_toolchain_identity(other));
	CHECK(HaxeBuild::web_toolchain_identity(emcc).begins_with(emcc + ":"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Editor][Modules][Haxe] the web toolchain path is an editable editor setting") {
	EditorSettings *settings = EditorSettings::get_singleton();
	REQUIRE(settings != nullptr);

	// The editor registers it during start-up; calling the registration here
	// makes the test independent of whether an EditorNode was created.
	HaxeBuild::register_web_editor_settings();
	REQUIRE(settings->has_setting(HaxeBuild::SETTING_WEB_EMCC));

	// The setting is machine state: the test process has already loaded the
	// user's real editor settings, so its current value is whatever that user
	// selected. Prove it is writable and restore it afterwards.
	const Variant previous = settings->get_setting(HaxeBuild::SETTING_WEB_EMCC);
	settings->set_setting(HaxeBuild::SETTING_WEB_EMCC, "sentinel-emcc");
	CHECK(String(settings->get_setting(HaxeBuild::SETTING_WEB_EMCC)) == "sentinel-emcc");
	settings->set_setting(HaxeBuild::SETTING_WEB_EMCC, previous);

	// It has to be reachable from the Editor Settings dialog with a file
	// dialog, otherwise the user can only type the path by hand. The setting
	// accepts a directory as well as the executable, so no name filter is
	// applied.
	bool has_file_hint = false;
	List<PropertyInfo> properties;
	settings->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == HaxeBuild::SETTING_WEB_EMCC) {
			has_file_hint = property.hint == PROPERTY_HINT_GLOBAL_FILE;
		}
	}
	CHECK(has_file_hint);
}
#endif

TEST_CASE("[Modules][Haxe] an implicit `this` receiver falls back to the edited class") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);
	const String mark = String::chr(0xFFFF);

	// `position` is not a local and not a type, so it must resolve as
	// `this.position` and inherit the type the base class declares for it.
	const HashSet<String> position = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tposition." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(position.has("x"));
	CHECK(position.has("y"));

	// A member of the script itself resolves the same way.
	const HashSet<String> own = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tvar target:godot.Node = null;\n"
			"\tfunction f() {\n"
			"\t\ttarget." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(own.has("add_child"));
	CHECK(own.has("get_parent"));

	// `super` is the base class instance, so its own and inherited methods
	// complete.
	const HashSet<String> parent = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\toverride public function _ready():Void {\n"
			"\t\tsuper." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(parent.has("_ready"));
	CHECK(parent.has("_process"));
	CHECK(parent.has("add_child"));
	CHECK(parent.has("get_parent"));

	// `self` still names the class being edited.
	const HashSet<String> self = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tself." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(self.has("position"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the Haxe standard library completes like any other type") {
	HaxeStdLibrary *stdlib = HaxeStdLibrary::get_singleton();
	if (!stdlib->ensure_indexed()) {
		// No Haxe installation to index; nothing to check here.
		return;
	}
	const String mark = String::chr(0xFFFF);

	// Top level standard library types appear by their short name, next to the
	// engine and project classes.
	const HashSet<String> scope = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tStringTo" +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(scope.has("StringTools"));

	const HashSet<String> array_scope = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tArra" +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(array_scope.has("Array"));

	// A value typed with a standard library class completes that class's own
	// methods, not `call()`-style engine members.
	const HashSet<String> members = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tvar list:Array<Int> = null;\n"
			"\t\tlist." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(members.has("push"));
	CHECK(members.has("length"));
	CHECK(members.has("join"));
	CHECK(members.has("concat"));
	CHECK_FALSE(members.has("add_child"));

	// Lookup opens the standard library file that declares the type.
	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("StringTools", "StringTools", String(), nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS);
	CHECK(result.script_path.begins_with(stdlib->get_std_dir()));
}

TEST_CASE("[Modules][Haxe] a file whose primary type is an enum is indexed") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Color.hx",
			"package game;\n"
			"enum Color {\n"
			"\tRed;\n"
			"\tRgb(r:Int, g:Int, b:Int);\n"
			"}\n");
	SourceDirScope scope(dir);

	// Both constructors are reached through the type, since Haxe enum values
	// are static by nature.
	const HashSet<String> constructors = completion_names(
			"package game;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tColor." +
			String::chr(0xFFFF) + "\n"
								  "\t}\n"
								  "}\n");
	CHECK(constructors.has("Red"));
	CHECK(constructors.has("Rgb"));

	// The enum is a real type: the import path offers it and lookup resolves
	// it inside its own file.
	const HashSet<String> imports = completion_names("import game.Co" + String::chr(0xFFFF) + "\n");
	CHECK(imports.has("Color"));

	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("Color", "Color", path, nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS);
	CHECK(result.script_path == path);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an omitted `:Type` is inferred from the initializer") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Probe.hx",
			"package game;\n"
			"import godot.Node;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed = 120.0;\n"
			"\t@:export var label = \"name\";\n"
			"\t@:export var count = 3;\n"
			"\t@:export var names = [];\n"
			"\t@:export var properties = {};\n"
			"\tvar target = new Node();\n"
			"\tfinal limit = 4;\n"
			"}\n");

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(path, info));

	// The initializer gives the Inspector a concrete editor instead of an
	// untyped `Nil` property.
	const HaxeMemberInfo *speed = info.find_member("speed");
	REQUIRE(speed != nullptr);
	CHECK(speed->type == "Float");
	CHECK(speed->to_property_info().type == Variant::FLOAT);

	const HaxeMemberInfo *label = info.find_member("label");
	REQUIRE(label != nullptr);
	CHECK(label->type == "String");
	CHECK(label->to_property_info().type == Variant::STRING);

	const HaxeMemberInfo *count = info.find_member("count");
	REQUIRE(count != nullptr);
	CHECK(count->type == "Int");

	// A collection literal carries the container kind, not the element type.
	const HaxeMemberInfo *names = info.find_member("names");
	REQUIRE(names != nullptr);
	CHECK(names->type == "Array<Dynamic>");

	const HaxeMemberInfo *properties = info.find_member("properties");
	REQUIRE(properties != nullptr);
	CHECK(properties->type == "Dictionary");

	// A plain field and a `final` field are parsed as well, so completion can
	// reach the constructed class's members.
	const HaxeMemberInfo *target = info.find_variable("target");
	REQUIRE(target != nullptr);
	CHECK(target->type == "Node");
	const HaxeMemberInfo *limit = info.find_variable("limit");
	REQUIRE(limit != nullptr);
	CHECK(limit->type == "Int");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a local without `:Type` completes through its initializer") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);
	const String mark = String::chr(0xFFFF);

	// `var n = new Node()` has no annotation, but the members of `Node` still
	// complete after the dot.
	const HashSet<String> names = completion_names(
			"package game;\n"
			"import godot.Node;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tvar n = new Node();\n"
			"\t\tn." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(names.has("add_child"));
	CHECK(names.has("get_parent"));
	CHECK(names.has("queue_free"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a call-result receiver completes the returned type") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);
	const String mark = String::chr(0xFFFF);

	// `get_parent()` returns a `Node`. `add_child` and `call_deferred` are the
	// members of the deferred-call idiom (`add_child.call_deferred(child)`)
	// that a Haxe call site writes as `get_parent().add_child(child)`.
	const HashSet<String> parent = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tget_parent()." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(parent.has("add_child"));
	CHECK(parent.has("call_deferred"));
	CHECK(parent.has("get_children"));
	CHECK(parent.has("queue_free"));

	// A chained call walks through each returned type.
	const HashSet<String> chained = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tget_parent().get_parent()." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(chained.has("add_child"));

	// A constructor call names a type, so its value's members complete too.
	const HashSet<String> vector = completion_names(
			"import godot.Vector2;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tVector2(1, 2)." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(vector.has("x"));
	CHECK(vector.has("y"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a typed accessor and `cast` refine the receiver type") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir.path_join("game"), "Main.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"}\n");
	SourceDirScope scope(dir);
	const String mark = String::chr(0xFFFF);

	// `get_node("Path", Sprite2D)` names the value's type with a trailing class
	// argument, the way the generated binding's `Class<T>` overload does, so the
	// members of `Sprite2D` complete instead of the broad `Node` members.
	const HashSet<String> typed = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"import godot.Sprite2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tget_node(\"Hero\", Sprite2D)." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(typed.has("texture"));
	CHECK(typed.has("centered"));
	CHECK(typed.has("frame"));

	// `cast(value, Type)` names its type directly, here over the result of
	// another call rather than over a plain identifier.
	const HashSet<String> casted = completion_names(
			"package game;\n"
			"import godot.Node2D;\n"
			"import godot.Sprite2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tcast(get_node(\"Hero\"), Sprite2D)." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			path);
	CHECK(casted.has("texture"));
	CHECK(casted.has("centered"));
	CHECK(casted.has("frame"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] completion resolves a class from an installed library") {
	const String dir = make_scratch_dir();
	// A project-local haxelib repository holding one library, in the layout
	// `haxelib install` produces: `<repository>/<name>/<version>/haxelib.json`
	// with the class path recorded there.
	const String haxelib_root = dir.path_join("haxelib");
	const String version_root = haxelib_root.path_join("mylib").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json",
			"{\n"
			"\t\"name\": \"mylib\",\n"
			"\t\"classPath\": \"src\"\n"
			"}\n");
	const String widget_path = write_source(version_root.path_join("src").path_join("mylib"), "Widget.hx",
			"package mylib;\n"
			"class Widget {\n"
			"\tpublic var label:String;\n"
			"\tpublic var count:Int;\n"
			"\tpublic static function create():Widget {\n"
			"\t\treturn new Widget();\n"
			"\t}\n"
			"\tpublic function resize(width:Int):Void {}\n"
			"}\n");

	HaxelibDirScope scope(haxelib_root);
	const String mark = String::chr(0xFFFF);

	// The index follows the class path recorded in `haxelib.json`, not a
	// hard coded location, and resolves a bare name when it is unambiguous.
	HaxeLibraryIndex *libraries = HaxeLibraryIndex::get_singleton();
	REQUIRE(libraries->ensure_indexed());
	CHECK(libraries->has_type("mylib.Widget"));
	CHECK(libraries->has_type("Widget"));
	HaxeClassInfo info;
	REQUIRE(libraries->get_type_info("mylib.Widget", info));
	CHECK(info.find_variable("label") != nullptr);
	CHECK(info.find_function("resize") != nullptr);

	// `import` completion offers the library's root package and its types.
	const HashSet<String> roots = completion_names("import " + mark + "\n");
	CHECK(roots.has("mylib"));
	const HashSet<String> package = completion_names("import mylib." + mark + "\n");
	CHECK(package.has("Widget"));

	// A type position offers the library class by its full name.
	const HashSet<String> types = completion_names(
			"class Main {\n"
			"\tvar w:" +
			mark + "\n"
				   "}\n");
	CHECK(types.has("mylib.Widget"));

	// A variable typed with the imported short name completes its instance
	// members, without the static ones.
	const HashSet<String> members = completion_names(
			"import mylib.Widget;\n"
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tvar w:Widget;\n"
			"\t\tw." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(members.has("label"));
	CHECK(members.has("count"));
	CHECK(members.has("resize"));
	CHECK_FALSE(members.has("create"));

	// A definition lookup jumps into the library file.
	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("Widget.", "label", String(), nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY);
	CHECK(result.script_path == widget_path);

	// Refreshing the project repository drops the cached index: with the
	// library gone its class is no longer offered. `remove_scratch_dir`
	// deletes the tree; the index is invalidated the way the Haxelib manager
	// does after an uninstall.
	remove_scratch_dir(dir);
	libraries->invalidate();
	CHECK_FALSE(libraries->ensure_indexed());
	CHECK_FALSE(libraries->has_type("mylib.Widget"));
}

TEST_CASE("[Modules][Haxe] the packaged library path shares the build name") {
	// An exported game loads the library from `res://haxe/<Name>.<ext>`, and the
	// build writes `<Name>.<ext>`; both derive `<Name>` from the project name the
	// same way, so a name that is not a valid identifier must be sanitized
	// identically in the two paths or the export would package a file the
	// runtime never looks for.
	//
	// The extension is the one hxcpp's host toolchain links (`.dso` on Linux,
	// `.dll` under MSVC, `.dylib` on macOS), so it is pinned per platform here
	// rather than hardcoded: a build that started writing the wrong extension
	// would still ship a file the runtime never opens.
#if defined(WINDOWS_ENABLED)
	const String extension = "dll";
#elif defined(MACOS_ENABLED)
	const String extension = "dylib";
#else
	const String extension = "dso";
#endif

	ProjectSettings *settings = ProjectSettings::get_singleton();
	const bool had_name = settings->has_setting("application/config/name");
	const Variant previous = had_name ? settings->get_setting("application/config/name") : Variant();

	settings->set_setting("application/config/name", "New Game Project");
	const String built = HaxeProject::get_output_library_path(true, false).get_file();
	CHECK(built == "New_Game_Project." + extension);
	// The export packages exactly the file the build produced, under
	// `res://haxe/`, which is where the runtime looks it up.
	CHECK(HaxeProject::get_packaged_library_path() == "res://haxe/" + built);

	settings->set_setting("application/config/name", "9lives.game");
	CHECK(HaxeProject::get_packaged_library_path() == "res://haxe/G9lives_game." + extension);

	if (had_name) {
		settings->set_setting("application/config/name", previous);
	} else {
		settings->clear("application/config/name");
	}
}

#ifdef TOOLS_ENABLED
TEST_CASE("[Editor][Modules][Haxe] conditional compilation lines use the comment colour") {
	Ref<HaxeSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	TextEdit *text_edit = memnew(TextEdit);
	text_edit->set_text("#if DEBUG\n// a comment\n\t#else\n#end\n");
	highlighter->set_text_edit(text_edit);

	// A directive line is a single run colored like a comment, from its `#`
	// (past any leading indentation) to the end of the line.
	const Dictionary directive = highlighter->get_line_syntax_highlighting(0);
	REQUIRE(directive.has(0));
	CHECK(directive.size() == 1);

	const Dictionary indented = highlighter->get_line_syntax_highlighting(2);
	REQUIRE(indented.has(1));

	const Dictionary comment = highlighter->get_line_syntax_highlighting(1);
	REQUIRE(comment.has(0));

	const Color directive_color = Dictionary(directive[0])["color"];
	const Color indented_color = Dictionary(indented[1])["color"];
	const Color comment_color = Dictionary(comment[0])["color"];
	CHECK(directive_color == comment_color);
	CHECK(indented_color == comment_color);

	memdelete(text_edit);
}
#endif

#ifdef TOOLS_ENABLED
TEST_CASE("[Modules][Haxe] syntax highlighter targets the Haxe language") {
	Ref<HaxeSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	CHECK(highlighter->_get_name() == "Haxe");
	CHECK(highlighter->_get_supported_languages().has("Haxe"));

	Ref<EditorSyntaxHighlighter> clone = highlighter->_create();
	REQUIRE(clone.is_valid());
	CHECK(clone->_get_name() == "Haxe");
}
#endif

#ifdef TOOLS_ENABLED
/* Writes a zip archive holding a single compressed entry, the way an APK or an
 * AAR stores a native library. Godot's Android templates enable
 * `useLegacyPackaging` for the minimum SDK it supports, so the libraries inside
 * them are deflated and a raw byte scan does not see their contents. */
static String write_zip_entry(const String &p_dir, const String &p_name, const String &p_entry, const String &p_payload) {
	const String path = p_dir.path_join(p_name);
	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	zipFile archive = zipOpen2(path.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);
	ERR_FAIL_NULL_V(archive, String());
	const CharString entry = p_entry.utf8();
	const CharString payload = p_payload.utf8();
	ERR_FAIL_COND_V(zipOpenNewFileInZip(archive, entry.get_data(), nullptr, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_COMPRESSION) != ZIP_OK, String());
	ERR_FAIL_COND_V(zipWriteInFileInZip(archive, payload.get_data(), payload.length()) != ZIP_OK, String());
	ERR_FAIL_COND_V(zipCloseFileInZip(archive) != ZIP_OK, String());
	ERR_FAIL_COND_V(zipClose(archive, nullptr) != ZIP_OK, String());
	return path;
}

TEST_CASE("[Editor][Modules][Haxe] Android templates are searched through their archives") {
	const String dir = make_scratch_dir();
	const String filler = String("x").repeat(40000);
	const String payload = filler + "HaxeScript" + filler;
	const String haxe_apk = write_zip_entry(dir, "haxe.apk", "lib/arm64-v8a/libGodotAndroid.so", payload);
	const String stock_apk = write_zip_entry(dir, "stock.apk", "lib/arm64-v8a/libGodotAndroid.so", filler + filler);
	const String haxe_aar = write_zip_entry(dir, "haxe.aar", "jni/arm64-v8a/libGodotAndroid.so", payload);
	const String host_binary = write_source(dir, "godot.linuxbsd.template_release.x86_64", "ELF payload HaxeScript\n");
	const String empty_binary = write_source(dir, "godot.stock", "ELF payload without the module\n");
	REQUIRE_FALSE(haxe_apk.is_empty());

	// The marker of a compressed entry is invisible in the archive bytes, which
	// is why the check has to inflate every library entry instead of scanning
	// the file as it is.
	const Vector<uint8_t> raw = FileAccess::get_file_as_bytes(haxe_apk);
	REQUIRE(raw.size() < payload.utf8().length());
	bool raw_has_marker = false;
	const char marker[] = "HaxeScript";
	for (int i = 0; i + (int)strlen(marker) <= raw.size(); i++) {
		if (memcmp(raw.ptr() + i, marker, strlen(marker)) == 0) {
			raw_has_marker = true;
			break;
		}
	}
	CHECK_FALSE(raw_has_marker);

	CHECK(HaxeExportPlugin::template_contains_haxe(haxe_apk));
	CHECK(HaxeExportPlugin::template_contains_haxe(haxe_aar));
	CHECK_FALSE(HaxeExportPlugin::template_contains_haxe(stock_apk));
	// The host templates are plain executables, not archives.
	CHECK(HaxeExportPlugin::template_contains_haxe(host_binary));
	CHECK_FALSE(HaxeExportPlugin::template_contains_haxe(empty_binary));
	// A missing path is not a template that can be exported to.
	CHECK_FALSE(HaxeExportPlugin::template_contains_haxe(dir.path_join("absent.apk")));
	// Directories are searched recursively, as a Gradle build template keeps
	// the engine library several directories deep (app/libs/<target>/*.aar).
	CHECK(HaxeExportPlugin::template_contains_haxe(dir));

	remove_scratch_dir(dir);
}

TEST_CASE("[Editor][Modules][Haxe] the host API version of an export template is checked") {
	// The engine binary carries its host API marker; the export refuses a
	// template built for another version instead of shipping a game that fails
	// with "incompatible host API version". The running engine's own binary has
	// to carry the marker too — `print_verbose()` in the module init is what
	// keeps it there, so this also guards against it being dropped.
	const String executable = OS::get_singleton()->get_executable_path();
	CHECK(HaxeExportPlugin::template_host_abi_matches(executable));
	CHECK(HaxeExportPlugin::template_declares_host_abi(executable));

	const String dir = make_scratch_dir();
	const String matching = write_source(dir, "matches.bin", String("ELF ") + HXGODOT_TEMPLATE_ABI_MARKER + " tail\n");
	const String other = write_source(dir, "other.bin", "ELF hxgodot-abi=999; tail\n");
	const String older = write_source(dir, "older.bin", "ELF payload HaxeScript, no marker\n");
	// A marker for another version is found by the prefix search but does not
	// match this engine.
	CHECK_FALSE(HaxeExportPlugin::template_host_abi_matches(other));
	CHECK(HaxeExportPlugin::template_declares_host_abi(other));
	CHECK_FALSE(HaxeExportPlugin::template_host_abi_matches(older));
	CHECK_FALSE(HaxeExportPlugin::template_declares_host_abi(older));
	CHECK(HaxeExportPlugin::template_host_abi_matches(matching));
	// The trailing `;` keeps `...=4;` from matching a longer version number.
	CHECK_FALSE(HaxeExportPlugin::template_host_abi_matches(write_source(dir, "long.bin", "ELF hxgodot-abi=40; tail\n")));

	remove_scratch_dir(dir);
}
#endif

#ifdef TOOLS_ENABLED
TEST_CASE("[Editor][Modules][Haxe] a web template's thread variant is read from its JavaScript") {
	const String dir = make_scratch_dir();
	const String threaded = write_zip_entry(dir, "threaded.zip", "godot.js",
			"var f = wasmExports[\"_emscripten_dlsync_threads\"];");
	const String single = write_zip_entry(dir, "single.zip", "godot.js",
			"var mode = \"single-threaded\";");
	const String no_js = write_zip_entry(dir, "side-only.zip", "godot.side.wasm", "Godot");
	const String missing = dir.path_join("missing.zip");
	REQUIRE_FALSE(threaded.is_empty());

	// Emscripten only emits the dlsync thread entry point in a `-pthread`
	// build. The check exists because a custom template of the other variant
	// makes the browser reject the Haxe module with "mismatch in shared state
	// of memory" instead of reporting an export error.
	CHECK(HaxeExportPlugin::web_template_threads(threaded) == 1);
	CHECK(HaxeExportPlugin::web_template_threads(single) == 0);
	// Without the engine JavaScript there is nothing to classify.
	CHECK(HaxeExportPlugin::web_template_threads(no_js) == -1);
	CHECK(HaxeExportPlugin::web_template_threads(missing) == -1);

	remove_scratch_dir(dir);
}
#endif

TEST_CASE("[Modules][Haxe] editing one script does not reparse the whole project") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Base.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"}\n");
	const String base_path = dir.path_join("game").path_join("Base.hx");
	const String child_path = write_source(dir.path_join("game"), "Child.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"import game.Base;\n"
			"class Child extends Base {\n"
			"\t@:export var speed:Float = 3.0;\n"
			"}\n");
	SourceDirScope scope(dir);

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(child_path, info));
	// Resolving the base chain of `Child` reads the index, which is now built.
	const uint64_t rebuilds = HaxeProject::get_index_rebuild_count();

	// Reloading a script is what the editor does after a save: it invalidates
	// that file and resolves the base chain again. Invalidating one file must
	// not throw away the project index, or every reload re-reads every source
	// in the project (which made opening a large project slower over time).
	Ref<HaxeScript> child;
	child.instantiate();
	child->set_path(child_path, true);
	REQUIRE(child->is_valid());
	for (int i = 0; i < 3; i++) {
		HaxeProject::invalidate(base_path);
		REQUIRE(child->reload() == OK);
	}

	CHECK(HaxeProject::get_index_rebuild_count() == rebuilds);
	// The base chain still resolves after the invalidation.
	CHECK(child->get_native_base_type() == "Node2D");

	remove_scratch_dir(dir);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[Modules][Haxe] an unsaved base edit refreshes inherited exports") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("game"), "Base.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"}\n");
	const String base_path = dir.path_join("game").path_join("Base.hx");
	const String child_path = write_source(dir.path_join("game"), "Child.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"import game.Base;\n"
			"class Child extends Base {\n"
			"\t@:export var speed:Float = 3.0;\n"
			"}\n");
	SourceDirScope scope(dir);

	Ref<HaxeScript> base;
	base.instantiate();
	base->set_path(base_path, true);
	REQUIRE(base->is_valid());

	Ref<HaxeScript> child;
	child.instantiate();
	child->set_path(child_path, true);
	REQUIRE(child->is_valid());

	Node *node = memnew(Node);
	PlaceHolderScriptInstance *placeholder = child->placeholder_instance_create(node);
	REQUIRE(placeholder != nullptr);
	node->set_script_instance(placeholder);

	auto placeholder_names = [&]() {
		List<PropertyInfo> properties;
		placeholder->get_property_list(&properties);
		HashSet<String> names;
		for (const PropertyInfo &property : properties) {
			names.insert(property.name);
		}
		return names;
	};

	CHECK(placeholder_names().has("health"));
	CHECK_FALSE(placeholder_names().has("armor"));

	// The script editor parses the text as it is typed and calls
	// `update_exports()` without saving. The subclass has to see the new
	// inherited member immediately, or its Inspector keeps the old set until
	// the next reload.
	base->set_source_code(
			"package game;\n"
			"import godot.Node2D;\n"
			"class Base extends Node2D {\n"
			"\t@:export var health:Int = 25;\n"
			"\t@:export var armor:Int = 5;\n"
			"}\n");
	base->update_exports();

	const HashSet<String> after = placeholder_names();
	CHECK(after.has("health"));
	CHECK(after.has("armor"));

	memdelete(node);
	remove_scratch_dir(dir);
}
#endif

namespace {

/* Runs a block in the state an exported game sees: the source index comes
 * from the packaged manifest instead of the `.hx` files. */
class PackagedMetadataScope {
public:
	PackagedMetadataScope() {
		HaxeProject::set_packaged_metadata_for_testing(true);
		HaxeProject::reset_packaged_metadata_state();
		HaxeProject::invalidate();
	}

	~PackagedMetadataScope() {
		HaxeProject::set_packaged_metadata_for_testing(false);
		HaxeProject::reset_packaged_metadata_state();
		HaxeProject::invalidate();
	}
};

} // namespace

TEST_CASE("[Modules][Haxe] an exported game reads script metadata without the source") {
	const String dir = make_scratch_dir();
	const String path = write_source(dir, "Probe.hx",
			"package game;\n"
			"import godot.Node2D;\n"
			"class Probe extends Node2D {\n"
			"\t@:export var speed:Float = 1.5;\n"
			"\t@:export var armor:Int = 5;\n"
			"\t@:signal function health_changed(value:Int):Void {}\n"
			"}\n");
	SourceDirScope scope(dir);

	// The exporter serializes the class descriptions, then replaces each `.hx`
	// with an empty stub.
	const String document = HaxeProject::serialize_script_metadata();
	{
		JSON json;
		REQUIRE(json.parse(document) == OK);
		Dictionary root = json.get_data();
		REQUIRE(root.has("scripts"));
		Dictionary scripts = root["scripts"];
		REQUIRE(scripts.has(path));
	}

	// Blank the file the way the export does, then load the manifest.
	(void)write_source(dir, "Probe.hx", "\n");
	const String manifest = dir.path_join("scripts.json");
	{
		Ref<FileAccess> file = FileAccess::open(manifest, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string(document);
	}

	PackagedMetadataScope state;
	REQUIRE(HaxeProject::load_script_metadata(manifest));

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(path, info));
	CHECK(info.name == "Probe");
	CHECK(info.base == "Node2D");

	HashSet<String> members;
	for (const HaxeMemberInfo &member : info.members) {
		members.insert(member.name);
	}
	CHECK(members.has("speed"));
	CHECK(members.has("armor"));

	const HaxeFunctionInfo *signal = info.find_function("health_changed");
	REQUIRE(signal != nullptr);
	CHECK(signal->is_signal);

	// Loading the stub resolves the class from the manifest instead of trying
	// to parse the empty file.
	Ref<HaxeScript> script;
	script.instantiate();
	CHECK(script->load_source_code(path) == OK);
	REQUIRE(script->is_valid());
	CHECK(script->get_native_base_type() == "Node2D");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a build with no packaged manifest is not an exported game") {
	// On a non-editor build `uses_packaged_metadata()` decides from the game
	// itself. Without the manifest an export ships, a template built from
	// source -- including the `tests=yes` templates the engine test suite runs
	// in -- has to keep reading its real `.hx` files. Deciding this from the
	// build options instead sent a source template down the packaged path and
	// an export of it lost every script.
	HaxeProject::set_packaged_metadata_for_testing(false);
	HaxeProject::reset_packaged_metadata_state();
	CHECK_FALSE(HaxeProject::uses_packaged_metadata());
	// Restore the default so the following cases re-decide for themselves.
	HaxeProject::set_packaged_metadata_for_testing(false);
	HaxeProject::reset_packaged_metadata_state();
}

TEST_CASE("[Modules][Haxe] an exported game does not scan the on-disk library index") {
	// The library index walks the project's `haxelib/` directory. In an
	// exported game that directory is a pack entry holding empty `.hx` stubs
	// and `globalize_path("res://")` has no real directory to resolve, so the
	// scan could only fail -- which is what produced the spurious `haxe.json`
	// and `/haxelib` errors on Android. The packaged manifest is the game's
	// index instead, so a game must skip the scan entirely.
	const String dir = make_scratch_dir();
	const String haxelib_root = dir.path_join("haxelib");
	const String version_root = haxelib_root.path_join("mylib").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json",
			"{\n"
			"\t\"name\": \"mylib\",\n"
			"\t\"classPath\": \"src\"\n"
			"}\n");
	(void)write_source(version_root.path_join("src").path_join("mylib"), "Widget.hx",
			"package mylib;\n"
			"class Widget {\n"
			"\tpublic var label:String;\n"
			"}\n");

	HaxelibDirScope scope(haxelib_root);

	// The library is on disk, so the editor's index finds it.
	HaxeLibraryIndex *libraries = HaxeLibraryIndex::get_singleton();
	REQUIRE(libraries->ensure_indexed());
	CHECK(libraries->has_type("mylib.Widget"));

	// A game leaves the index empty: the same on-disk library is not indexed
	// and nothing touches `haxelib/` (or the machine-wide repository).
	{
		PackagedMetadataScope state;
		libraries->invalidate();
		CHECK_FALSE(libraries->ensure_indexed());
		CHECK_FALSE(libraries->has_type("mylib.Widget"));
	}

	// Leaving the scope restores the editor behaviour.
	libraries->invalidate();
	REQUIRE(libraries->ensure_indexed());
	CHECK(libraries->has_type("mylib.Widget"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] Android libraries use the name the linker resolves") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const bool had_name = settings->has_setting("application/config/name");
	const Variant previous = had_name ? settings->get_setting("application/config/name") : Variant();
	settings->set_setting("application/config/name", "9lives.game");

	// Godot's Android exporter requires a `lib` prefix and the dynamic linker
	// resolves the packaged library by that soname, so the version moves into
	// the directory instead of the file name.
	CHECK(HaxeProject::get_library_file_name("arm64-v8a") == "libG9lives_game.so");
	CHECK(HaxeProject::get_android_library_soname() == "libG9lives_game.so");
	const String versioned = HaxeProject::get_versioned_library_path(true, false, "0123456789abcdef", "arm64-v8a");
	CHECK(versioned.get_file() == "libG9lives_game.so");
	CHECK(versioned.get_base_dir().get_file() == "0123456789ab");
	// hxcpp itself appends the architecture (`LIBEXTRA`) to the file it links,
	// so the build has to look for those names in the `cpp` directory even
	// though the packaged library is always `lib<Name>.so`.
	CHECK(HaxeProject::get_hxcpp_output_file_name("arm64-v8a") == "libG9lives_game-64.so");
	CHECK(HaxeProject::get_hxcpp_output_file_name("armeabi-v7a") == "libG9lives_game-v7.so");
	CHECK(HaxeProject::get_hxcpp_output_file_name("x86") == "libG9lives_game-x86.so");
	CHECK(HaxeProject::get_hxcpp_output_file_name("x86_64") == "libG9lives_game-x86_64.so");
	CHECK(HaxeProject::get_hxcpp_output_file_name() == HaxeProject::get_library_file_name());
	CHECK(HaxeProject::get_output_library_path(true, false, "arm64-v8a").get_file() == "libG9lives_game-64.so");
	CHECK(HaxeProject::get_output_library_path(true, false, "arm64-v8a").get_base_dir().get_file() == "cpp");
	// Every ABI has its own object and library directory.
	CHECK(HaxeProject::get_config_dir(false, false, "arm64-v8a").contains("android"));
	CHECK(HaxeProject::get_config_dir(false, false, "x86_64") != HaxeProject::get_config_dir(false, false, "arm64-v8a"));
	// The host keeps the platform extension and no prefix.
	CHECK_FALSE(HaxeProject::get_library_file_name().begins_with("lib"));

	const Vector<String> arm64 = HaxeBuild::android_compiler_defines("arm64-v8a", "/opt/android-ndk");
	CHECK(arm64.has("android"));
	CHECK(arm64.has("HXCPP_ARM64"));
	CHECK(arm64.has("PLATFORM=android-24"));
	CHECK(arm64.has("ANDROID_NDK_ROOT=/opt/android-ndk"));
	CHECK_FALSE(arm64.has("HXCPP_ARMV7"));

	const Vector<String> armv7 = HaxeBuild::android_compiler_defines("armeabi-v7a", String());
	CHECK(armv7.has("HXCPP_ARMV7"));
	CHECK_FALSE(armv7.has("HXCPP_ARM64"));

	// An unknown ABI produces no defines, which the build reports as an error
	// instead of linking for the wrong architecture.
	CHECK(HaxeBuild::android_compiler_defines("mips", String()).is_empty());

	if (had_name) {
		settings->set_setting("application/config/name", previous);
	} else {
		settings->clear("application/config/name");
	}
}

TEST_CASE("[Modules][Haxe] a desktop build pins the architecture of the host") {
	// hxcpp normally infers the host architecture itself: `uname -m` on POSIX,
	// the `PROCESSOR_ARCHITECTURE` variables on Windows. On Windows the latter
	// can report the emulated 32-bit architecture even though the editor is
	// 64-bit, and the resulting 32-bit library cannot be loaded (`error 193`,
	// ERROR_BAD_EXE_FORMAT). The engine knows its own architecture at compile
	// time, so the desktop build passes the matching hxcpp define explicitly.
	const Vector<String> defines = HaxeBuild::host_arch_defines();
	REQUIRE(defines.size() == 2);
	CHECK(defines[0] == "-D");
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
	CHECK(defines[1] == "HXCPP_ARM64");
#elif defined(_WIN64) || defined(__x86_64__) || defined(__amd64__) || defined(__LP64__)
	CHECK(defines[1] == "HXCPP_M64");
#else
	CHECK(defines[1] == "HXCPP_M32");
#endif

	if (!HaxeSupport::is_available()) {
		return;
	}

	// The define has to reach the real compiler arguments, not just the helper.
	const String dir = make_scratch_dir();
	(void)write_source(dir, "Main.hx",
			"package;\n"
			"import godot.Node;\n"
			"class Main extends Node {\n"
			"}\n");
	SourceDirScope scope(dir);

	Vector<HaxeBuild::Library> libraries;
	Vector<HaxeClassInfo> classes;
	Vector<Pair<String, String>> mappings;
	const Vector<String> arguments = HaxeBuild::collect_arguments(true, false, libraries, classes, mappings);

	bool pinned = false;
	for (int i = 0; i + 1 < arguments.size(); i++) {
		if (arguments[i] == "-D" && arguments[i + 1] == defines[1]) {
			pinned = true;
		}
	}
	CHECK(pinned);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a GDScript class is parsed into the shared class description") {
	HaxeClassInfo info;
	REQUIRE(parse_gdscript_source(
					"class_name Hud\n"
					"extends Node2D\n"
					"\n"
					"signal died\n"
					"const MAX_HEALTH = 100\n"
					"static var shared_count: int = 0\n"
					"@export var health: int = 10\n"
					"var label: String = \"x\"\n"
					"var weights: Array[float]\n"
					"var tail := \"a\"\n"
					"\n"
					"static func create_hud(p: int) -> Hud:\n"
					"\treturn null\n"
					"func refresh(amount: float, scale: float = 2.0, unused) -> void:\n"
					"\tpass\n",
					"res://hud.gd", info) == OK);
	CHECK(info.valid);
	CHECK(info.name == "Hud");
	CHECK(info.base == "Node2D");

	const HaxeFunctionInfo *create = info.find_function("create_hud");
	REQUIRE(create != nullptr);
	CHECK(create->is_static);
	CHECK(create->return_type == "Hud");
	REQUIRE(create->arguments.size() == 1);
	CHECK(create->arguments[0].name == "p");
	CHECK(create->arguments[0].type == "Int");
	CHECK_FALSE(create->arguments[0].optional);

	const HaxeFunctionInfo *refresh = info.find_function("refresh");
	REQUIRE(refresh != nullptr);
	CHECK_FALSE(refresh->is_static);
	CHECK(refresh->return_type == "Void");
	REQUIRE(refresh->arguments.size() == 3);
	CHECK(refresh->arguments[0].type == "Float");
	CHECK_FALSE(refresh->arguments[0].optional);
	CHECK(refresh->arguments[1].optional);
	CHECK(refresh->arguments[2].type == "Dynamic");

	const HaxeMemberInfo *health = info.find_variable("health");
	REQUIRE(health != nullptr);
	CHECK(health->type == "Int");
	CHECK(health->exported);
	CHECK(info.find_member("health") != nullptr);

	const HaxeMemberInfo *shared = info.find_variable("shared_count");
	REQUIRE(shared != nullptr);
	CHECK(shared->is_static);
	CHECK(shared->type == "Int");
	CHECK_FALSE(shared->constant);

	const HaxeMemberInfo *max_health = info.find_variable("MAX_HEALTH");
	REQUIRE(max_health != nullptr);
	CHECK(max_health->is_static);
	CHECK(max_health->constant);
	CHECK(max_health->type == "Int");

	const HaxeMemberInfo *weights = info.find_variable("weights");
	REQUIRE(weights != nullptr);
	CHECK(weights->type == "Array<Float>");

	const HaxeMemberInfo *tail = info.find_variable("tail");
	REQUIRE(tail != nullptr);
	CHECK(tail->type == "String");

	const HaxeMemberInfo *died = info.find_variable("died");
	REQUIRE(died != nullptr);
	CHECK(died->type == "godot.Signal");
}

TEST_CASE("[Modules][Haxe] a GDScript stub dispatches through the engine bindings") {
	HaxeClassInfo info;
	REQUIRE(parse_gdscript_source(
					"class_name Hud\n"
					"extends Node2D\n"
					"@export var health: int = 10\n"
					"static var shared_count: int = 0\n"
					"const MAX_HEALTH = 100\n"
					"static func create_hud(p: int) -> Hud:\n"
					"\treturn null\n"
					"func refresh(amount: float, scale: float = 2.0) -> void:\n"
					"\tpass\n",
					"res://hud.gd", info) == OK);

	HaxeGDScriptEntry entry;
	entry.name = "Hud";
	entry.path = "res://hud.gd";
	entry.declared_base = "Node2D";
	entry.native_base = "Node2D";
	entry.stub_base = "godot.Node2D";

	HashSet<String> reserved;
	const String stub = generate_gdscript_stub(entry, info, reserved);
	CHECK(stub.contains("class Hud extends godot.Node2D"));
	// Static calls go through the script resource, instance calls through the
	// node's object id.
	CHECK(stub.contains("static function create_hud(p:Int):Hud"));
	CHECK(stub.contains("hxgodot.Native.call(__hxgodotScript().__hxgodotId, \"create_hud\", [p])"));
	CHECK(stub.contains("function refresh(amount:Float, ?scale:Float):Void"));
	CHECK(stub.contains("hxgodot.Native.call(__hxgodotId, \"refresh\", __args)"));
	CHECK(stub.contains("while (__args.length > 0 && __args[__args.length - 1] == null)"));
	// A variable becomes a property that reads and writes the engine property.
	CHECK(stub.contains("public var health(get, set):Int"));
	CHECK(stub.contains("hxgodot.Native.getProperty(__hxgodotId, \"health\")"));
	CHECK(stub.contains("hxgodot.Native.setProperty(__hxgodotId, \"health\", value)"));
	// A `static var` is writable; a `const` is only readable.
	CHECK(stub.contains("public static var shared_count(get, set):Int"));
	CHECK(stub.contains("hxgodot.Native.setProperty(__hxgodotScript().__hxgodotId, \"shared_count\", value)"));
	CHECK(stub.contains("public static var MAX_HEALTH(get, never):Int"));
	CHECK_FALSE(stub.contains("static function set_MAX_HEALTH"));

	// A member the engine base already declares is not redeclared.
	HashSet<String> reserved_refresh;
	reserved_refresh.insert("refresh");
	const String trimmed = generate_gdscript_stub(entry, info, reserved_refresh);
	CHECK_FALSE(trimmed.contains("function refresh"));
}

TEST_CASE("[Modules][Haxe] completion and lookup see a GDScript global class") {
	const String dir = make_scratch_dir();
	const String gd_path = write_source(dir, "hud.gd",
			"class_name Hud\n"
			"extends Node2D\n"
			"signal died\n"
			"@export var health: int = 10\n"
			"var label: String = \"x\"\n"
			"static func create_hud(p: int) -> Hud:\n"
			"\treturn null\n"
			"func refresh(amount: float) -> void:\n"
			"\tpass\n");
	GDScriptGlobalClassScope scope("Hud", "Node2D", gd_path);

	HaxeGDScriptLibrary *classes = HaxeGDScriptLibrary::get_singleton();
	REQUIRE(classes->ensure_indexed());
	CHECK(classes->has_type("Hud"));
	HaxeClassInfo info;
	REQUIRE(classes->get_type_info("Hud", info));
	CHECK(info.find_function("refresh") != nullptr);

	const String main_path = write_source(dir, "Main.hx",
			"package;\n"
			"import godot.Node2D;\n"
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t}\n"
			"}\n");

	const String mark = String::chr(0xFFFF);

	// A type position offers the GDScript class.
	const HashSet<String> types = completion_names(
			"class Main {\n"
			"\tvar h:" +
			mark + "\n"
				   "}\n");
	CHECK(types.has("Hud"));

	// The class name offers its static members only.
	const HashSet<String> statics = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tHud." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(statics.has("create_hud"));
	CHECK_FALSE(statics.has("refresh"));

	// A value typed with the class offers the instance members, including the
	// engine ones inherited from the native base.
	const HashSet<String> instance = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tvar h:Hud;\n"
			"\t\th." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(instance.has("refresh"));
	CHECK(instance.has("health"));
	CHECK(instance.has("label"));
	CHECK(instance.has("died"));
	CHECK(instance.has("position"));
	CHECK_FALSE(instance.has("create_hud"));

	// `get_node("x", Hud)` refines the engine node to the GDScript class.
	const HashSet<String> typed = completion_names(
			"class Main {\n"
			"\tfunction f() {\n"
			"\t\tget_node(\"Hero\", Hud)." +
			mark + "\n"
				   "\t}\n"
				   "}\n");
	CHECK(typed.has("refresh"));
	CHECK(typed.has("health"));

	// Without the typed argument the result stays an engine node: engine
	// members are offered, but the GDScript ones need the typed form
	// (`get_node("Hero", Hud)`), because the static analysis cannot know which
	// script is attached at run time.
	const HashSet<String> untyped = completion_names(
			"class Main extends Node2D {\n"
			"\tfunction f() {\n"
			"\t\tget_node(\"Hero\")." +
					mark + "\n"
						   "\t}\n"
						   "}\n",
			main_path);
	CHECK(untyped.has("queue_free"));
	CHECK_FALSE(untyped.has("refresh"));

	// A definition lookup jumps into the `.gd` file.
	ScriptLanguage::LookupResult result;
	REQUIRE(haxe_lookup_code("Hud.", "refresh", String(), nullptr, result) == OK);
	CHECK(result.type == ScriptLanguage::LOOKUP_RESULT_CLASS_METHOD);
	CHECK(result.script_path == gd_path);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the utility host resolves a GDScript class base") {
	const String dir = make_scratch_dir();
	const String gd_path = write_source(dir, "hud.gd",
			"class_name Hud\n"
			"extends Node2D\n"
			"func refresh() -> void:\n"
			"\tpass\n");
	GDScriptGlobalClassScope scope("Hud", "Node2D", gd_path);

	// The glue walks from the script name to the binding of its native base.
	HxValue value = {};
	REQUIRE(HaxeRuntime::host_utility("class_parent", 0, "Hud", &value) == 0);
	REQUIRE(value.s != nullptr);
	CHECK(String::utf8(value.s) == "Node2D");

	// A plain engine class keeps the ClassDB answer.
	HxValue engine = {};
	REQUIRE(HaxeRuntime::host_utility("class_parent", 0, "Node2D", &engine) == 0);
	REQUIRE(engine.s != nullptr);
	CHECK(String::utf8(engine.s) == "CanvasItem");

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a package prefix in code completes like an import") {
	const String dir = make_scratch_dir();
	(void)write_source(dir.path_join("pkg"), "Top.hx",
			"package pkg;\nclass Top {\n}\n");
	(void)write_source(dir.path_join("pkg").path_join("alpha"), "Beta.hx",
			"package pkg.alpha;\nclass Beta {\n}\n");
	(void)write_source(dir.path_join("pkg").path_join("alpha").path_join("gamma"), "Delta.hx",
			"package pkg.alpha.gamma;\nclass Delta {\n}\n");
	SourceDirScope scope(dir);

	// `pkg.` inside a function body: the receiver names a package, not a type,
	// so the sub-package `alpha` and the type `Top` are offered, and the
	// dotted-away names (`pkg.Top`) are not repeated.
	const HashSet<String> root = completion_names(
			"class Other {\n\tfunction f() {\n\t\tpkg." + String::chr(0xFFFF) + "\n\t}\n}\n");
	CHECK(root.has("alpha"));
	CHECK(root.has("Top"));
	CHECK_FALSE(root.has("pkg.Top"));

	// A second package level offers the next sub-package and the types after it.
	const HashSet<String> nested = completion_names(
			"class Other {\n\tfunction f() {\n\t\tpkg.alpha." + String::chr(0xFFFF) + "\n\t}\n}\n");
	CHECK(nested.has("Beta"));
	CHECK(nested.has("gamma"));
	CHECK_FALSE(nested.has("pkg.alpha.Beta"));

	// A receiver that is neither a type nor a package stays empty rather than
	// offering unrelated project classes.
	const HashSet<String> unknown = completion_names(
			"class Other {\n\tfunction f() {\n\t\tnotAPackage." + String::chr(0xFFFF) + "\n\t}\n}\n");
	CHECK(unknown.is_empty());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a project class extends a library class") {
	const String dir = make_scratch_dir();
	// A project-local haxelib holding one Godot-facing library class: it
	// extends `Node2D` and exports a member, exactly as a project script would.
	const String haxelib_root = dir.path_join("haxelib");
	const String version_root = haxelib_root.path_join("mylib").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json",
			"{\n\t\"name\": \"mylib\",\n\t\"classPath\": \"src\"\n}\n");
	(void)write_source(version_root.path_join("src").path_join("mylib"), "BaseThing.hx",
			"package mylib;\n"
			"import godot.Node2D;\n"
			"@:globalClass\n"
			"class BaseThing extends Node2D {\n"
			"\t@:export var hp:Int = 3;\n"
			"\tpublic var helper:String;\n"
			"\toverride function _ready():Void {}\n"
			"\tpublic function libraryMethod():Void {}\n"
			"}\n");
	const String project_dir = dir.path_join("source");
	const String bar_path = write_source(project_dir, "Bar.hx",
			"package;\n"
			"import mylib.BaseThing;\n"
			"class Bar extends BaseThing {\n"
			"\t@:export var speed:Float = 1.0;\n"
			"\tpublic function ownMethod():Void {}\n"
			"}\n");
	HaxelibDirScope lib_scope(haxelib_root);
	SourceDirScope src_scope(project_dir);

	// The library class is parsed like a project class: its base and its
	// `@:globalClass` annotation are read from the source. The project-only
	// lookup deliberately does not answer for it, so completion can keep
	// preferring engine, standard library and project types.
	HaxeClassInfo base_info;
	REQUIRE(HaxeProject::resolve_class_info_by_name("mylib.BaseThing", base_info));
	CHECK(base_info.base == "Node2D");
	CHECK(base_info.is_global_class);
	HaxeClassInfo project_only;
	CHECK_FALSE(HaxeProject::get_class_info_by_name("mylib.BaseThing", project_only));

	// Because the library class extends a Godot class, a project class
	// extending it resolves the same native base it would for a project base.
	// Before this was resolved through the library index the lookup failed and
	// the script could not be attached at all.
	HaxeClassInfo bar_info;
	REQUIRE(HaxeProject::get_class_info(bar_path, bar_info));
	CHECK(HaxeProject::resolve_native_base(bar_info) == "Node2D");

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(bar_path) == OK);
	REQUIRE(script->is_valid());

	// A library base has no editable project resource, so it is not handed out
	// as the project script's base; the class chain still resolves through the
	// library index for members, as the checks below show.
	CHECK(script->get_base_script().is_null());

	// The library's `@:export` member is inherited, with its default value and
	// line, while a plain library variable is not a property.
	HashSet<String> names;
	List<PropertyInfo> properties;
	script->get_script_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		names.insert(property.name);
	}
	CHECK(names.has("speed"));
	CHECK(names.has("hp"));
	CHECK_FALSE(names.has("helper"));

	Variant value;
	CHECK(script->get_property_default_value("hp", value));
	CHECK((int)value == 3);
	CHECK(script->get_member_line("hp") > 0);

	// The library class becomes a script class because it reaches `Node2D`, so
	// the build marks its fields `@:keep` and an engine-called override such as
	// `_ready` survives `-dce full`. Only classes that reach an engine class are
	// listed, and by their own name (not their package).
	const Vector<String> library_scripts = HaxeProject::get_library_script_classes();
	CHECK(library_scripts.has("mylib.BaseThing"));
	CHECK(library_scripts.size() == 1);

	// The build marks that class `@:keep` (type and fields), so the engine can
	// instantiate it from a scene even when no project code says `new`, and its
	// `@:export` members and engine-called overrides survive `-dce full`. A
	// library `@:globalClass` is offered as an attachable script, so keeping
	// the type is what makes an exported game run it.
	Vector<Pair<String, String>> keep_mappings;
	const Vector<String> keep_arguments = HaxeBuild::collect_arguments(false, false, Vector<HaxeBuild::Library>(), Vector<HaxeClassInfo>(), keep_mappings);
	CHECK(keep_arguments.has("--macro"));
	CHECK(keep_arguments.has("addGlobalMetadata('mylib.BaseThing', '@:keep', true, true, true)"));

	// The engine asks the script instance whether it has a method, and it must
	// answer for a method inherited from the library base: `_ready` declared by
	// `BaseThing` is called on a `Bar` instance. `has_method()` stays own-only,
	// as `GDScript::has_method` does, while the inherited list carries both.
	CHECK_FALSE(script->has_method("_ready"));
	CHECK(script->has_inherited_method("_ready"));
	CHECK(script->has_inherited_method("libraryMethod"));
	CHECK(script->has_inherited_method("ownMethod"));
	CHECK_FALSE(script->has_inherited_method("missingMethod"));
	List<MethodInfo> methods;
	script->get_inherited_script_method_list(&methods);
	HashSet<String> method_names;
	for (const MethodInfo &method : methods) {
		method_names.insert(method.name);
	}
	CHECK(method_names.has("_ready"));
	CHECK(method_names.has("libraryMethod"));
	CHECK(method_names.has("ownMethod"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] library @:globalClass types are registered with the editor") {
	const String dir = make_scratch_dir();
	// A project-local haxelib with one Godot-facing `@:globalClass` class and
	// one plain class. Only the former can be a node script.
	const String haxelib_root = dir.path_join("haxelib");
	const String version_root = haxelib_root.path_join("visual").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json",
			"{\n\t\"name\": \"visual\",\n\t\"classPath\": \"src\"\n}\n");
	const String video_path = write_source(version_root.path_join("src").path_join("visual"), "Video.hx",
			"package visual;\n"
			"import godot.Sprite2D;\n"
			"@:globalClass\n"
			"class Video extends Sprite2D {\n"
			"\t@:exportFile([\"*.mp4\"])\n"
			"\t@:export var defaultVideo:String = \"\";\n"
			"}\n");
	(void)write_source(version_root.path_join("src").path_join("visual"), "Helper.hx",
			"package visual;\n"
			"class Helper {\n"
			"\tpublic static function ping():Void {}\n"
			"}\n");
	const String project_dir = dir.path_join("source");
	HaxelibDirScope lib_scope(haxelib_root);
	SourceDirScope src_scope(project_dir);

	// The library class reaches `Sprite2D`, so it is offered like a project
	// script. Its stored path is the localized one, the form the editor and a
	// saved scene use. The plain class reaches no engine class and stays out.
	const Vector<HaxeProject::LibraryGlobalClass> global_classes = HaxeProject::get_library_global_classes();
	REQUIRE(global_classes.size() == 1);
	CHECK(global_classes[0].name == "Video");
	CHECK(global_classes[0].native_base == "Sprite2D");
	const String registered_path = ProjectSettings::get_singleton()->localize_path(video_path);
	CHECK(global_classes[0].path == registered_path);

	// A project-registered Haxe class and a GDScript class under the haxelib
	// directory must survive the synchronization: only Haxe library classes
	// are managed here, and a project class is never dropped.
	const String project_thing_path = project_dir.path_join("ProjectThing.hx");
	ScriptServer::add_global_class("ProjectThing", "Node2D", "Haxe", project_thing_path, false, false);
	ScriptServer::add_global_class("GdThing", "Node2D", "GDScript", haxelib_root.path_join("GdThing.gd"), false, false);

	HaxeProject::sync_library_global_classes();
	CHECK(ScriptServer::is_global_class("Video"));
	CHECK(ScriptServer::get_global_class_language("Video") == "Haxe");
	CHECK(ScriptServer::get_global_class_native_base("Video") == "Sprite2D");
	CHECK(ScriptServer::get_global_class_path("Video") == registered_path);
	CHECK(ScriptServer::is_global_class("ProjectThing"));
	CHECK(ScriptServer::is_global_class("GdThing"));

	// Removing the library drops the registered class again; a stale entry
	// would keep pointing at a source that is no longer on disk.
	remove_scratch_dir(version_root);
	HaxeLibraryIndex::get_singleton()->invalidate();
	HaxeProject::invalidate();
	HaxeProject::sync_library_global_classes();
	CHECK_FALSE(ScriptServer::is_global_class("Video"));

	ScriptServer::remove_global_class("ProjectThing");
	ScriptServer::remove_global_class("GdThing");
	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] a library global class is packaged for an exported game") {
	const String dir = make_scratch_dir();
	// A project-local haxelib with one Godot-facing `@:globalClass` class, the
	// same shape as `hxvlc.godot.Video`: a scene saves it by its `res://` path.
	const String haxelib_root = dir.path_join("haxelib");
	const String version_root = haxelib_root.path_join("visual").path_join("1.0.0");
	(void)write_source(version_root, "haxelib.json",
			"{\n\t\"name\": \"visual\",\n\t\"classPath\": \"src\"\n}\n");
	const String video_path = write_source(version_root.path_join("src").path_join("visual"), "Video.hx",
			"package visual;\n"
			"import godot.Sprite2D;\n"
			"@:globalClass\n"
			"class Video extends Sprite2D {\n"
			"\t@:export var defaultVideo:String = \"\";\n"
			"\toverride function _ready():Void {}\n"
			"}\n");
	const String project_dir = dir.path_join("source");
	HaxelibDirScope lib_scope(haxelib_root);
	SourceDirScope src_scope(project_dir);

	// The exporter blanks every `.hx` it ships and writes this manifest in its
	// place. A library source is `.gdignore`d, so it never reaches the
	// per-file exporter; its description has to be in the manifest under the
	// very path a saved scene references.
	const String key = ProjectSettings::get_singleton()->localize_path(video_path);
	const String document = HaxeProject::serialize_script_metadata();
	{
		JSON json;
		REQUIRE(json.parse(document) == OK);
		Dictionary root = json.get_data();
		REQUIRE(root.has("scripts"));
		Dictionary scripts = root["scripts"];
		REQUIRE(scripts.has(key));
		Dictionary entry = scripts[key];
		CHECK(String(entry.get("name", String())) == "Video");
		CHECK(String(entry.get("base", String())) == "Sprite2D");
		CHECK(String(entry.get("fq_name", String())) == "visual.Video");
		CHECK((bool)entry.get("is_global_class", false));
		CHECK(String(entry.get("path", String())) == key);
	}

	// Blank the source the way the exporter does, then load the manifest: the
	// stub resolves to the library class description instead of failing, so an
	// exported game can attach and instantiate it.
	(void)write_source(video_path.get_base_dir(), video_path.get_file(), "\n");
	const String manifest = dir.path_join("scripts.json");
	{
		Ref<FileAccess> file = FileAccess::open(manifest, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string(document);
	}

	PackagedMetadataScope state;
	REQUIRE(HaxeProject::load_script_metadata(manifest));

	HaxeClassInfo info;
	REQUIRE(HaxeProject::get_class_info(key, info));
	CHECK(info.name == "Video");
	CHECK(info.fq_name == "visual.Video");
	CHECK(info.is_global_class);
	CHECK(HaxeProject::resolve_native_base(info) == "Sprite2D");

	Ref<HaxeScript> script;
	script.instantiate();
	REQUIRE(script->load_source_code(key) == OK);
	CHECK(script->is_valid());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] the Android NDK is found in every documented layout") {
	const String dir = make_scratch_dir();

	// A fake NDK: the two entries `has_android_ndk_layout()` requires.
	const auto make_ndk = [](const String &p_root, const String &p_name) {
		const String ndk = p_root.path_join(p_name);
		(void)write_source(ndk, "source.properties", "Pkg.Revision = 27.3.13750724\n");
		(void)write_source(ndk.path_join("toolchains/llvm/prebuilt"), ".keep", "");
		return ndk;
	};

	// 1. `ANDROID_NDK_ROOT` pointing straight at an install.
	const String direct = make_ndk(dir.path_join("standalone"), "android-ndk-r29");
	CHECK(HaxeBuild::find_android_ndk(direct) == direct);

	// 2. A directory holding several versioned installs: the newest wins.
	const String versioned_root = dir.path_join("versioned");
	const String older = make_ndk(versioned_root, "25.2.9519653");
	const String newer = make_ndk(versioned_root, "27.3.13750724");
	CHECK(HaxeBuild::find_android_ndk(versioned_root) == newer);
	CHECK(HaxeBuild::find_android_ndk(older) == older);

	// 3. An SDK whose NDK lives under `ndk/<version>/`.
	const String sdk = dir.path_join("android-sdk");
	const String sdk_ndk = make_ndk(sdk.path_join("ndk"), "27.3.13750724");
	CHECK(HaxeBuild::find_android_ndk(sdk) == sdk_ndk);

	// 4. The legacy `ndk-bundle/` layout is accepted too.
	const String legacy_sdk = dir.path_join("legacy-sdk");
	const String legacy_ndk = make_ndk(legacy_sdk, "ndk-bundle");
	CHECK(HaxeBuild::find_android_ndk(legacy_sdk) == legacy_ndk);

	// 5. The SDK manager's `ndk/` directory passed directly (one level without
	//    the SDK root) still resolves.
	CHECK(HaxeBuild::find_android_ndk(sdk.path_join("ndk")) == sdk_ndk);

	// 6. The layout that fails on a fresh machine: the SDK has no `ndk/` of its
	//    own, but a standalone NDK was downloaded next to it. The hint is the
	//    SDK, and the NDK is a sibling.
	const String sibling_parent = dir.path_join("sibling");
	const String bare_sdk = sibling_parent.path_join("android-sdk");
	(void)write_source(bare_sdk.path_join("platform-tools"), "adb", ""); // just a directory
	const String sibling_ndk = make_ndk(sibling_parent, "android-ndk-r29");
	CHECK(HaxeBuild::find_android_ndk(bare_sdk) == sibling_ndk);

	// 7. A hint that does not exist and has no NDK anywhere above it resolves
	//    to nothing instead of guessing.
	CHECK(HaxeBuild::find_android_ndk(dir.path_join("missing").path_join("deep").path_join("ndk")).is_empty());
	CHECK(HaxeBuild::find_android_ndk(String()).is_empty());

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] an edited library changes the build signature") {
	const String dir = make_scratch_dir();
	const String class_path = dir.path_join("source");

	// A directory that does not exist yet still has a stable signature.
	const String missing = HaxeBuild::library_edit_signature(class_path);
	CHECK_FALSE(missing.is_empty());

	(void)write_source(class_path, "Widget.hx", "class Widget {}\n");
	const String with_file = HaxeBuild::library_edit_signature(class_path);
	CHECK(with_file != missing);

	// A content change of a different length changes the signature even when
	// the platform's modification-time resolution would not.
	(void)write_source(class_path, "Widget.hx", "class Widget { var longer:String; }\n");
	const String edited = HaxeBuild::library_edit_signature(class_path);
	CHECK(edited != with_file);

	// A file in a sub-package counts too.
	(void)write_source(class_path.path_join("pkg"), "Extra.hx", "package pkg;\nclass Extra {}\n");
	const String nested = HaxeBuild::library_edit_signature(class_path);
	CHECK(nested != edited);

	remove_scratch_dir(dir);
}

/* Little-endian writers for the synthetic ELF object the dynamic dependency
 * tests build. */
static void elf_put_u16(PackedByteArray &p_data, int64_t p_offset, uint16_t p_value) {
	p_data.write[p_offset] = p_value & 0xff;
	p_data.write[p_offset + 1] = (p_value >> 8) & 0xff;
}

static void elf_put_u32(PackedByteArray &p_data, int64_t p_offset, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		p_data.write[p_offset + i] = (p_value >> (8 * i)) & 0xff;
	}
}

static void elf_put_u64(PackedByteArray &p_data, int64_t p_offset, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		p_data.write[p_offset + i] = (p_value >> (8 * i)) & 0xff;
	}
}

/* Builds a minimal little-endian 64-bit ELF shared object with the given
 * soname and `DT_NEEDED` names, so the reader can be tested without depending
 * on a toolchain or on a library that happens to be installed. */
static PackedByteArray build_test_elf(const String &p_soname, const Vector<String> &p_needed, uint16_t p_machine) {
	PackedByteArray strings;
	Vector<uint64_t> needed_offsets;
	auto add_string = [&strings](const String &p_value) -> uint64_t {
		const uint64_t offset = strings.size();
		for (int i = 0; i < p_value.length(); i++) {
			strings.push_back((uint8_t)p_value[i]);
		}
		strings.push_back(0);
		return offset;
	};
	const uint64_t soname_offset = add_string(p_soname);
	for (const String &needed : p_needed) {
		needed_offsets.push_back(add_string(needed));
	}

	const int64_t dynamic_offset = 0x100;
	const int64_t string_offset = 0x200;
	const uint64_t load_base = 0x1000;
	// DT_NEEDED entries plus DT_SONAME, DT_STRTAB, DT_STRSZ and DT_NULL.
	const int entries = p_needed.size() + 4;
	const int64_t dynamic_size = (int64_t)entries * 16;
	const int64_t total_size = string_offset + strings.size();

	PackedByteArray data;
	data.resize(total_size);
	data.fill(0);

	// ELF header.
	data.write[0] = 0x7f;
	data.write[1] = 'E';
	data.write[2] = 'L';
	data.write[3] = 'F';
	data.write[4] = 2; // ELFCLASS64
	data.write[5] = 1; // ELFDATA2LSB
	data.write[6] = 1; // EV_CURRENT
	elf_put_u16(data, 16, 3); // ET_DYN
	elf_put_u16(data, 18, p_machine);
	elf_put_u32(data, 20, 1);
	elf_put_u64(data, 32, 0x40); // e_phoff
	elf_put_u16(data, 52, 64); // e_ehsize
	elf_put_u16(data, 54, 56); // e_phentsize
	elf_put_u16(data, 56, 2); // e_phnum

	// PT_LOAD covering the whole file: the string table address is translated
	// back to a file offset through it.
	const int64_t load_header = 0x40;
	elf_put_u32(data, load_header + 0, 1); // PT_LOAD
	elf_put_u64(data, load_header + 8, 0); // p_offset
	elf_put_u64(data, load_header + 16, load_base); // p_vaddr
	elf_put_u64(data, load_header + 24, load_base); // p_paddr
	elf_put_u64(data, load_header + 32, total_size); // p_filesz
	elf_put_u64(data, load_header + 40, total_size); // p_memsz
	elf_put_u64(data, load_header + 48, 0x1000); // p_align

	// PT_DYNAMIC pointing at the dynamic array.
	const int64_t dynamic_header = load_header + 56;
	elf_put_u32(data, dynamic_header + 0, 2); // PT_DYNAMIC
	elf_put_u64(data, dynamic_header + 8, dynamic_offset);
	elf_put_u64(data, dynamic_header + 16, load_base + dynamic_offset);
	elf_put_u64(data, dynamic_header + 24, load_base + dynamic_offset);
	elf_put_u64(data, dynamic_header + 32, dynamic_size);
	elf_put_u64(data, dynamic_header + 40, dynamic_size);
	elf_put_u64(data, dynamic_header + 48, 8);

	// Dynamic entries.
	int64_t entry = dynamic_offset;
	auto put_entry = [&data, &entry](uint64_t p_tag, uint64_t p_value) {
		elf_put_u64(data, entry, p_tag);
		elf_put_u64(data, entry + 8, p_value);
		entry += 16;
	};
	for (const uint64_t needed_offset : needed_offsets) {
		put_entry(1, needed_offset); // DT_NEEDED
	}
	put_entry(14, soname_offset); // DT_SONAME
	put_entry(5, load_base + string_offset); // DT_STRTAB
	put_entry(10, strings.size()); // DT_STRSZ
	put_entry(0, 0); // DT_NULL

	for (int i = 0; i < strings.size(); i++) {
		data.write[string_offset + i] = strings[i];
	}
	return data;
}

static Error write_bytes(const String &p_path, const PackedByteArray &p_data) {
	DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}
	file->store_buffer(p_data);
	return OK;
}

TEST_CASE("[Modules][Haxe] the ELF reader reports soname and dynamic dependencies") {
	const String dir = make_scratch_dir();

	Vector<String> needed;
	needed.push_back("libvlc.so");
	needed.push_back("libc++_shared.so");
	const String path = dir.path_join("libsample.so");
	REQUIRE(write_bytes(path, build_test_elf("libsample.so", needed, HaxeElf::MACHINE_AARCH64)) == OK);

	HaxeElf::Object object;
	REQUIRE(HaxeElf::parse(path, object) == OK);
	CHECK(object.bits == 64);
	CHECK(object.machine == HaxeElf::MACHINE_AARCH64);
	CHECK(object.soname == "libsample.so");
	REQUIRE(object.needed.size() == 2);
	CHECK(object.needed[0] == "libvlc.so");
	CHECK(object.needed[1] == "libc++_shared.so");

	// The ABI names map to the machines the bundled libraries are built for.
	CHECK(HaxeElf::abi_to_machine("arm64-v8a") == HaxeElf::MACHINE_AARCH64);
	CHECK(HaxeElf::abi_to_machine("armeabi-v7a") == HaxeElf::MACHINE_ARM);
	CHECK(HaxeElf::abi_to_machine("x86") == HaxeElf::MACHINE_X86);
	CHECK(HaxeElf::abi_to_machine("x86_64") == HaxeElf::MACHINE_X86_64);
	CHECK(HaxeElf::abi_to_machine("wasm32") == HaxeElf::MACHINE_NONE);
	CHECK(HaxeElf::machine_to_abi(HaxeElf::MACHINE_X86_64) == "x86_64");

	// A file that is not an ELF object is rejected instead of misread, so an
	// unrelated `.so` name in the project cannot be packaged by accident.
	const String text = write_source(dir, "not_elf.so", "not an ELF object\n");
	CHECK(HaxeElf::parse(text, object) != OK);

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] bundled native libraries are resolved for one Android ABI") {
	const String dir = make_scratch_dir();
	const String haxelib = dir.path_join("haxelib");
	const String libdir = haxelib.path_join("hxvlc-godot").path_join("git").path_join("lib");

	// The compiled Haxe library needs libvlc.so and a system library. hxvlc
	// ships libvlc under per-ABI names that all declare the soname libvlc.so,
	// and libvlc itself needs libc++_shared.so, so the resolution has to be
	// transitive and pick the file built for the exported ABI.
	Vector<String> root_needed;
	root_needed.push_back("libvlc.so");
	root_needed.push_back("libm.so");
	const String root_path = dir.path_join("libmy_project.so");
	REQUIRE(write_bytes(root_path, build_test_elf("libmy_project.so", root_needed, HaxeElf::MACHINE_AARCH64)) == OK);

	Vector<String> vlc_needed;
	vlc_needed.push_back("libc++_shared.so");
	Vector<String> none;
	REQUIRE(write_bytes(libdir.path_join("libvlc-64.so"),
					build_test_elf("libvlc.so", vlc_needed, HaxeElf::MACHINE_AARCH64)) == OK);
	REQUIRE(write_bytes(libdir.path_join("libvlc-v7.so"),
					build_test_elf("libvlc.so", vlc_needed, HaxeElf::MACHINE_ARM)) == OK);
	REQUIRE(write_bytes(libdir.path_join("libc++_shared-64.so"),
					build_test_elf("libc++_shared.so", none, HaxeElf::MACHINE_AARCH64)) == OK);

	HaxelibDirScope lib_scope(haxelib);

	const Vector<HaxeProject::NativeLibrary> arm64 = HaxeProject::resolve_native_libraries(root_path, "arm64-v8a");
	REQUIRE(arm64.size() == 2);
	HashSet<String> names;
	for (const HaxeProject::NativeLibrary &library : arm64) {
		names.insert(library.name);
	}
	CHECK(names.has("libvlc.so"));
	CHECK(names.has("libc++_shared.so"));
	for (const HaxeProject::NativeLibrary &library : arm64) {
		CHECK(library.path.get_file().ends_with("-64.so"));
	}

	// The 32-bit library is chosen for the 32-bit ABI, and the missing system
	// library is reported instead of packaged.
	Vector<String> missing;
	const Vector<HaxeProject::NativeLibrary> armv7 = HaxeProject::resolve_native_libraries(root_path, "armeabi-v7a", &missing);
	REQUIRE(armv7.size() == 1);
	CHECK(armv7[0].name == "libvlc.so");
	CHECK(armv7[0].path.get_file() == "libvlc-v7.so");
	CHECK(missing.has("libm.so"));
	CHECK(missing.has("libc++_shared.so"));

	remove_scratch_dir(dir);
}

TEST_CASE("[Modules][Haxe] HaxeAudioStream is written by a producer thread and pulled by the mixer") {
	Ref<HaxeAudioStream> stream;
	stream.instantiate();
	CHECK(stream->get_stream_name() == "HaxeAudioStream");
	CHECK(stream->is_monophonic());
	CHECK(stream->buffered_frames() == 0);

	// Half a second of ring, and the few dozen milliseconds of backlog the
	// consumer waits for before it starts pulling.
	const double rate = 44100.0;
	stream->configure(rate, 0.5, 0.04);
	const int target = (int)Math::ceil(rate * 0.04);

	// Interleaved signed 16 bit, as LibVLC hands the samples over. The helper
	// fills a mono chunk with a constant sample and writes it.
	LocalVector<int16_t> samples;
	const int16_t raw = 16384; // 0.5 in signed 16 bit
	const auto write_mono = [&](int p_count) {
		samples.resize(p_count);
		for (int i = 0; i < p_count; i++) {
			samples[i] = raw;
		}
		return stream->write_frames(samples.ptr(), p_count, 1);
	};

	// A chunk smaller than the backlog is queued but not consumed: the consumer
	// holds silence until it has enough to play without underrunning.
	CHECK(write_mono(100) == 100);
	CHECK(stream->buffered_frames() == 100);

	LocalVector<AudioFrame> mixed;
	mixed.resize(512);

	// A playback is what the audio server actually mixes, and it is the only
	// thing that ever pulls from the stream.
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	CHECK(!playback->is_playing());
	playback->start(0.0);
	CHECK(playback->is_playing());

	CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	for (int i = 0; i < 512; i++) {
		CHECK(mixed[i].left == 0.0f);
		CHECK(mixed[i].right == 0.0f);
	}
	CHECK(stream->buffered_frames() == 100);

	// Past the backlog the sound starts, and a mono source is duplicated into
	// both channels. A pull always hands over as many frames as it was asked
	// for; a short count would make the audio server delete the playback like a
	// finished stream.
	CHECK(write_mono(target) == target);
	CHECK(stream->buffered_frames() == 100 + target);

	CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	bool heard = false;
	for (int i = 0; i < 512; i++) {
		CHECK(mixed[i].left == mixed[i].right);
		if (Math::abs(mixed[i].left) > 0.01f) {
			heard = true;
		}
	}
	CHECK(heard);
	CHECK(stream->buffered_frames() < 100 + target);
	CHECK(stream->get_underrun_count() == 0);

	// Draining the queue produces silence, which is counted as an underrun.
	while (stream->buffered_frames() > 0) {
		CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	}
	CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	CHECK(stream->get_underrun_count() > 0);

	// A flush (a seek, or the start of a loop) drops the queue and re-arms the
	// warm up, so the sound is not heard against a stale position.
	stream->reset_queue();
	CHECK(stream->buffered_frames() == 0);
	CHECK(write_mono(200) == 200);
	CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	CHECK(stream->buffered_frames() == 200);

	// A producer that runs far ahead of real time is trimmed back to the
	// backlog instead of leaving the sound behind the picture for good.
	CHECK(write_mono(30000) == 30000);
	CHECK(stream->buffered_frames() == 30200);
	CHECK(stream->pull_frames(mixed.ptr(), 512) == 512);
	CHECK(stream->get_trimmed_frames() > 0);
	CHECK(stream->buffered_frames() <= target);

	// A write that does not fit is dropped rather than blocking the producer's
	// thread, and what fits is taken.
	stream->reset_queue();
	const int taken = write_mono(200000);
	CHECK(taken > 0);
	CHECK((int64_t)taken == stream->buffered_frames());
	CHECK(stream->get_dropped_frames() == 200000 - taken);

	// The playback keeps its position and stops cleanly.
	playback->stop();
	CHECK(!playback->is_playing());
	CHECK(playback->get_playback_position() >= 0.0);
}

} // namespace TestHaxe
