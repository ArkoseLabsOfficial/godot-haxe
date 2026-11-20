/**************************************************************************/
/*  haxe_project.cpp                                                      */
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

#include "haxe_project.h"

#include "haxe_runtime.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "core/variant/variant_utility.h"

const char *HaxeProject::SETTING_SOURCE_DIR = "haxe/source_directory";
const char *HaxeProject::SETTING_HAXELIB_DIR = "haxe/haxelib_directory";
const char *HaxeProject::SETTING_BUILD_DIR = "haxe/build_directory";
const char *HaxeProject::SETTING_HAXE_EXECUTABLE = "haxe/toolchain/haxe_executable";
const char *HaxeProject::SETTING_HAXELIB_EXECUTABLE = "haxe/toolchain/haxelib_executable";
const char *HaxeProject::SETTING_DEFINES = "haxe/compiler/defines";
const char *HaxeProject::SETTING_EXTRA_ARGS = "haxe/compiler/extra_arguments";
const char *HaxeProject::SETTING_ALLOW_GLOBAL_HAXELIB = "haxe/haxelib/allow_global";
const char *HaxeProject::SETTING_BUILD_ON_RUN = "haxe/build/build_on_run";
const char *HaxeProject::SETTING_DEBUG_SYMBOLS = "haxe/build/debug_symbols";

Mutex HaxeProject::cache_mutex;
HashMap<String, HaxeProject::CacheEntry> HaxeProject::cache;
HashMap<String, String> HaxeProject::name_to_path;
HashMap<String, HaxeProject::SecondaryEntry> HaxeProject::secondary_types;
bool HaxeProject::index_valid = false;
uint64_t HaxeProject::index_build_time = 0;
Vector<String> HaxeProject::indexed_files;
uint64_t HaxeProject::index_check_time = 0;
static uint64_t index_rebuild_count = 0;
bool HaxeProject::packaged_metadata_loaded = false;
/* Set by the engine tests through `set_packaged_metadata_for_testing()`. It
 * overrides the default in both directions, which is why it is not guarded by
 * `TOOLS_ENABLED`: a `tests=yes` template build is also not an exported game,
 * and the tests that exercise the exported-game path need to turn it back on
 * there too. Nothing outside the tests calls the setter. */
static bool packaged_metadata_forced = false;
/* Tri-state cache for the runtime detection in `uses_packaged_metadata()`:
 * -1 not decided yet, 0 a project, 1 an exported game. It is reset together
 * with `packaged_metadata_loaded` so the engine tests can flip the answer. */
static int packaged_metadata_detected = -1;

static String absolute_project_path(const String &p_setting, const String &p_default) {
	String path = GLOBAL_GET(p_setting);
	if (path.is_empty()) {
		path = p_default;
	}
	if (path.is_absolute_path()) {
		return path.simplify_path();
	}
	return HaxeProject::get_project_root().path_join(path).simplify_path();
}

/* The project name reduced to characters a file name and a C identifier can
 * hold, shared by the build output and the packaged library so both agree on
 * `<Name>` and only the extension differs per platform. */
static String sanitized_project_name() {
	String project_name = GLOBAL_GET("application/config/name");
	if (project_name.is_empty()) {
		project_name = HaxeProject::get_project_root().get_file();
	}
	String sanitized;
	for (int i = 0; i < project_name.length(); i++) {
		char32_t c = project_name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
			sanitized += c;
		} else if (c == ' ' || c == '-' || c == '.') {
			sanitized += '_';
		}
	}
	if (sanitized.is_empty()) {
		sanitized = "Game";
	}
	if (sanitized[0] >= '0' && sanitized[0] <= '9') {
		sanitized = "G" + sanitized;
	}
	return sanitized;
}

Error HaxeProject::remove_directory(const String &p_path) {
	Vector<String> directories;
	Vector<String> files;
	{
		Ref<DirAccess> dir = DirAccess::open(p_path);
		if (dir.is_null()) {
			return ERR_CANT_OPEN;
		}
		dir->set_include_hidden(true);
		dir->list_dir_begin();
		for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
			// `get_next()` also yields the navigational entries, and recursing
			// into ".." would walk up the tree forever.
			if (entry == "." || entry == "..") {
				continue;
			}
			// A symbolic link is deleted like a file even when it points at a
			// directory: class-path aliases link to the project's source, and
			// following them here would erase the user's files instead of the
			// cache entry.
			if (dir->current_is_dir() && !dir->is_link(entry)) {
				directories.push_back(entry);
			} else {
				files.push_back(entry);
			}
		}
		dir->list_dir_end();
	}
	for (const String &file : files) {
		DirAccess::remove_absolute(p_path.path_join(file));
	}
	for (const String &sub_directory : directories) {
		remove_directory(p_path.path_join(sub_directory));
	}
	return DirAccess::remove_absolute(p_path);
}

void HaxeProject::prune_directory(const String &p_root, int p_keep, const String &p_always_keep) {
	// Newest first: the newest build is the one the editor loads, and the one
	// right before it is the fallback for instances that are still running.
	struct EntryAge {
		String name;
		bool is_directory = false;
		uint64_t modified = 0;

		// `Vector::sort()` uses this, so the newest entry ends up first.
		bool operator<(const EntryAge &p_other) const { return modified > p_other.modified; }
	};
	Vector<EntryAge> ages;
	{
		Ref<DirAccess> dir = DirAccess::open(p_root);
		if (dir.is_null()) {
			return;
		}
		dir->set_include_hidden(true);
		dir->list_dir_begin();
		for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
			// Skip the navigational entries: they are not cache entries, and
			// they would shift which builds count as the newest.
			if (entry == "." || entry == "..") {
				continue;
			}
			EntryAge age;
			age.name = entry;
			age.is_directory = dir->current_is_dir();
			age.modified = FileAccess::get_modified_time(p_root.path_join(entry));
			ages.push_back(age);
		}
		dir->list_dir_end();
	}
	ages.sort();
	int kept = 0;
	for (const EntryAge &age : ages) {
		String path = p_root.path_join(age.name);
		if (kept++ < p_keep || (!p_always_keep.is_empty() && path == p_always_keep)) {
			continue;
		}
		if (age.is_directory) {
			remove_directory(path);
		} else {
			DirAccess::remove_absolute(path);
		}
	}
}

