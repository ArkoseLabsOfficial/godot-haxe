/**************************************************************************/
/*  haxe_library_index.h                                                  */
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

/* Index of the Haxe libraries installed for the project (the project-local
 * `haxelib/` repository and, only when the project explicitly opts in, the
 * machine wide one).
 *
 * Autocompletion has to know the classes a project actually builds against, so
 * that a class from `hxbit`, `heaps`, ... is offered, its members complete and
 * a definition lookup jumps into the library. Like `HaxeStdLibrary` this walks
 * each resolved class path once to record the type names, and parses a file
 * only when a member list or a definition is asked for. Nothing is hard coded:
 * installing or removing a library and refreshing the Haxelib manager is
 * enough for completion to follow, and the machine wide repository is never
 * consulted unless `haxe/allows_global_haxelib` says so.
 */
class HaxeLibraryIndex {
	static HaxeLibraryIndex *singleton;

	mutable Mutex mutex;
	bool indexed = false;
	String error;
	/* Full type name (`hxbit.Serializable`) to its source file. */
	HashMap<String, String> file_of_type;
	/* Top level library types (no package) are reachable by their short name
	 * as well. */
	HashSet<String> top_level_types;
	/* Short name to the full name(s) that declare it. A bare name resolves
	 * only when exactly one library declares it, so two libraries sharing a
	 * class name cannot make an unqualified type ambiguous. */
	HashMap<String, Vector<String>> short_name_types;
	Vector<String> type_names;
	HashSet<String> packages;
	/* Parsed on demand and kept, since member lists are requested repeatedly. */
	HashMap<String, HaxeClassInfo> parsed;

	void index();
	void add_root(const String &p_root);
	void add_type(const String &p_fq_name, const String &p_path);
	/* Maps a written type name (a full name, or a bare name that resolves to
	 * exactly one library type) to the full name. */
	bool resolve_type_name(const String &p_name, String &r_fq_name) const;
	bool parse_file(const String &p_fq_name, HaxeClassInfo &r_info);

public:
	static HaxeLibraryIndex *get_singleton();

	/* Locates the installed libraries and records the type names. Returns
	 * false when the project has no library at all (not an error). */
	bool ensure_indexed();
	String get_error() const;

	/* True when p_name (a full name, or a unique bare name) is a library
	 * type. Builds the index on first use. */
	bool has_type(const String &p_name);
	/* Every recorded type by full name; top level types by their short name. */
	const Vector<String> &get_type_names() const;

	/* Parses and returns the type named by `p_name`. */
	bool get_type_info(const String &p_name, HaxeClassInfo &r_info);

	/* For `import` completion: types declared directly in p_package (short
	 * names), and its immediate subpackages. */
	Vector<String> get_package_types(const String &p_package);
	Vector<String> get_subpackages(const String &p_package);

	void invalidate();
};
