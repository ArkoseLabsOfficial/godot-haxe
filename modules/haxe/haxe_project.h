/**************************************************************************/
/*  haxe_project.h                                                        */
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

/* Project level Haxe configuration and source index.
 *
 * All paths are derived from the Godot project settings so that the editor,
 * the build pipeline and the exporter resolve the same project layout.
 */
class HaxeProject {
public:
	/* Pseudo-ABIs that select the web target. They travel through the same
	 * `p_abi` plumbing as the Android ABIs, so a web library gets its own
	 * build directory (`builds/<stamp>/<Name>.so`) and its own defines, but
	 * they are not Android ABIs and never reach the Android exporter.
	 *
	 * There are two of them because Emscripten's wasm modules record whether
	 * they use a shared (threaded) linear memory, and that declaration has to
	 * match the host engine module. A preset with "Thread Support" enabled is
	 * served by a template whose main module uses shared memory, so its Haxe
	 * library has to be linked with `-pthread` as well. */
	static const char *const WEB_TARGET;
	static const char *const WEB_THREADS_TARGET;
	static bool is_web_target(const String &p_abi);
	/* Whether this web target is the shared-memory (threads) variant. */
	static bool is_web_threads_target(const String &p_abi);

	// Settings names.
	static const char *SETTING_SOURCE_DIR;
	static const char *SETTING_HAXELIB_DIR;
	static const char *SETTING_BUILD_DIR;
	static const char *SETTING_HAXE_EXECUTABLE;
	static const char *SETTING_HAXELIB_EXECUTABLE;
	static const char *SETTING_DEFINES;
	static const char *SETTING_EXTRA_ARGS;
	static const char *SETTING_ALLOW_GLOBAL_HAXELIB;
	static const char *SETTING_BUILD_ON_RUN;
	static const char *SETTING_DEBUG_SYMBOLS;

	static void register_project_settings();

	static String get_project_root();
	static String get_source_dir();
	/* The Haxe package a script at `p_path` belongs to, derived from its
	 * directory relative to the source directory (`res://source/game/Coin.hx`
	 * → `game`). Empty for scripts in the source root or outside it. */
	static String get_package_for_path(const String &p_path);
	static String get_haxelib_dir();
	static String get_build_dir();
	/* Editor and game libraries are built separately so that the editor build
	 * can enable editor-only APIs (`-D hxgodot_editor`). `@:tool` classes are
	 * part of both, matching GDScript's `@tool`. */
	static String get_config_dir(bool p_release, bool p_editor, const String &p_abi = String());
	static String get_output_library_path(bool p_release, bool p_editor, const String &p_abi = String());
	static String get_versioned_library_path(bool p_release, bool p_editor, const String &p_stamp, const String &p_abi = String());
	static String get_generated_dir(bool p_release, bool p_editor, const String &p_abi = String());
	/* The project-local hxcpp build configuration used by the web target. It
	 * sits next to the generated bindings of that target so a project keeps
	 * everything belonging to a build together. `p_abi` is one of the web
	 * targets, since the threads variant needs a different configuration. */
	static String get_web_hxcpp_config_path(bool p_release, bool p_editor, const String &p_abi = WEB_TARGET);
	/* The file name of the compiled library, without its directory. It is
	 * `<Name>.dso`/`.dll`/`.dylib` on the host and `lib<Name>.so` for an
	 * Android ABI, because Godot's Android exporter requires the `lib` prefix
	 * and the dynamic linker resolves the library by that soname. */
	static String get_library_file_name(const String &p_abi = String());
	/* The file hxcpp itself links inside the `cpp` build directory. It is not
	 * always `get_library_file_name()`: the Android toolchain appends the
	 * architecture to the name (`-64` for arm64, `-v7` for armv7, `-x86` and
	 * `-x86_64` for the Intel ABIs) through `LIBEXTRA`. The build copies this
	 * file to the final `lib<Name>.so` name. */
	static String get_hxcpp_output_file_name(const String &p_abi = String());
	/* The name hxcpp is told to give the linked library, before the toolchain
	 * adds its prefix and extension. */
	static String get_library_base_name();
	/* The library name the packaged game loads on Android. The OS extracts
	 * native libraries from the APK into the application's library directory,
	 * so the game opens this soname instead of a `res://` path. */
	static String get_android_library_soname();