void HaxeProject::register_project_settings() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings->has_setting(SETTING_SOURCE_DIR)) {
		settings->set(SETTING_SOURCE_DIR, "source");
	}
	if (!settings->has_setting(SETTING_HAXELIB_DIR)) {
		settings->set(SETTING_HAXELIB_DIR, "haxelib");
	}
	if (!settings->has_setting(SETTING_BUILD_DIR)) {
		settings->set(SETTING_BUILD_DIR, ".godot/haxe");
	}
	if (!settings->has_setting(SETTING_HAXE_EXECUTABLE)) {
		settings->set(SETTING_HAXE_EXECUTABLE, "haxe");
	}
	if (!settings->has_setting(SETTING_HAXELIB_EXECUTABLE)) {
		settings->set(SETTING_HAXELIB_EXECUTABLE, "haxelib");
	}
	if (!settings->has_setting(SETTING_DEFINES)) {
		settings->set(SETTING_DEFINES, PackedStringArray());
	}
	if (!settings->has_setting(SETTING_EXTRA_ARGS)) {
		settings->set(SETTING_EXTRA_ARGS, PackedStringArray());
	}
	if (!settings->has_setting(SETTING_ALLOW_GLOBAL_HAXELIB)) {
		settings->set(SETTING_ALLOW_GLOBAL_HAXELIB, false);
	}
	if (!settings->has_setting(SETTING_BUILD_ON_RUN)) {
		settings->set(SETTING_BUILD_ON_RUN, true);
	}
	if (!settings->has_setting(SETTING_DEBUG_SYMBOLS)) {
		settings->set(SETTING_DEBUG_SYMBOLS, true);
	}

	// Ensure the settings are visible with their defaults, matching how the
	// C# module exposes its build configuration.
	for (const char *name : { SETTING_SOURCE_DIR, SETTING_HAXELIB_DIR, SETTING_BUILD_DIR,
				 SETTING_HAXE_EXECUTABLE, SETTING_HAXELIB_EXECUTABLE, SETTING_DEFINES, SETTING_EXTRA_ARGS,
				 SETTING_ALLOW_GLOBAL_HAXELIB, SETTING_BUILD_ON_RUN, SETTING_DEBUG_SYMBOLS }) {
		settings->set_initial_value(name, settings->get(name));
		// The module reads these on every build and does not tolerate their
		// absence, so the Project Settings dialog must not delete them.
		settings->set_setting_protected(name);
	}
}

String HaxeProject::get_project_root() {
	return ProjectSettings::get_singleton()->globalize_path("res://").simplify_path();
}

String HaxeProject::get_source_dir() {
	return absolute_project_path(SETTING_SOURCE_DIR, "source");
}

String HaxeProject::get_package_for_path(const String &p_path) {
	String source_dir = get_source_dir();
	if (source_dir.is_empty() || p_path.is_empty()) {
		return String();
	}
	String path = ProjectSettings::get_singleton()->globalize_path(p_path).simplify_path();
	String root = source_dir.simplify_path();
	while (root.length() > 1 && root.ends_with("/")) {
		root = root.substr(0, root.length() - 1);
	}
	if (!path.begins_with(root + "/")) {
		return String();
	}
	String directory = path.substr(root.length() + 1).get_base_dir();
	if (directory.is_empty() || directory == ".") {
		return String();
	}
	String package;
	for (const String &part : directory.split("/")) {
		if (part.is_empty() || part == "." || part == "..") {
			continue;
		}
		if (!package.is_empty()) {
			package += ".";
		}
		package += part.validate_unicode_identifier();
	}
	return package.trim_suffix(".");
}

String HaxeProject::get_haxelib_dir() {
	return absolute_project_path(SETTING_HAXELIB_DIR, "haxelib");
}

String HaxeProject::get_build_dir() {
	return absolute_project_path(SETTING_BUILD_DIR, ".godot/haxe");
}

const char *const HaxeProject::WEB_TARGET = "wasm32";
const char *const HaxeProject::WEB_THREADS_TARGET = "wasm32-threads";

bool HaxeProject::is_web_target(const String &p_abi) {
	return p_abi == WEB_TARGET || p_abi == WEB_THREADS_TARGET;
}

bool HaxeProject::is_web_threads_target(const String &p_abi) {
	return p_abi == WEB_THREADS_TARGET;
}

String HaxeProject::get_config_dir(bool p_release, bool p_editor, const String &p_abi) {
	String dir = get_build_dir();
	dir = dir.path_join(p_editor ? "editor" : "game");
	dir = dir.path_join(p_release ? "release" : "debug");
	// Android object files and libraries are architecture specific, so every
	// ABI gets its own directory; a shared one would hand one ABI's objects to
	// another ABI's linker. The web target keeps its own directory for the same
	// reason: wasm objects are not host objects.
	if (!p_abi.is_empty()) {
		dir = dir.path_join(is_web_target(p_abi) ? "web" : "android").path_join(p_abi);
	}
	return dir;
}

String HaxeProject::get_generated_dir(bool p_release, bool p_editor, const String &p_abi) {
	return get_config_dir(p_release, p_editor, p_abi).path_join("generated");
}

String HaxeProject::get_web_hxcpp_config_path(bool p_release, bool p_editor, const String &p_abi) {
	return get_config_dir(p_release, p_editor, p_abi).path_join("hxcpp_config.xml");
}

/* The extension hxcpp gives the shared library it links. Each platform
 * toolchain sets its own (`toolchain/linux-toolchain.xml` uses `.dso`, the
 * MSVC one `.dll`, macOS `.dylib`), and Android a plain `.so`. */
static String host_library_extension() {
#ifdef WINDOWS_ENABLED
	return ".dll";
#elif defined(MACOS_ENABLED)
	return ".dylib";
#else
	return ".dso";
#endif
}

String HaxeProject::get_library_base_name() {
	return sanitized_project_name();
}

String HaxeProject::get_library_file_name(const String &p_abi) {
	if (is_web_target(p_abi)) {
		// Emscripten's `dll` linker names its output `<base>.so`, and the
		// browser fetches the module by the name the game opens with dlopen,
		// so no `lib` prefix is involved (that prefix is an Android APK rule).
		return sanitized_project_name() + ".so";
	}
	if (!p_abi.is_empty()) {
		return "lib" + sanitized_project_name() + ".so";
	}
	return sanitized_project_name() + host_library_extension();
}

String HaxeProject::get_android_library_soname() {
	return "lib" + sanitized_project_name() + ".so";
}

