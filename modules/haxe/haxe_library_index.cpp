/**************************************************************************/
/*  haxe_library_index.cpp                                                */
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

#include "haxe_library_index.h"

#include "haxe_build.h"
#include "haxe_project.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"

HaxeLibraryIndex *HaxeLibraryIndex::singleton = nullptr;

HaxeLibraryIndex *HaxeLibraryIndex::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(HaxeLibraryIndex);
	}
	return singleton;
}

void HaxeLibraryIndex::index() {
	indexed = true;

	// An exported game has no usable project directory to scan: the `haxelib/`
	// copies are shipped as empty `.hx` stubs and `globalize_path("res://")` is
	// not a real path inside a pack, so the on-disk scan below could only walk
	// `/haxelib` and log failures. Every library class a game can resolve -- a
	// `@:globalClass` offered as a script, or the base of a project class -- is
	// already described by the packaged script metadata
	// (`res://haxe/scripts.json`), which is the game's index. Leaving this one
	// empty keeps the runtime lookups off the filesystem entirely.
	if (HaxeProject::uses_packaged_metadata()) {
		return;
	}

	Vector<HaxeBuild::Library> libraries;
	// What is on disk is the source of truth, exactly as for the Haxelib
	// manager: a library that is installed but not (yet) written to haxe.json
	// still has to complete.
	HaxeBuild::list_installed_libraries(libraries);

	// The machine wide repository is only ever consulted when the project
	// explicitly opts in. `resolve_dependencies` is the one place that knows
	// how to ask haxelib for the global path, so its global entries are merged
	// in; local libraries are already covered above.
	if (HaxeProject::allows_global_haxelib()) {
		Vector<HaxeBuild::Library> resolved;
		String resolution_error;
		if (HaxeBuild::resolve_dependencies(resolved, resolution_error) == OK) {
			for (const HaxeBuild::Library &library : resolved) {
				if (library.global) {
					libraries.push_back(library);
				}
			}
		}
	}

	HashSet<String> seen_roots;
	for (const HaxeBuild::Library &library : libraries) {
		const String root = library.class_path;
		if (root.is_empty() || seen_roots.has(root) || !DirAccess::dir_exists_absolute(root)) {
			continue;
		}
		seen_roots.insert(root);
		add_root(root);
	}
}

void HaxeLibraryIndex::add_root(const String &p_root) {
	Vector<String> pending;
	pending.push_back(String());
	while (!pending.is_empty()) {
		String rel = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		String dir = rel.is_empty() ? p_root : p_root.path_join(rel);
		for (const String &sub : DirAccess::get_directories_at(dir)) {
			if (sub.is_empty() || sub.begins_with(".")) {
				continue;
			}
			pending.push_back(rel.is_empty() ? sub : rel.path_join(sub));
		}
		for (const String &file : DirAccess::get_files_at(dir)) {
			if (!file.ends_with(".hx") || file == "import.hx" || file.begins_with(".")) {
				continue;
			}
			String rel_file = rel.is_empty() ? file : rel.path_join(file);
			String fq = rel_file.substr(0, rel_file.length() - 3).replace("/", ".");
			if (fq.is_empty()) {
				continue;
			}
			add_type(fq, dir.path_join(file));
		}
	}
}

void HaxeLibraryIndex::add_type(const String &p_fq_name, const String &p_path) {
	if (p_fq_name.is_empty() || file_of_type.has(p_fq_name)) {
		return;
	}
	file_of_type[p_fq_name] = p_path;
	type_names.push_back(p_fq_name);
	bool top_level = !p_fq_name.contains(".");
	if (top_level) {
		top_level_types.insert(p_fq_name);
	}
	String short_name = top_level ? p_fq_name : p_fq_name.get_slice(".", p_fq_name.get_slice_count(".") - 1);
	Vector<String> &declarations = short_name_types[short_name];
	if (!declarations.has(p_fq_name)) {
		declarations.push_back(p_fq_name);
	}
	// Record every ancestor package so `import` completion can walk them.
	String package = p_fq_name;
	int separator = package.rfind(".");
	while (separator > 0) {
		package = package.substr(0, separator);
		packages.insert(package);
		separator = package.rfind(".");
	}
}