	/* The resource path the compiled game library is packaged under in an
	 * exported game (`res://haxe/<Name>.<ext>`, the extension being the host
	 * toolchain's). The export plugin writes that file into the pck; an
	 * exported game has no build directory, so the runtime loads the library
	 * from here instead of building one. */
	static String get_packaged_library_path();
	/* Reads the packaged library out of the exported pck into a real file, so
	 * the platform's dynamic loader can open it. Returns the extracted path,
	 * or an empty string when no library is packaged (the editor, or a project
	 * that has never been exported). */
	static String extract_packaged_library();

	static String get_haxe_executable();
	static String get_haxelib_executable();
	static Vector<String> get_defines();
	static Vector<String> get_extra_args();
	static bool allows_global_haxelib();
	static bool build_on_run();
	static bool debug_symbols_enabled(bool p_release);

	/* Cache maintenance ------------------------------------------------ */

	/* Removes a file or a directory tree, ignoring failures (a loaded library
	 * cannot be deleted on every platform, for example). */
	static Error remove_directory(const String &p_path);
	/* Keeps the newest `p_keep` entries of a directory and deletes the rest.
	 * Used to drop libraries and extraction directories that nothing loads any
	 * more, so a project does not grow without bound. */
	static void prune_directory(const String &p_root, int p_keep, const String &p_always_keep = String());
	/* Runs the cache cleanup for the current project: old versioned libraries,
	 * old support extractions and the per-user directories of earlier module
	 * versions. Called after a successful build. */
	static void prune_caches(const String &p_keep_support_dir = String());

	/* Source index ------------------------------------------------------- */

	static Vector<String> scan_sources();
	static bool get_class_info(const String &p_path, HaxeClassInfo &r_info);
	static bool get_class_info_by_name(const String &p_name, HaxeClassInfo &r_info, String *r_path = nullptr);
	static Vector<HaxeClassInfo> get_all_classes();
	static void invalidate(const String &p_path = String());
	/* Replaces the index entry for `p_info.path` with a freshly parsed result
	 * that has not been written to disk yet (the script editor parses the text
	 * as it is edited). Without this, a subclass that inherits an `@:export`
	 * member would keep reading the previous version of the base class from
	 * the on-disk cache, so its Inspector would lag behind the edit. */
	static void cache_class_info(const HaxeClassInfo &p_info);

	/* The metadata of every project script, serialized as the JSON document an
	 * exported game ships instead of the sources. The editor has the `.hx`
	 * files, but a game only needs the class description to attach scripts and
	 * resolve their base chain, so the exporter replaces each source with an
	 * empty stub and packages this manifest. */
	static String serialize_script_metadata();
	/* Reads the packaged manifest. Exported games call this lazily from the
	 * source index; the editor never does. */
	static bool load_packaged_script_metadata();
	/* The same reader for an explicit path, used by the tests without writing
	 * into the project. */
	static bool load_script_metadata(const String &p_path);
	static String get_packaged_metadata_path();

	/* Whether the source index should be built from the packaged manifest
	 * rather than by scanning the (stubbed) project sources. Decided at run
	 * time from the manifest itself: an exported game ships it, a project
	 * (including an engine-test run in a `tests=yes` template) does not. */
	static bool uses_packaged_metadata();
	/* Marks that `packaged_metadata_loaded` was already attempted and clears
	 * the cached manifest detection, so a game without a manifest reports the
	 * failure once instead of on every lookup. The engine tests reset it
	 * between cases. */
	static void reset_packaged_metadata_state();
	/* Engine tests run inside an editor or a `tests=yes` template, neither of
	 * which is an exported game, so they need a way to exercise the path a game
	 * takes -- and, in the template case, to get the source path back. Nothing
	 * but the tests calls this. */
	static void set_packaged_metadata_for_testing(bool p_enabled);
	/* How many times the whole project index was rebuilt. Tests use it to
	 * assert that editing one script does not reparse the project. */
	static uint64_t get_index_rebuild_count();