String HaxeProject::get_hxcpp_output_file_name(const String &p_abi) {
	if (is_web_target(p_abi) || p_abi.is_empty()) {
		return get_library_file_name(p_abi);
	}
	// `toolchain/finish-setup.xml` sets `LIBEXTRA` per architecture and
	// `toolchain/haxe-target.xml` appends it to the Android library name, so
	// an arm64 build creates `lib<Name>-64.so`, not `lib<Name>.so`.
	String suffix;
	if (p_abi == "arm64-v8a") {
		suffix = "-64";
	} else if (p_abi == "armeabi-v7a") {
		suffix = "-v7";
	} else if (p_abi == "x86") {
		suffix = "-x86";
	} else if (p_abi == "x86_64") {
		suffix = "-x86_64";
	} else {
		// An unknown ABI is rejected before a build starts, so its final name
		// is only used to report a missing file.
		return get_library_file_name(p_abi);
	}
	return "lib" + sanitized_project_name() + suffix + ".so";
}

String HaxeProject::get_versioned_library_path(bool p_release, bool p_editor, const String &p_stamp, const String &p_abi) {
	const String short_stamp = p_stamp.substr(0, 12);
	const String builds = get_config_dir(p_release, p_editor, p_abi).path_join("builds");
	if (!p_abi.is_empty()) {
		// An Android library is packaged under the name the linker resolves,
		// so the version moves into the directory instead of the file name.
		return builds.path_join(short_stamp).path_join(get_library_file_name(p_abi));
	}
	const String base = sanitized_project_name() + "-" + short_stamp;
	return builds.path_join(base + host_library_extension());
}

String HaxeProject::get_output_library_path(bool p_release, bool p_editor, const String &p_abi) {
	return get_config_dir(p_release, p_editor, p_abi).path_join("cpp").path_join(get_hxcpp_output_file_name(p_abi));
}

String HaxeProject::get_packaged_library_path() {
	return String("res://haxe/") + get_library_file_name();
}

String HaxeProject::extract_packaged_library() {
#ifdef ANDROID_ENABLED
	// Godot packages native libraries into the APK and the OS extracts them
	// into the application's library directory at install time. The linker
	// searches that directory for a bare soname, so no extraction is needed.
	return get_android_library_soname();
#elif defined(WEB_ENABLED)
	// A browser has no writable native code directory: Emscripten's dlopen
	// resolves a plain file name against the page the game is served from, so
	// the web exporter ships the library next to index.html (as a shared
	// object, which also registers it in the page's dynamic library list).
	// Loading it is therefore a `dlopen("<Name>.so")` with no extraction step.
	return get_library_file_name(WEB_TARGET);
#else
	const String source = get_packaged_library_path();
	if (source.is_empty() || !FileAccess::exists(source)) {
		return String();
	}

	String data = OS::get_singleton()->get_data_path();
	if (data.is_empty()) {
		return String();
	}
	String dir = data.path_join(OS::get_singleton()->get_godot_dir_name()).path_join("haxe_library");
	const String extension = get_library_file_name().get_extension();
	String target = dir.path_join(sanitized_project_name() + "-" + FileAccess::get_md5(source).substr(0, 12) + "." + extension);
	if (FileAccess::exists(target)) {
		// The name is derived from the contents, so an existing file is the
		// same library. Reusing it also never overwrites a library a running
		// process has already loaded.
		return target;
	}

	Ref<FileAccess> file = FileAccess::open(source, FileAccess::READ);
	if (file.is_null()) {
		return String();
	}
	Vector<uint8_t> contents = file->get_buffer(file->get_length());
	if (contents.is_empty()) {
		return String();
	}

	Error error = DirAccess::make_dir_recursive_absolute(dir);
	if (error != OK) {
		ERR_PRINT(vformat("Cannot create the Haxe library directory '%s' (error %d).", dir, (int)error));
		return String();
	}
	Ref<FileAccess> output = FileAccess::open(target, FileAccess::WRITE);
	if (output.is_null()) {
		ERR_PRINT(vformat("Cannot extract the exported Haxe library to '%s'.", target));
		return String();
	}
	output->store_buffer(contents.ptr(), contents.size());
	prune_directory(dir, 3, target);
	return target;
#endif
}

void HaxeProject::prune_caches(const String &p_keep_support_dir) {
	String build_dir = get_build_dir();
	if (build_dir.is_empty() || !build_dir.is_absolute_path()) {
		return;
	}

	// Versioned libraries: a build is copied to a new file every time so that
	// running instances keep working, which means the directory grows with
	// every compile. Keeping the newest few is enough for the editor to load
	// the current build and for instances started right before a rebuild.
	for (const bool editor : { false, true }) {
		for (const bool release : { false, true }) {
			Vector<String> abis;
			abis.push_back(String());
			// Android builds are per ABI; their versioned directories grow the
			// same way and are pruned by the same rule. The web target keeps its
			// own directory too.
			for (const char *abi : { "armeabi-v7a", "arm64-v8a", "x86", "x86_64" }) {
				abis.push_back(abi);
			}
			abis.push_back(WEB_TARGET);
			abis.push_back(WEB_THREADS_TARGET);
			for (const String &abi : abis) {
				String builds = get_config_dir(release, editor, abi).path_join("builds");
				if (!DirAccess::dir_exists_absolute(builds)) {
					continue;
				}
				prune_directory(builds, 3);
			}
		}
	}

	// Support files embedded in the engine are extracted per manifest hash; a
	// new engine build leaves the previous extraction behind.
	String support_root = build_dir.path_join("support");
	if (DirAccess::dir_exists_absolute(support_root)) {
		prune_directory(support_root, 2, p_keep_support_dir);
	}

#ifdef TOOLS_ENABLED
	// Earlier versions of this module kept their working files in the user data
	// directory. Only the editor deletes them, and only when the project does
	// not keep its own state there: a project path containing a space moves the
	// launcher and the class-path aliases into these directories, and deleting
	// them after every build would break the next one.
	if (Engine::get_singleton()->is_editor_hint()) {
		String data = OS::get_singleton()->get_data_path();
		const bool project_state = project_state_is_space_free();
		if (!data.is_empty()) {
			String godot_dir = data.path_join(OS::get_singleton()->get_godot_dir_name());
			String support = godot_dir.path_join("haxe_support");
			if (DirAccess::dir_exists_absolute(support) && !p_keep_support_dir.begins_with(support)) {
				remove_directory(support);
			}
			if (project_state) {
				remove_directory(godot_dir.path_join("haxe_work"));
				remove_directory(godot_dir.path_join("haxe_class_path"));
			}
		}
	}
#endif
}

String HaxeProject::get_haxe_executable() {
	return GLOBAL_GET(SETTING_HAXE_EXECUTABLE);
}

String HaxeProject::get_haxelib_executable() {
	return GLOBAL_GET(SETTING_HAXELIB_EXECUTABLE);
}

Vector<String> HaxeProject::get_defines() {
	Vector<String> result;
	PackedStringArray array = GLOBAL_GET(SETTING_DEFINES);
	for (const String &define : array) {
		if (!define.strip_edges().is_empty()) {
			result.push_back(define.strip_edges());
		}
	}
	return result;
}

