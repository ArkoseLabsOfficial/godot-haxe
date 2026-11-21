/**************************************************************************/
/*  haxe_gdscript.h                                                       */
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

#include "haxe_source.h"

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

/* Parses a GDScript source file into the same description a Haxe script gets
 * (`HaxeClassInfo`), so completion, member listing and definition lookup can
 * treat both languages the same way.
 *
 * Only class level declarations (indentation zero) are read: `class_name`,
 * `extends`, `static func`, `func`, `var`, `static var`, `const` and `signal`.
 * Nested classes and statements inside function bodies are ignored. The
 * parser never executes project code.
 *
 * `r_info.base` is the declared base as written (a native class, another
 * GDScript global class, or a `res://` path), so the completion can follow the
 * inheritance chain through `classify_type`.
 */
Error parse_gdscript_source(const String &p_source, const String &p_path, HaxeClassInfo &r_info);

/* One GDScript global class (`class_name X`) of the project. */
struct HaxeGDScriptEntry {
	String name;
	String path;
	/* The base as declared (`extends`), which completion follows. */
	String declared_base;
	/* The engine class the script ultimately extends. */
	String native_base;
	/* The Haxe class the generated stub extends: the stub of the declared
	 * base when it is another GDScript class, otherwise `godot.<native_base>`.
	 */
	String stub_base;
};

/* Index of the project's GDScript global classes.
 *
 * GDScript and Haxe share one object model, so a Haxe script has to be able to
 * call the other way round: `MyClass.staticFunction()` and
 * `get_node("x", MyClass).instanceFunction()`. The index reads the classes the
 * running editor knows about (`ScriptServer`), parses them on demand, and
 * writes a small Haxe stub class for each so the compiler resolves the calls.
 * Nothing is hard coded: adding, renaming or removing a `class_name` in a
 * GDScript file changes the index and the stubs.
 *
 * The stub bodies dispatch at runtime through the same object id the generated
 * engine bindings use, so a GDScript method call is an ordinary engine call.
 */
class HaxeGDScriptLibrary {
	static HaxeGDScriptLibrary *singleton;

	mutable Mutex mutex;
	String signature;
	HashMap<String, HaxeGDScriptEntry> entries;
	Vector<String> names;

	void index();
	/* Fills `r_entry` for `p_name`, resolving the stub base against the other
	 * indexed classes. */
	bool resolve_entry(const String &p_name, HaxeGDScriptEntry &r_entry) const;
	/* Member names declared by `p_name` and its GDScript ancestors, so a stub
	 * does not redeclare an inherited field. */
	void collect_declared_names(const String &p_name, HashSet<String> &r_names) const;

public:
	static HaxeGDScriptLibrary *get_singleton();

	/* Rebuilds the index when the editor's global class list changed. Returns
	 * false when the project has no GDScript global class. */
	bool ensure_indexed();

	bool has_type(const String &p_name);
	bool get_type_info(const String &p_name, HaxeClassInfo &r_info);
	bool get_entry(const String &p_name, HaxeGDScriptEntry &r_entry);
	const Vector<String> &get_type_names();

	/* Writes the stub of every indexed class into `p_dir`, replacing stale
	 * files from a previous index. Returns OK, or an error and its reason. */
	Error generate_stubs(const String &p_dir, String &r_error);

	void invalidate();
};

/* The Haxe source of one GDScript class stub. `p_reserved` holds the member
 * names the stub must not redeclare (engine members of its base, plus the
 * ancestors' members). Split from the class so tests can check the generated
 * text without touching the script server. */
String generate_gdscript_stub(const HaxeGDScriptEntry &p_entry, const HaxeClassInfo &p_info, const HashSet<String> &p_reserved);
