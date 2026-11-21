/**************************************************************************/
/*  haxe_stdlib.cpp                                                       */
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

#include "haxe_stdlib.h"

#include "haxe_project.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

HaxeStdLibrary *HaxeStdLibrary::singleton = nullptr;

/* Directories under the standard library root that hold another target's
 * replacement of the base classes. The project builds for cpp, so those are
 * ignored here; `cpp/_std` is indexed explicitly, on top of the base. */
static const char *TARGET_DIRS[] = {
	"cpp", "cs", "flash", "hl", "java", "js", "jvm", "lua", "neko", "php",
	"python", "eval", "_std",
	nullptr
};

/* `StdTypes.hx` declares the core types and is never named after one of them,
 * so its types are recorded under their real names. */
static const char *CORE_TYPES[] = {
	"Void", "Float", "Int", "Single", "Null", "Bool", "Dynamic",
	"Iterator", "Iterable", "KeyValueIterator", "KeyValueIterable", "ArrayAccess",
	nullptr
};

static bool is_skipped_directory(const String &p_name, bool p_top_level) {
	if (p_name.is_empty() || p_name.begins_with(".")) {
		return true;
	}
	if (!p_top_level) {
		return p_name == "_std";
	}
	for (int i = 0; TARGET_DIRS[i] != nullptr; i++) {
		if (p_name == TARGET_DIRS[i]) {
			return true;
		}
	}
	return false;
}

/* A PATH lookup for the compiler, so a standard library installed next to a
 * non-standard `haxe` can still be found. */
static String find_haxe_on_path() {
	String path = OS::get_singleton()->get_environment("PATH");
	for (const String &dir : path.split(":")) {
		if (dir.strip_edges().is_empty()) {
			continue;
		}
		String candidate = dir.path_join("haxe");
		if (FileAccess::exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

HaxeStdLibrary *HaxeStdLibrary::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(HaxeStdLibrary);
	}
	return singleton;
}

static bool looks_like_std_dir(const String &p_dir) {
	return !p_dir.is_empty() && FileAccess::exists(p_dir.path_join("Std.hx")) && FileAccess::exists(p_dir.path_join("Array.hx"));
}

void HaxeStdLibrary::index() {
	indexed = true;

	Vector<String> candidates;
	String from_env = OS::get_singleton()->get_environment("HAXE_STD_PATH");
	if (!from_env.strip_edges().is_empty()) {
		candidates.push_back(from_env.strip_edges());
	}
	String executable = HaxeProject::get_haxe_executable().strip_edges();
	if (!executable.contains("/")) {
		String found = find_haxe_on_path();
		if (!found.is_empty()) {
			executable = found;
		}
	}
	if (executable.contains("/")) {
		String base = executable.get_base_dir().simplify_path();
		static const char *RELATIVE[] = {
			"std", "../std", "../share/haxe/std", "../lib/haxe/std",
			"../../share/haxe/std", "../../lib/haxe/std", "../../../share/haxe/std",
			nullptr
		};
		for (int i = 0; RELATIVE[i] != nullptr; i++) {
			candidates.push_back(base.path_join(RELATIVE[i]).simplify_path());
		}
	}
	candidates.push_back("/usr/share/haxe/std");
	candidates.push_back("/usr/local/share/haxe/std");
	candidates.push_back("/usr/lib/haxe/std");

	for (const String &candidate : candidates) {
		if (looks_like_std_dir(candidate)) {
			std_dir = candidate;
			break;
		}
	}
	if (std_dir.is_empty()) {
		error = "The Haxe standard library was not found. Set HAXE_STD_PATH to the compiler's `std` directory.";
		return;
	}

	/* The base classpath, plus the cpp target's replacements. `cpp/_std` is
	 * added after the base so its entries win. */
	auto add_tree = [&](const String &p_root) {
		Vector<String> pending;
		pending.push_back(String());
		while (!pending.is_empty()) {
			String rel = pending[pending.size() - 1];
			pending.remove_at(pending.size() - 1);
			String dir = rel.is_empty() ? p_root : p_root.path_join(rel);
			for (const String &sub : DirAccess::get_directories_at(dir)) {
				if (is_skipped_directory(sub, rel.is_empty())) {
					continue;
				}
				pending.push_back(rel.is_empty() ? sub : rel.path_join(sub));
			}
			for (const String &file : DirAccess::get_files_at(dir)) {
				if (!file.ends_with(".hx")) {
					continue;
				}
				String rel_file = rel.is_empty() ? file : rel.path_join(file);
				String fq = rel_file.substr(0, rel_file.length() - 3).replace("/", ".");
				if (fq == "StdTypes") {
					// Named for the file, not for a type: handled below.
					continue;
				}
				add_type(fq, dir.path_join(file), !rel_file.contains("/"));
			}
		}
	};

	add_tree(std_dir);
	String cpp_std = std_dir.path_join("cpp").path_join("_std");
	if (DirAccess::dir_exists_absolute(cpp_std)) {
		add_tree(cpp_std);
	}

	// The core types live together in `StdTypes.hx`.
	String core_path = std_dir.path_join("StdTypes.hx");
	if (FileAccess::exists(core_path)) {
		for (int i = 0; CORE_TYPES[i] != nullptr; i++) {
			add_type(CORE_TYPES[i], core_path, true);
		}
	}
}

void HaxeStdLibrary::add_type(const String &p_fq_name, const String &p_path, bool p_top_level) {
	if (p_fq_name.is_empty()) {
		return;
	}
	bool is_new = !file_of_type.has(p_fq_name);
	file_of_type[p_fq_name] = p_path;
	if (p_top_level) {
		top_level_types.insert(p_fq_name);
	}
	if (is_new) {
		type_names.push_back(p_fq_name);
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

bool HaxeStdLibrary::ensure_indexed() {
	MutexLock lock(mutex);
	if (!indexed) {
		index();
	}
	return !std_dir.is_empty();
}

String HaxeStdLibrary::get_error() const {
	return error;
}

String HaxeStdLibrary::get_std_dir() const {
	return std_dir;
}

bool HaxeStdLibrary::has_type(const String &p_name) const {
	if (p_name.is_empty()) {
		return false;
	}
	if (p_name.contains(".")) {
		return file_of_type.has(p_name);
	}
	return top_level_types.has(p_name);
}

const Vector<String> &HaxeStdLibrary::get_type_names() const {
	return type_names;
}

bool HaxeStdLibrary::parse_file(const String &p_fq_name, HaxeClassInfo &r_info) {
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

bool HaxeStdLibrary::get_type_info(const String &p_name, HaxeClassInfo &r_info) {
	if (!ensure_indexed()) {
		return false;
	}
	if (!has_type(p_name)) {
		return false;
	}
	MutexLock lock(mutex);
	if (parsed.has(p_name)) {
		r_info = parsed[p_name];
		return true;
	}
	HaxeClassInfo info;
	if (!parse_file(p_name, info)) {
		return false;
	}
	parsed[p_name] = info;
	r_info = info;
	return true;
}

Vector<String> HaxeStdLibrary::get_package_types(const String &p_package) {
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

Vector<String> HaxeStdLibrary::get_subpackages(const String &p_package) {
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

void HaxeStdLibrary::invalidate() {
	MutexLock lock(mutex);
	indexed = false;
	std_dir = String();
	error = String();
	file_of_type.clear();
	top_level_types.clear();
	type_names.clear();
	packages.clear();
	parsed.clear();
}