Vector<String> HaxeProject::get_extra_args() {
	Vector<String> result;
	PackedStringArray array = GLOBAL_GET(SETTING_EXTRA_ARGS);
	for (const String &argument : array) {
		if (!argument.strip_edges().is_empty()) {
			result.push_back(argument.strip_edges());
		}
	}
	return result;
}

bool HaxeProject::allows_global_haxelib() {
	return GLOBAL_GET(SETTING_ALLOW_GLOBAL_HAXELIB);
}

bool HaxeProject::build_on_run() {
	return GLOBAL_GET(SETTING_BUILD_ON_RUN);
}

bool HaxeProject::debug_symbols_enabled(bool p_release) {
	if (p_release) {
		return false;
	}
	return GLOBAL_GET(SETTING_DEBUG_SYMBOLS);
}

uint64_t HaxeProject::file_modified_time(const String &p_path) {
	return (uint64_t)FileAccess::get_modified_time(p_path);
}

void HaxeProject::rebuild_index() {
	index_rebuild_count++;
	HashMap<String, CacheEntry> new_cache;
	HashMap<String, String> new_names;
	HashMap<String, SecondaryEntry> new_secondary;

	// A rebuild may be triggered by a file being added or removed. Files whose
	// previous parse is still valid are reused instead of being read and parsed
	// again, so a rebuild is proportional to the change, not to the project.
	HashMap<String, CacheEntry> previous;
	{
		MutexLock lock(cache_mutex);
		previous = cache;
	}

	String source_dir = get_source_dir();
	Vector<String> files;
	if (DirAccess::dir_exists_absolute(source_dir)) {
		Vector<String> pending;
		pending.push_back(source_dir);
		while (!pending.is_empty()) {
			String directory = pending[pending.size() - 1];
			pending.remove_at(pending.size() - 1);
			Ref<DirAccess> dir = DirAccess::open(directory);
			if (dir.is_null()) {
				continue;
			}
			dir->list_dir_begin();
			String name = dir->get_next();
			while (!name.is_empty()) {
				if (dir->current_is_dir()) {
					if (!name.begins_with(".") && name != "haxelib" && name != "bin") {
						pending.push_back(directory.path_join(name));
					}
				} else if (name.get_extension().to_lower() == "hx") {
					files.push_back(directory.path_join(name));
				}
				name = dir->get_next();
			}
			dir->list_dir_end();
		}

		for (const String &file : files) {
			CacheEntry entry;
			entry.modified = file_modified_time(file);
			HashMap<String, CacheEntry>::Iterator previous_entry = previous.find(file);
			if (previous_entry != previous.end() && !previous_entry->value.stale && previous_entry->value.modified == entry.modified) {
				entry = previous_entry->value;
			} else {
				Ref<FileAccess> access = FileAccess::open(file, FileAccess::READ);
				if (access.is_null()) {
					continue;
				}
				String source = access->get_as_utf8_string();
				if (parse_haxe_source(source, file, entry.info) != OK || !entry.info.valid) {
					continue;
				}
			}
			if (!entry.info.fq_name.is_empty()) {
				new_names[entry.info.fq_name] = file;
			}
			if (!new_names.has(entry.info.name)) {
				new_names[entry.info.name] = file;
			}
			for (const HaxeSecondaryTypeInfo &type : entry.info.secondary_types) {
				SecondaryEntry secondary;
				secondary.path = file;
				secondary.type_name = type.name;
				if (!entry.info.fq_name.is_empty()) {
					new_secondary[entry.info.fq_name + "." + type.name] = secondary;
				}
				if (!new_secondary.has(type.name)) {
					new_secondary[type.name] = secondary;
				}
			}
			new_cache[file] = entry;
		}
	}

	MutexLock lock(cache_mutex);
	cache = new_cache;
	name_to_path = new_names;
	secondary_types = new_secondary;
	indexed_files = files;
	indexed_files.sort();
	index_valid = true;
	index_build_time = Time::get_singleton()->get_ticks_msec();
}

void HaxeProject::ensure_index() {
	// A game has no usable sources (they are empty stubs) and its file set
	// cannot change at run time, so the packaged manifest is the index.
	if (uses_packaged_metadata()) {
		bool needs_load = false;
		{
			MutexLock lock(cache_mutex);
			needs_load = !index_valid && !packaged_metadata_loaded;
		}
		if (needs_load) {
			packaged_metadata_loaded = true;
			load_packaged_script_metadata();
		}
		return;
	}

	uint64_t now = Time::get_singleton()->get_ticks_msec();
	bool needs_rebuild = false;
	{
		MutexLock lock(cache_mutex);
		if (!index_valid) {
			needs_rebuild = true;
		} else if (now - index_check_time >= 200) {
			// Adding or deleting a source file does not invalidate the cache
			// through the script editor callbacks, so the file set is
			// re-checked periodically. The throttle keeps a directory walk out
			// of the per-file hot path.
			index_check_time = now;
			Vector<String> files = scan_sources();
			if (files.size() != indexed_files.size()) {
				needs_rebuild = true;
			} else {
				for (int i = 0; i < files.size(); i++) {
					if (files[i] != indexed_files[i]) {
						needs_rebuild = true;
						break;
					}
				}
			}
		}
	}
	if (needs_rebuild) {
		rebuild_index();
	}
}

bool HaxeProject::get_class_info(const String &p_path, HaxeClassInfo &r_info) {
	ensure_index();
	if (uses_packaged_metadata()) {
		// The shipped `.hx` is an empty stub: the class description comes from
		// the manifest loaded above, never from parsing the file.
		MutexLock lock(cache_mutex);
		HashMap<String, CacheEntry>::Iterator it = cache.find(p_path);
		if (it == cache.end()) {
			r_info = HaxeClassInfo();
			r_info.path = p_path;
			r_info.error = "This exported game has no metadata for the Haxe script.";
			return false;
		}
		r_info = it->value.info;
		return r_info.valid;
	}
	uint64_t modified = file_modified_time(p_path);

	{
		MutexLock lock(cache_mutex);
		HashMap<String, CacheEntry>::Iterator it = cache.find(p_path);
		if (it != cache.end() && !it->value.stale && it->value.modified == modified) {
			r_info = it->value.info;
			return r_info.valid;
		}
	}

	Ref<FileAccess> access = FileAccess::open(p_path, FileAccess::READ);
	if (access.is_null()) {
		r_info = HaxeClassInfo();
		r_info.error = "Cannot open file.";
		return false;
	}
	String source = access->get_as_utf8_string();
	CacheEntry entry;
	entry.modified = modified;
	Error error = parse_haxe_source(source, p_path, entry.info);

	MutexLock lock(cache_mutex);
	// A stale entry can still know the names it used to expose; dropping them
	// keeps a rename from leaving the old name mapped to this file.
	HashMap<String, CacheEntry>::Iterator stale_entry = cache.find(p_path);
	if (stale_entry != cache.end()) {
		drop_names_for_path(stale_entry->value.info, p_path);
	}
	cache[p_path] = entry;
	if (!entry.info.fq_name.is_empty()) {
		name_to_path[entry.info.fq_name] = p_path;
	}
	if (!name_to_path.has(entry.info.name)) {
		name_to_path[entry.info.name] = p_path;
	}
	index_secondary_types(entry.info);
	r_info = entry.info;
	return error == OK && r_info.valid;
}

