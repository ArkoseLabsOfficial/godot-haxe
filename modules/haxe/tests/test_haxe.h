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

#include "../haxe_build.h"
#include "../haxe_completion.h"
#include "../haxe_language.h"
#include "../haxe_project.h"
#include "../haxe_script.h"
#include "../haxe_source.h"
#include "../haxe_stdlib.h"
#include "../haxe_support.h"

#ifdef TOOLS_ENABLED
#include "../editor/haxe_export_plugin.h"
#include "../editor/haxe_highlighter.h"

#include "core/io/zip_io.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/text_edit.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/object/script_language.h"
#include "core/templates/hash_set.h"
#include "core/variant/variant.h"
// `Node` is a base class of every scripted scene type the tests instantiate.
// Editor builds get it through `scene/gui/text_edit.h` below, template builds
// (which compile these tests with `tests=yes`) do not, so include it directly.
#include "scene/main/node.h"
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

	// An embedded script has no file to compile, so it is rejected with an
	// explanation instead of the removed missing-base error.
	List<ScriptLanguage::ScriptError> embedded_errors;
	CHECK_FALSE(language->validate("class Embedded {\n}\n", "res://scene.tscn::HaxeScript_1", &functions, &embedded_errors, nullptr, nullptr));
	REQUIRE_FALSE(embedded_errors.is_empty());
	CHECK(embedded_errors.front()->get().message.contains("cannot be embedded"));

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

} // namespace TestHaxe
