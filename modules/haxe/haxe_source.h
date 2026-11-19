/**************************************************************************/
/*  haxe_source.h                                                         */
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

#include "core/object/property_info.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

struct HaxeArgumentInfo {
	String name;
	String type;
};

struct HaxeFunctionInfo {
	String name;
	// Text of the doc comment above the declaration, if any.
	String doc;
	Vector<HaxeArgumentInfo> arguments;
	String return_type;
	int line = 0;
	bool is_static = false;
	bool is_signal = false;
	MethodInfo to_method_info() const;
};

struct HaxeMemberInfo {
	String name;
	// Text of the doc comment above the declaration, if any. It is what the
	// Inspector and the built-in help show as the property description.
	String doc;
	String type;
	String default_source;
	Variant default_value;
	int line = 0;
	bool exported = false;
	bool is_static = false;
	String group;
	String subgroup;
	String hint_string;
	PropertyHint hint = PROPERTY_HINT_NONE;
	PropertyInfo to_property_info() const;
};

/* A type declared after the primary class in the same file (`class Second {}`).
 *
 * Haxe allows several types per file. They are imported as
 * `<package>.<FileName>.<Type>` and are visible from the file that declares
 * them, so completion and lookup have to know their members too. They are not
 * scripts: nothing here becomes an Inspector property.
 */
struct HaxeSecondaryTypeInfo {
	String name;
	String base;
	int line = 0;
	bool is_abstract = false;
	/* Variables marked `@:export`. Secondary types are never scripts, so this
	 * is only kept so the shared parsing code has somewhere to put them. */
	Vector<HaxeMemberInfo> members;
	Vector<HaxeMemberInfo> variables;
	Vector<HaxeFunctionInfo> functions;
};

/* Metadata extracted from a .hx file. This is the editor facing description
 * of a Haxe script: it is derived from source (like GDScript's parser output)
 * so the inspector and class discovery work without running user code.
 */
struct HaxeClassInfo {
	bool valid = false;
	String error;
	String path;
	String package;
	String name;
	String base;
	String fq_name;
	/* Doc comment above the type declaration, used as its brief
	 * description where the editor shows one (script help, Inspector). */
	String doc;
	int class_line = 0;
	bool is_tool = false;
	bool is_abstract = false;
	/* Only classes annotated with `@:globalClass` are registered with the
	 * editor as global classes and offered in the "Create Node" dialog. */
	bool is_global_class = false;
	/* Exported variables (`@:export`, `@:exported` or an export hint such as
	 * `@:exportRange`), the ones that become Godot properties. */
	Vector<HaxeMemberInfo> members;
	/* Every declared variable, exported or not. Completion lists these, while
	 * the inspector and script instance only expose `members`. */
	Vector<HaxeMemberInfo> variables;
	Vector<HaxeFunctionInfo> functions;
	/* Types declared in the same file after the primary class (for example
	 * `class Second {}`). They are importable as
	 * `import <package>.<FileName>.<Type>`. */
	Vector<String> secondary_classes;
	/* Members of those types, so that completion and lookup work on them. */
	Vector<HaxeSecondaryTypeInfo> secondary_types;
	MethodInfo to_signal_method_info(const HaxeFunctionInfo &p_function) const;
	const HaxeFunctionInfo *find_function(const String &p_name) const;
	const HaxeMemberInfo *find_member(const String &p_name) const;
	const HaxeMemberInfo *find_variable(const String &p_name) const;
};

/* Parses a Haxe source file. Supports the metadata understood by the Godot
 * binding layer:
 *   @:tool, @:export, @:exportGroup("..."), @:exportSubgroup("..."),
 *   @:exportRange(min, max[, step]) / @:exportEnum("A,B,C"),
 *   @:signal on function declarations, @:globalClass on class declarations.
 * Parsing never executes project code.
 */
Error parse_haxe_source(const String &p_source, const String &p_path, HaxeClassInfo &r_info);

/* Maps a Haxe type to the engine Variant type used for property metadata. */
Variant::Type haxe_type_to_variant(const String &p_type, String &r_class_name);
String variant_type_to_haxe(Variant::Type p_type);

/* Best-effort static type of a variable initializer, used when a declaration
 * omits its `:Type`. Recognizes literals (`120.0` → `Float`), the math value
 * constructors (`Vector2(1, 2)`), `new SomeClass(...)` and array/map literals.
 * Returns an empty string when the expression carries no type information, so
 * the caller can leave the declaration untyped instead of guessing wrong. */
String haxe_infer_initializer_type(const String &p_source);

/* Converts a Haxe literal expression to a Variant. Returns an empty Variant
 * and sets r_ok to false when the expression is not a compile time literal.
 */
Variant haxe_eval_literal(const String &p_source, bool &r_ok);