void HaxeProject::drop_names_for_path(const HaxeClassInfo &p_info, const String &p_path) {
	// Secondary types are dropped by index_secondary_types(); this removes the
	// primary and fully qualified names of a file that was parsed again.
	List<String> stale;
	if (!p_info.fq_name.is_empty()) {
		stale.push_back(p_info.fq_name);
	}
	if (!p_info.name.is_empty()) {
		stale.push_back(p_info.name);
	}
	for (const String &name : stale) {
		HashMap<String, String>::Iterator it = name_to_path.find(name);
		if (it != name_to_path.end() && it->value == p_path) {
			name_to_path.remove(it);
		}
	}
}

void HaxeProject::index_secondary_types(const HaxeClassInfo &p_info) {
	// The file was parsed again: drop the entries that used to point at it, so
	// renaming or deleting a secondary type cannot leave a stale class behind.
	Vector<String> stale;
	for (const KeyValue<String, SecondaryEntry> &entry : secondary_types) {
		if (entry.value.path == p_info.path) {
			stale.push_back(entry.key);
		}
	}
	for (const String &key : stale) {
		secondary_types.erase(key);
	}

	for (const HaxeSecondaryTypeInfo &type : p_info.secondary_types) {
		SecondaryEntry entry;
		entry.path = p_info.path;
		entry.type_name = type.name;
		if (!p_info.fq_name.is_empty()) {
			secondary_types[p_info.fq_name + "." + type.name] = entry;
		}
		if (!secondary_types.has(type.name)) {
			secondary_types[type.name] = entry;
		}
	}
}

bool HaxeProject::get_class_info_by_name(const String &p_name, HaxeClassInfo &r_info, String *r_path) {
	ensure_index();
	String path;
	String type_name;
	{
		MutexLock lock(cache_mutex);
		HashMap<String, String>::Iterator it = name_to_path.find(p_name);
		if (it != name_to_path.end()) {
			path = it->value;
		} else {
			// A type declared in the same file as another class (`Maina` in
			// `Main.hx`) is reachable by its short name or through its import
			// path (`game.Main.Maina`).
			HashMap<String, SecondaryEntry>::Iterator secondary = secondary_types.find(p_name);
			if (secondary == secondary_types.end()) {
				return false;
			}
			path = secondary->value.path;
			type_name = secondary->value.type_name;
		}
	}
	if (r_path) {
		*r_path = path;
	}
	if (type_name.is_empty()) {
		return get_class_info(path, r_info);
	}

	HaxeClassInfo file_info;
	if (!get_class_info(path, file_info) || !file_info.valid) {
		return false;
	}
	for (const HaxeSecondaryTypeInfo &type : file_info.secondary_types) {
		if (type.name != type_name) {
			continue;
		}
		r_info = HaxeClassInfo();
		r_info.valid = true;
		r_info.path = file_info.path;
		r_info.package = file_info.package;
		r_info.name = type.name;
		r_info.base = type.base;
		r_info.class_line = type.line;
		r_info.is_abstract = type.is_abstract;
		// The import path is what the compiler expects for a secondary type.
		r_info.fq_name = file_info.fq_name + "." + type.name;
		r_info.members = type.members;
		r_info.variables = type.variables;
		r_info.functions = type.functions;
		return true;
	}
	return false;
}

Vector<HaxeClassInfo> HaxeProject::get_all_classes() {
	ensure_index();
	Vector<HaxeClassInfo> classes;
	MutexLock lock(cache_mutex);
	for (const KeyValue<String, CacheEntry> &entry : cache) {
		if (entry.value.info.valid) {
			classes.push_back(entry.value.info);
		}
	}
	return classes;
}

void HaxeProject::invalidate(const String &p_path) {
	MutexLock lock(cache_mutex);
	if (p_path.is_empty()) {
		index_valid = false;
		cache.clear();
		name_to_path.clear();
		return;
	}
	// Only this file's content changed. Marking the whole index invalid would
	// make the next lookup reparse every source in the project, which turned a
	// reload into an O(project) operation. The file set is unchanged, so the
	// index stays valid and this one entry is re-read on demand.
	HashMap<String, CacheEntry>::Iterator it = cache.find(p_path);
	if (it != cache.end()) {
		it->value.stale = true;
	}
}

uint64_t HaxeProject::get_index_rebuild_count() {
	MutexLock lock(cache_mutex);
	return index_rebuild_count;
}

void HaxeProject::cache_class_info(const HaxeClassInfo &p_info) {
	if (p_info.path.is_empty()) {
		return;
	}
	MutexLock lock(cache_mutex);
	HashMap<String, CacheEntry>::Iterator existing = cache.find(p_info.path);
	CacheEntry entry;
	if (existing != cache.end()) {
		drop_names_for_path(existing->value.info, p_info.path);
	}
	entry.info = p_info;
	// The editor parses unsaved text. Recording the on-disk timestamp keeps
	// the entry valid until the file is actually written, at which point the
	// stamp changes and it is parsed again from disk.
	entry.modified = file_modified_time(p_info.path);
	cache[p_info.path] = entry;
	if (!p_info.fq_name.is_empty()) {
		name_to_path[p_info.fq_name] = p_info.path;
	}
	if (!name_to_path.has(p_info.name)) {
		name_to_path[p_info.name] = p_info.path;
	}
	index_secondary_types(p_info);
}

/* ---- Packaged script metadata ------------------------------------------ */

/* An exported game ships empty `.hx` stubs and this manifest: the compiled
 * library holds the code, while the manifest holds the class description the
 * engine needs to attach a script to a node (name, base, exported members,
 * signals). Keeping it out of the `.hx` files is what stops the export from
 * handing out the project's source, the same way Godot's C# export stubs out
 * `.cs` files (see `dotnet/include_scripts_content`). */

