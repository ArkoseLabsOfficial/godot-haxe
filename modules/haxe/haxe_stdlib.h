/**************************************************************************/
/*  haxe_stdlib.h                                                         */
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

/* Index of the installed Haxe standard library.
 *
 * Completion has to know Haxe's own types (`Array`, `Map`, `String`,
 * `StringTools`, `haxe.io.Bytes`, ...) as well as the engine bindings. This
 * index locates the standard library that belongs to the Haxe compiler the
 * project builds with, walks its `.hx` files once to record the type names,
 * and parses a file only when a member list or a definition is actually asked
 * for. Nothing is hard coded: a different Haxe installation is followed as
 * long as its standard library keeps the usual layout.
 */
class HaxeStdLibrary {
	static HaxeStdLibrary *singleton;

	mutable Mutex mutex;
	bool indexed = false;
	String std_dir;
	String error;
	/* Full type name (`haxe.ds.StringMap`) to its source file. */
	HashMap<String, String> file_of_type;
	/* Top level types (`Array`, `String`, ...) are also reachable by their
	 * short name; package types are not, so they do not shadow engine or
	 * project classes. */
	HashSet<String> top_level_types;
	Vector<String> type_names;
	HashSet<String> packages;
	/* Parsed on demand and kept, since member lists are requested repeatedly. */
	HashMap<String, HaxeClassInfo> parsed;

	void index();
	void add_type(const String &p_fq_name, const String &p_path, bool p_top_level);
	/* Type names declared by a single file, from its primary type and any
	 * secondary types. */
	bool parse_file(const String &p_fq_name, HaxeClassInfo &r_info);

public:
	static HaxeStdLibrary *get_singleton();

	/* Locates the standard library and records the type names. Returns false
	 * and stores the reason in get_error() when none can be found. */
	bool ensure_indexed();
	String get_error() const;
	String get_std_dir() const;

	/* True when p_name (short for a top level type, qualified otherwise) is a
	 * standard library type. */
	bool has_type(const String &p_name) const;
	/* Every recorded type, top level names first. */
	const Vector<String> &get_type_names() const;

	/* Parses and returns the type named by `p_name`. */
	bool get_type_info(const String &p_name, HaxeClassInfo &r_info);

	/* For `import` completion: types declared directly in p_package (short
	 * names), and its immediate subpackages. */
	Vector<String> get_package_types(const String &p_package);
	Vector<String> get_subpackages(const String &p_package);

	void invalidate();
};
