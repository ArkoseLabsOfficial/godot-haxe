/**************************************************************************/
/*  haxe_api_index.h                                                      */
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

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

/* Editor facing index of the generated Godot bindings.
 *
 * `dist/bindings/api.json` is written by `dist/tools/generate_bindings.py`
 * from the same `extension_api.json` the Haxe bindings are generated from, so
 * the editor can offer class and member completion, member lists and
 * documentation links for the exact engine version without parsing the
 * thousand generated Haxe files. It is loaded lazily, on the first editor
 * request, and only in editor builds.
 */
class HaxeApiIndex {
	static HaxeApiIndex *singleton;

	mutable Mutex mutex;
	bool loaded = false;
	bool failed = false;
	String error;
	String version;
	Dictionary classes;
	Dictionary global_enums;
	HashMap<String, String> bases;
	Vector<String> class_names;
	HashMap<String, Vector<String>> member_cache;
	HashMap<String, Dictionary> inherited_cache;

	bool load();

public:
	static HaxeApiIndex *get_singleton();

	/* Loads the index on demand. Returns false and stores the reason in
	 * get_error() when the engine build has no binding metadata. */
	bool ensure_loaded();
	String get_error() const;
	String get_version() const;

	bool has_class(const String &p_name);
	String get_base(const String &p_name);
	Vector<String> get_class_names();
	Dictionary get_class(const String &p_name);

	/* Class and member names, derived classes first, including inherited
	 * members. Cached per class. */
	Vector<String> get_member_names(const String &p_name);

	/* Members declared by the class itself, without ancestors. Each entry is
	 * the raw api.json record. */
	Array get_declared_members(const String &p_name);
	Dictionary get_class_enums(const String &p_name);
	Vector<String> get_global_enum_names();
	Vector<String> get_global_enum_values(const String &p_name);

	/* Looks a member up along the inheritance chain. */
	bool find_member(const String &p_class, const String &p_member, Dictionary &r_member, String &r_owner);

	void invalidate();
};