static Dictionary member_to_dict(const HaxeMemberInfo &p_member) {
	Dictionary dict;
	dict["name"] = p_member.name;
	dict["type"] = p_member.type;
	dict["default_source"] = p_member.default_source;
	// The default value can be any Variant (a resource path, an array, ...),
	// which JSON cannot carry, so it round-trips through the engine's own
	// text serialization.
	dict["default_value"] = VariantUtilityFunctions::var_to_str(p_member.default_value);
	dict["line"] = p_member.line;
	dict["exported"] = p_member.exported;
	dict["is_static"] = p_member.is_static;
	dict["group"] = p_member.group;
	dict["subgroup"] = p_member.subgroup;
	dict["hint_string"] = p_member.hint_string;
	dict["hint"] = (int)p_member.hint;
	return dict;
}

static HaxeMemberInfo member_from_dict(const Dictionary &p_dict) {
	HaxeMemberInfo member;
	member.name = p_dict.get("name", String());
	member.type = p_dict.get("type", String());
	member.default_source = p_dict.get("default_source", String());
	member.default_value = VariantUtilityFunctions::str_to_var(p_dict.get("default_value", String("null")));
	member.line = p_dict.get("line", 0);
	member.exported = p_dict.get("exported", false);
	member.is_static = p_dict.get("is_static", false);
	member.group = p_dict.get("group", String());
	member.subgroup = p_dict.get("subgroup", String());
	member.hint_string = p_dict.get("hint_string", String());
	member.hint = (PropertyHint)(int)p_dict.get("hint", 0);
	return member;
}

static Dictionary function_to_dict(const HaxeFunctionInfo &p_function) {
	Dictionary dict;
	dict["name"] = p_function.name;
	Array arguments;
	for (const HaxeArgumentInfo &argument : p_function.arguments) {
		Dictionary argument_dict;
		argument_dict["name"] = argument.name;
		argument_dict["type"] = argument.type;
		arguments.push_back(argument_dict);
	}
	dict["arguments"] = arguments;
	dict["return_type"] = p_function.return_type;
	dict["line"] = p_function.line;
	dict["is_static"] = p_function.is_static;
	dict["is_signal"] = p_function.is_signal;
	return dict;
}

static HaxeFunctionInfo function_from_dict(const Dictionary &p_dict) {
	HaxeFunctionInfo function;
	function.name = p_dict.get("name", String());
	Array arguments = p_dict.get("arguments", Array());
	for (int i = 0; i < arguments.size(); i++) {
		Dictionary argument_dict = arguments[i];
		HaxeArgumentInfo argument;
		argument.name = argument_dict.get("name", String());
		argument.type = argument_dict.get("type", String());
		function.arguments.push_back(argument);
	}
	function.return_type = p_dict.get("return_type", String());
	function.line = p_dict.get("line", 0);
	function.is_static = p_dict.get("is_static", false);
	function.is_signal = p_dict.get("is_signal", false);
	return function;
}

static Array members_to_array(const Vector<HaxeMemberInfo> &p_members) {
	Array array;
	for (const HaxeMemberInfo &member : p_members) {
		array.push_back(member_to_dict(member));
	}
	return array;
}

static Vector<HaxeMemberInfo> members_from_array(const Array &p_array) {
	Vector<HaxeMemberInfo> members;
	for (int i = 0; i < p_array.size(); i++) {
		members.push_back(member_from_dict(p_array[i]));
	}
	return members;
}

static Array functions_to_array(const Vector<HaxeFunctionInfo> &p_functions) {
	Array array;
	for (const HaxeFunctionInfo &function : p_functions) {
		array.push_back(function_to_dict(function));
	}
	return array;
}

static Vector<HaxeFunctionInfo> functions_from_array(const Array &p_array) {
	Vector<HaxeFunctionInfo> functions;
	for (int i = 0; i < p_array.size(); i++) {
		functions.push_back(function_from_dict(p_array[i]));
	}
	return functions;
}

static Dictionary class_info_to_dict(const HaxeClassInfo &p_info) {
	Dictionary dict;
	dict["valid"] = p_info.valid;
	dict["path"] = p_info.path;
	dict["package"] = p_info.package;
	dict["name"] = p_info.name;
	dict["base"] = p_info.base;
	dict["fq_name"] = p_info.fq_name;
	dict["class_line"] = p_info.class_line;
	dict["is_tool"] = p_info.is_tool;
	dict["is_abstract"] = p_info.is_abstract;
	dict["is_global_class"] = p_info.is_global_class;
	dict["members"] = members_to_array(p_info.members);
	dict["variables"] = members_to_array(p_info.variables);
	dict["functions"] = functions_to_array(p_info.functions);
	Array secondary_classes;
	for (const String &type_name : p_info.secondary_classes) {
		secondary_classes.push_back(type_name);
	}
	dict["secondary_classes"] = secondary_classes;
	Array secondary;
	for (const HaxeSecondaryTypeInfo &type : p_info.secondary_types) {
		Dictionary type_dict;
		type_dict["name"] = type.name;
		type_dict["base"] = type.base;
		type_dict["line"] = type.line;
		type_dict["is_abstract"] = type.is_abstract;
		type_dict["members"] = members_to_array(type.members);
		type_dict["variables"] = members_to_array(type.variables);
		type_dict["functions"] = functions_to_array(type.functions);
		secondary.push_back(type_dict);
	}
	dict["secondary_types"] = secondary;
	return dict;
}

static HaxeClassInfo class_info_from_dict(const Dictionary &p_dict) {
	HaxeClassInfo info;
	info.valid = p_dict.get("valid", false);
	info.path = p_dict.get("path", String());
	info.package = p_dict.get("package", String());
	info.name = p_dict.get("name", String());
	info.base = p_dict.get("base", String());
	info.fq_name = p_dict.get("fq_name", String());
	info.class_line = p_dict.get("class_line", 0);
	info.is_tool = p_dict.get("is_tool", false);
	info.is_abstract = p_dict.get("is_abstract", false);
	info.is_global_class = p_dict.get("is_global_class", false);
	info.members = members_from_array(p_dict.get("members", Array()));
	info.variables = members_from_array(p_dict.get("variables", Array()));
	info.functions = functions_from_array(p_dict.get("functions", Array()));
	Array secondary_classes = p_dict.get("secondary_classes", Array());
	for (int i = 0; i < secondary_classes.size(); i++) {
		info.secondary_classes.push_back(secondary_classes[i]);
	}
	Array secondary = p_dict.get("secondary_types", Array());
	for (int i = 0; i < secondary.size(); i++) {
		Dictionary type_dict = secondary[i];
		HaxeSecondaryTypeInfo type;
		type.name = type_dict.get("name", String());
		type.base = type_dict.get("base", String());
		type.line = type_dict.get("line", 0);
		type.is_abstract = type_dict.get("is_abstract", false);
		type.members = members_from_array(type_dict.get("members", Array()));
		type.variables = members_from_array(type_dict.get("variables", Array()));
		type.functions = functions_from_array(type_dict.get("functions", Array()));
		info.secondary_types.push_back(type);
	}
	return info;
}