bool HaxeLibraryIndex::ensure_indexed() {
	MutexLock lock(mutex);
	if (!indexed) {
		index();
		type_names.sort();
	}
	return !file_of_type.is_empty();
}

String HaxeLibraryIndex::get_error() const {
	return error;
}

bool HaxeLibraryIndex::resolve_type_name(const String &p_name, String &r_fq_name) const {
	if (p_name.is_empty()) {
		return false;
	}
	if (p_name.contains(".")) {
		if (file_of_type.has(p_name)) {
			r_fq_name = p_name;
			return true;
		}
		return false;
	}
	if (top_level_types.has(p_name)) {
		r_fq_name = p_name;
		return true;
	}
	// A bare name resolves when every library agrees on one full name; an
	// ambiguous name is left to the compiler instead of guessing.
	if (short_name_types.has(p_name) && short_name_types[p_name].size() == 1) {
		r_fq_name = short_name_types[p_name][0];
		return true;
	}
	return false;
}

bool HaxeLibraryIndex::has_type(const String &p_name) {
	if (!ensure_indexed()) {
		return false;
	}
	String fq;
	return resolve_type_name(p_name, fq);
}

const Vector<String> &HaxeLibraryIndex::get_type_names() const {
	return type_names;
}

bool HaxeLibraryIndex::parse_file(const String &p_fq_name, HaxeClassInfo &r_info) {
	if (!file_of_type.has(p_fq_name)) {
		return false;
	}
	String path = file_of_type[p_fq_name];
	Error read_error = OK;
	String source = FileAccess::get_file_as_string(path, &read_error);
	if (read_error != OK || source.is_empty()) {
		return false;
	}
	HaxeClassInfo file_info;
	if (parse_haxe_source(source, path, file_info) != OK || !file_info.valid) {
		return false;
	}
	return haxe_module_find_type(file_info, p_fq_name, r_info);
}

bool HaxeLibraryIndex::get_type_info(const String &p_name, HaxeClassInfo &r_info) {
	if (!ensure_indexed()) {
		return false;
	}
	String fq;
	if (!resolve_type_name(p_name, fq)) {
		return false;
	}
	MutexLock lock(mutex);
	if (parsed.has(fq)) {
		r_info = parsed[fq];
		return true;
	}
	HaxeClassInfo info;
	if (!parse_file(fq, info)) {
		return false;
	}
	parsed[fq] = info;
	r_info = info;
	return true;
}

Vector<String> HaxeLibraryIndex::get_package_types(const String &p_package) {
	Vector<String> types;
	if (!ensure_indexed()) {
		return types;
	}
	for (const String &name : type_names) {
		int separator = name.rfind(".");
		String package = separator > 0 ? name.substr(0, separator) : String();
		if (package != p_package) {
			continue;
		}
		types.push_back(separator > 0 ? name.substr(separator + 1) : name);
	}
	types.sort();
	return types;
}

Vector<String> HaxeLibraryIndex::get_subpackages(const String &p_package) {
	Vector<String> result;
	if (!ensure_indexed()) {
		return result;
	}
	for (const String &package : packages) {
		if (p_package.is_empty()) {
			int separator = package.find(".");
			String root = separator > 0 ? package.substr(0, separator) : package;
			if (!result.has(root)) {
				result.push_back(root);
			}
			continue;
		}
		if (!package.begins_with(p_package + ".")) {
			continue;
		}
		String remainder = package.substr(p_package.length() + 1);
		String child = remainder.contains(".") ? remainder.get_slice(".", 0) : remainder;
		if (!child.is_empty() && !result.has(child)) {
			result.push_back(child);
		}
	}
	result.sort();
	return result;
}

void HaxeLibraryIndex::invalidate() {
	MutexLock lock(mutex);
	indexed = false;
	error = String();
	file_of_type.clear();
	top_level_types.clear();
	short_name_types.clear();
	type_names.clear();
	packages.clear();
	parsed.clear();
}