	/* Resolves the native ClassDB base class of a script by following Haxe
	 * inheritance between project scripts. */
	static String resolve_native_base(const HaxeClassInfo &p_info);
	static String get_global_class_name(const String &p_path, String *r_base_type = nullptr, String *r_icon_path = nullptr, bool *r_is_abstract = nullptr, bool *r_is_tool = nullptr);

	/* Creates the default project layout (source dir, haxe.json, .gdignore)
	 * without touching existing files. Returns an error message, empty on
	 * success. */
	static String initialize_project();

	/* Creates `<dir>/.gdignore` so the editor does not import directory
	 * contents that the module manages (generated output and the project local
	 * haxelib repository). Does nothing when the directory is absent or the
	 * file already exists. */
	static void ensure_editor_ignore(const String &p_dir);

	/* A per-user directory for module state that has to be reachable through a
	 * path without spaces. Haxe passes every class path to hxcpp as an
	 * unquoted `-I<path>` argument, so a state directory inside a project
	 * stored below `~/My Games/` cannot be used for class-path aliases: the
	 * path would be split and hxcpp would treat a fragment as a build target.
	 * Prefers the user data directory and falls back to the temporary
	 * directory. Returns an empty string when neither is space free. */
	static String get_space_free_state_dir(const String &p_leaf);

	/* Whether the compiler's state (launcher script, class-path aliases) can
	 * live below `haxe/build_directory`. False when the build directory itself
	 * contains a space, in which case `get_space_free_state_dir()` is used. */
	static bool project_state_is_space_free();

	/* Dependency manifest ------------------------------------------------- */

	struct Dependency {
		String name;
		String version; // empty or a concrete version
	};

	static String get_manifest_path();
	static Error load_manifest(Vector<Dependency> &r_dependencies, String &r_error);
	static Error save_manifest(const Vector<Dependency> &p_dependencies, String &r_error);
	/* The `haxe.json` text for `p_dependencies`. An empty list produces an
	 * object with no `dependencies` key, which is what save_manifest() writes
	 * after the last library is removed. */
	static String serialize_manifest(const Vector<Dependency> &p_dependencies);

private:
	struct CacheEntry {
		uint64_t modified = 0;
		HaxeClassInfo info;
		/* The file content changed while the file set did not. The entry is
		 * re-read on the next query, but the project index (and the name map)
		 * stays valid so unrelated lookups do not reparse the whole project. */
		bool stale = false;
	};

	/* A type declared after the primary class of an indexed file. Several
	 * types can share a file, so they need their own index entry; it points at
	 * the file and names the type inside it. */
	struct SecondaryEntry {
		String path;
		String type_name;
	};

	static Mutex cache_mutex;
	static HashMap<String, CacheEntry> cache;
	static HashMap<String, String> name_to_path;
	/* Keyed by import path (`game.File.Second`) and by short name. */
	static HashMap<String, SecondaryEntry> secondary_types;
	static bool index_valid;
	static uint64_t index_build_time;
	/* Whether the packaged manifest was already attempted, so a game without
	 * one reports the error once instead of on every lookup. */
	static bool packaged_metadata_loaded;
	// Sorted list of the source files the index was built from. It is compared
	// against a fresh directory scan so that added or deleted files invalidate
	// the index instead of leaving stale classes behind.
	static Vector<String> indexed_files;
	static uint64_t index_check_time;

	static void rebuild_index();
	static void ensure_index();
	/* Adds the secondary types of a freshly parsed file to the index, dropping
	 * the entries that pointed at it before. The cache lock must be held. */
	static void index_secondary_types(const HaxeClassInfo &p_info);
	/* Drops the primary names a file used to expose. The cache lock must be
	 * held. */
	static void drop_names_for_path(const HaxeClassInfo &p_info, const String &p_path);
	static uint64_t file_modified_time(const String &p_path);
};