String HaxeProject::get_packaged_metadata_path() {
	return "res://haxe/scripts.json";
}

bool HaxeProject::uses_packaged_metadata() {
	if (packaged_metadata_forced) {
		return true;
	}
#ifdef TOOLS_ENABLED
	// The editor (and a project run from it) always has the real sources and
	// the build directory, even when the "game" is a `tests=yes` build the
	// engine test suite drives with `--test`. There is no manifest in a
	// project directory, and treating one as a game would break the editor.
	return false;
#else
	// Decide from the game itself instead of from the build options. An
	// exported game ships the manifest and empty `.hx` stubs, while a
	// template built from source (Godot's workflows pass `tests=yes` to export
	// templates too) keeps the real sources and has no manifest. Deciding this
	// from the build options misclassified the latter as a game, so a real
	// export parsed the empty stubs and lost every script. The engine tests
	// still exercise both paths explicitly through
	// `set_packaged_metadata_for_testing()`.
	if (packaged_metadata_detected < 0) {
		packaged_metadata_detected = FileAccess::exists(get_packaged_metadata_path()) ? 1 : 0;
	}
	return packaged_metadata_detected == 1;
#endif
}

void HaxeProject::reset_packaged_metadata_state() {
	MutexLock lock(cache_mutex);
	packaged_metadata_loaded = false;
	packaged_metadata_detected = -1;
}

void HaxeProject::set_packaged_metadata_for_testing(bool p_enabled) {
	packaged_metadata_forced = p_enabled;
}

String HaxeProject::serialize_script_metadata() {
	Dictionary document;
	document["version"] = 1;
	Dictionary scripts_dict;
	for (const String &path : scan_sources()) {
		HaxeClassInfo info;
		if (!get_class_info(path, info) || !info.valid) {
			continue;
		}
		// The engine opens scripts through `res://` paths, so the manifest is
		// keyed the same way. `localize_path()` leaves a path outside the
		// project unchanged, which keeps the document usable in tests.
		const String key = ProjectSettings::get_singleton()->localize_path(path);
		Dictionary entry = class_info_to_dict(info);
		entry["path"] = key;
		scripts_dict[key] = entry;
	}
	document["scripts"] = scripts_dict;
	return JSON::stringify(document);
}

bool HaxeProject::load_packaged_script_metadata() {
	return load_script_metadata(get_packaged_metadata_path());
}

bool HaxeProject::load_script_metadata(const String &p_path) {
	const String path = p_path;
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	JSON json;
	if (json.parse(file->get_as_utf8_string()) != OK) {
		ERR_PRINT(vformat("Haxe: cannot parse the packaged script metadata at '%s'.", path));
		return false;
	}
	Dictionary document = json.get_data();
	if (document.is_empty()) {
		return false;
	}
	Dictionary scripts_dict = document.get("scripts", Dictionary());

	HashMap<String, CacheEntry> new_cache;
	HashMap<String, String> new_names;
	HashMap<String, SecondaryEntry> new_secondary;
	Vector<String> files;
	Array keys = scripts_dict.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String script_path = keys[i];
		Dictionary entry_dict = scripts_dict[script_path];
		CacheEntry entry;
		entry.info = class_info_from_dict(entry_dict);
		// The path is the key, so the document stays correct even if a script
		// is moved without its stored path being updated.
		entry.info.path = script_path;
		if (!entry.info.valid) {
			continue;
		}
		files.push_back(script_path);
		if (!entry.info.fq_name.is_empty()) {
			new_names[entry.info.fq_name] = script_path;
		}
		if (!new_names.has(entry.info.name)) {
			new_names[entry.info.name] = script_path;
		}
		for (const HaxeSecondaryTypeInfo &type : entry.info.secondary_types) {
			SecondaryEntry secondary;
			secondary.path = script_path;
			secondary.type_name = type.name;
			if (!entry.info.fq_name.is_empty()) {
				new_secondary[entry.info.fq_name + "." + type.name] = secondary;
			}
			if (!new_secondary.has(type.name)) {
				new_secondary[type.name] = secondary;
			}
		}
		new_cache[script_path] = entry;
	}

	MutexLock lock(cache_mutex);
	cache = new_cache;
	name_to_path = new_names;
	secondary_types = new_secondary;
	indexed_files = files;
	indexed_files.sort();
	index_valid = true;
	index_build_time = Time::get_singleton()->get_ticks_msec();
	return true;
}

String HaxeProject::resolve_native_base(const HaxeClassInfo &p_info) {
	String base = p_info.base;
	HashSet<String> visited;
	while (!base.is_empty()) {
		String clean = base;
		if (clean.contains(".")) {
			clean = clean.get_slice(".", clean.get_slice_count(".") - 1);
		}
		if (visited.has(clean)) {
			return String();
		}
		visited.insert(clean);
		if (ClassDB::class_exists(StringName(clean))) {
			return clean;
		}
		HaxeClassInfo parent;
		if (!get_class_info_by_name(base, parent) && !get_class_info_by_name(clean, parent)) {
			return String();
		}
		base = parent.base;
	}
	return String();
}

String HaxeProject::get_global_class_name(const String &p_path, String *r_base_type, String *r_icon_path, bool *r_is_abstract, bool *r_is_tool) {
	HaxeClassInfo info;
	if (!get_class_info(p_path, info) || !info.valid) {
		return String();
	}
	if (r_is_abstract) {
		*r_is_abstract = info.is_abstract;
	}
	if (r_is_tool) {
		*r_is_tool = info.is_tool;
	}
	if (r_base_type) {
		String native_base = resolve_native_base(info);
		*r_base_type = native_base.is_empty() ? info.base : native_base;
	}
	if (!info.is_global_class) {
		// Only classes annotated with `@:globalClass` are registered with the
		// editor (and offered in the "Add Child Node" dialog). The others stay
		// ordinary scripts.
		return String();
	}
	if (r_icon_path) {
		// No icon: a global class keeps the icon of its native base type, so
		// only the class name is registered here.
		*r_icon_path = String();
	}
	return info.name;
}

Vector<String> HaxeProject::scan_sources() {
	Vector<String> files;
	String source_dir = get_source_dir();
	if (!DirAccess::dir_exists_absolute(source_dir)) {
		return files;
	}
	Vector<String> pending;
	pending.push_back(source_dir);
	while (!pending.is_empty()) {
		String directory = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		Ref<DirAccess> dir = DirAccess::open(directory);
		if (dir.is_null()) {
			continue;
		}
		dir->list_dir_begin();
		String name = dir->get_next();
		while (!name.is_empty()) {
			if (dir->current_is_dir()) {
				if (!name.begins_with(".") && name != "haxelib" && name != "bin") {
					pending.push_back(directory.path_join(name));
				}
			} else if (name.get_extension().to_lower() == "hx") {
				files.push_back(directory.path_join(name));
			}
			name = dir->get_next();
		}
		dir->list_dir_end();
	}
	files.sort();
	return files;
}

String HaxeProject::get_manifest_path() {
	return get_project_root().path_join("haxe.json");
}

Error HaxeProject::load_manifest(Vector<Dependency> &r_dependencies, String &r_error) {
	r_dependencies.clear();
	String path = get_manifest_path();
	if (!FileAccess::exists(path)) {
		return OK;
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		r_error = vformat("Cannot open %s.", path);
		return ERR_CANT_OPEN;
	}
	JSON json;
	Error error = json.parse(file->get_as_utf8_string());
	if (error != OK) {
		r_error = vformat("%s is not valid JSON: %s", path, json.get_error_message());
		return error;
	}
	Dictionary root = json.get_data();
	if (!root.has("dependencies")) {
		return OK;
	}
	Variant dependencies = root["dependencies"];
	if (dependencies.get_type() != Variant::DICTIONARY) {
		r_error = vformat("%s: \"dependencies\" must be an object.", path);
		return ERR_INVALID_DATA;
	}
	Dictionary dictionary = dependencies;
	for (const KeyValue<Variant, Variant> &entry : dictionary) {
		Dependency dependency;
		dependency.name = entry.key;
		if (entry.value.get_type() == Variant::DICTIONARY) {
			Dictionary spec = entry.value;
			dependency.version = spec.has("version") ? String(spec["version"]) : String();
		} else {
			dependency.version = entry.value;
		}
		r_dependencies.push_back(dependency);
	}
	return OK;
}

String HaxeProject::serialize_manifest(const Vector<Dependency> &p_dependencies) {
	Dictionary dependencies;
	for (const Dependency &dependency : p_dependencies) {
		dependencies[dependency.name] = dependency.version;
	}
	Dictionary root;
	// An empty dependency list is written as an empty object: `haxe.json` then
	// has no `dependencies` key at all, which is what the module accepts and
	// what avoids leaving a stale `{}` behind after the last library is
	// removed.
	if (!dependencies.is_empty()) {
		root["dependencies"] = dependencies;
	}
	return JSON::stringify(root, "\t", false, true);
}

Error HaxeProject::save_manifest(const Vector<Dependency> &p_dependencies, String &r_error) {
	String text = serialize_manifest(p_dependencies);
	Ref<FileAccess> file = FileAccess::open(get_manifest_path(), FileAccess::WRITE);
	if (file.is_null()) {
		r_error = vformat("Cannot write %s.", get_manifest_path());
		return ERR_CANT_CREATE;
	}
	file->store_string(text + "\n");
	return OK;
}

String HaxeProject::get_space_free_state_dir(const String &p_leaf) {
	String data = OS::get_singleton()->get_data_path();
	if (!data.is_empty() && !data.contains(" ") && !data.contains("\t")) {
		return data.path_join(OS::get_singleton()->get_godot_dir_name()).path_join(p_leaf);
	}
	String temp = OS::get_singleton()->get_temp_path();
	if (!temp.is_empty() && !temp.contains(" ") && !temp.contains("\t")) {
		return temp.path_join(OS::get_singleton()->get_godot_dir_name()).path_join(p_leaf);
	}
#ifdef WINDOWS_ENABLED
	// Both per-user directories live below `C:/Users/<name>`, so a user name
	// containing a space makes them unusable for a class-path alias. ProgramData
	// is machine-wide and normally `C:/ProgramData`; every user may create
	// subdirectories there.
	String program_data = OS::get_singleton()->get_environment("ProgramData").strip_edges();
	if (!program_data.is_empty() && !program_data.contains(" ") && !program_data.contains("\t")) {
		return program_data.path_join(OS::get_singleton()->get_godot_dir_name()).path_join(p_leaf);
	}
#endif
	return String();
}

bool HaxeProject::project_state_is_space_free() {
	const String build = get_build_dir();
	return !build.is_empty() && build.is_absolute_path() && !build.contains(" ") && !build.contains("\t");
}

void HaxeProject::ensure_editor_ignore(const String &p_dir) {
	if (!DirAccess::dir_exists_absolute(p_dir)) {
		return;
	}
	String path = p_dir.path_join(".gdignore");
	if (FileAccess::exists(path)) {
		return;
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string("");
	}
}

String HaxeProject::initialize_project() {
	String source_dir = get_source_dir();
	Error error = DirAccess::make_dir_recursive_absolute(source_dir);
	if (error != OK && error != ERR_ALREADY_EXISTS) {
		return vformat("Cannot create source directory %s.", source_dir);
	}

	// Keep generated output and libraries out of the resource importer.
	String build_dir = get_build_dir();
	DirAccess::make_dir_recursive_absolute(build_dir);
	ensure_editor_ignore(build_dir);
	String haxelib_dir = get_haxelib_dir();
	DirAccess::make_dir_recursive_absolute(haxelib_dir);
	ensure_editor_ignore(haxelib_dir);

	String manifest = get_manifest_path();
	if (!FileAccess::exists(manifest)) {
		Ref<FileAccess> file = FileAccess::open(manifest, FileAccess::WRITE);
		if (file.is_valid()) {
			// No `dependencies` key while nothing is installed, matching what
			// save_manifest() writes when the last library is removed.
			file->store_string("{}\n");
		}
	}

	// A small starter class so the project builds right after enabling Haxe.
	String main_path = source_dir.path_join("Main.hx");
	if (!FileAccess::exists(main_path)) {
		Ref<FileAccess> file = FileAccess::open(main_path, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(
					"package;\n\n"
					"import godot.Node;\n\n"
					"/** Project entry point. Attach this script to a node to run it. */\n"
					"class Main extends Node {\n"
					"\t@:export var greeting:String = \"Hello from Haxe\";\n\n"
					"\toverride public function _ready():Void {\n"
					"\t\tprint(greeting);\n"
					"\t}\n"
					"}\n");
		}
	}

	return String();
}
