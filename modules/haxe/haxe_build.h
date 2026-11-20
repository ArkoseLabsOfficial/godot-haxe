/**************************************************************************/
/*  haxe_build.h                                                          */
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

#include "core/io/file_access.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/templates/pair.h"
#include "core/templates/vector.h"

/* An external process whose output is read while it runs.
 *
 * `OS::execute()` waits for the command to exit, which froze the editor for the
 * whole compile. This class starts the command with
 * `OS::execute_with_pipe(path, arguments, false)` instead, so the caller keeps
 * running and `poll()` collects whatever the command has written so far. */
class HaxeProcess {
	Ref<FileAccess> stdout_pipe;
	Ref<FileAccess> stderr_pipe;
	ProcessID pid = 0;
	PackedByteArray raw_output;
	Error start_error = OK;
	int exit_code = -1;
	bool started = false;
	bool finished = false;

	void _read_pipes();

public:
	/* Starts `p_executable`. The arguments are passed verbatim: there is no
	 * shell involved, so nothing needs quoting. `p_haxelib_path` is set in the
	 * child's environment only, and `p_environment` adds further variables
	 * (the web target points `HXCPP_CONFIG` at the project-local hxcpp build
	 * configuration). The process is created with the current environment,
	 * which is restored right after the spawn. */
	Error start(const String &p_executable, const List<String> &p_arguments, const String &p_haxelib_path = String(), const Vector<Pair<String, String>> &p_environment = Vector<Pair<String, String>>());
	/* Reads the output written so far. Returns true while the process runs. */
	bool poll();
	bool is_running() const { return started && !finished; }
	bool is_finished() const { return finished; }
	Error get_start_error() const { return start_error; }
	int get_exit_code() const { return exit_code; }
	/* Everything the process wrote so far, decoded on demand so the caller can
	 * follow the output of a running command. */
	String get_output() const;
	void kill();
};

/* Build pipeline for a Godot project written in Haxe.
 *
 * The pipeline:
 *   1. resolves project-local Haxe libraries from haxe.json + haxelib/,
 *   2. generates the entry point that keeps every project script class,
 *   3. invokes the Haxe compiler with the hxcpp shared-library target,
 *   4. parses compiler diagnostics into engine ScriptError records,
 *   5. returns the resulting .dso path.
 *
 * Compilation is incremental: a stamp over sources, libraries and compiler
 * configuration short-circuits the call to Haxe when nothing changed.
 */
class HaxeBuild {
public:
	struct Result {
		bool success = false;
		bool up_to_date = false;
		String library_path;
		String message;
		String raw_output;
		int exit_code = 0;
		Vector<ScriptLanguage::ScriptError> errors;

		static Result failed(const String &p_message) {
			Result result;
			result.message = p_message;
			return result;
		}
	};

	struct Library {
		String name;
		String version;
		String path;
		String class_path;
		bool global = false;
		/* Only meaningful for `list_installed_libraries`: whether the library
		 * is also declared in haxe.json. */
		bool declared = false;
	};

	/* Resolves project dependencies. Missing or conflicting libraries produce
	 * a message that names the exact haxelib command needed to fix it. */
	static Error resolve_dependencies(Vector<Library> &r_libraries, String &r_error);

	/* Lists every library installed in the project-local haxelib directory,
	 * including ones that are not declared in haxe.json. The library manager
	 * shows what is actually on disk, so an installed library never disappears
	 * from the panel just because it is missing from the manifest. */
	static void list_installed_libraries(Vector<Library> &r_libraries);

	/* Builds the project. `p_release` selects the release configuration,
	 * `p_editor` selects the editor library, which additionally contains
	 * @:tool classes. A non-empty `p_abi` cross-compiles for that Android ABI
	 * instead of the host; `p_ndk_root` is the NDK hxcpp should use (it is
	 * ignored for a host build). */
	static Result build(bool p_release, bool p_editor, bool p_force = false, const String &p_abi = String(), const String &p_ndk_root = String());

	/* Compiler arguments used for editor tooling (`haxe --display`): the same
	 * class paths and defines as a real build, without the output target. */
	static Vector<String> collect_display_arguments();

	/* Compiler arguments that make hxcpp cross-compile for an Android ABI. They
	 * are appended to a normal build, so the class paths and defines stay
	 * identical between the host and Android libraries. Exposed for
	 * `collect_arguments()` and for tests that check the toolchain selection
	 * without spawning a compiler. */
	static Vector<String> android_compiler_defines(const String &p_abi, const String &p_ndk_root);

	/* Compiler arguments that make hxcpp link an Emscripten side module for the
	 * web export. `p_hxcpp_config_path` is the file
	 * `write_web_hxcpp_config()` produced. */
	static Vector<String> web_compiler_defines(const String &p_hxcpp_config_path);

	/* Writes the project-local hxcpp configuration that turns hxcpp's
	 * Emscripten link into a side module (`-sSIDE_MODULE=2`) and exports its C
	 * ABI (`-Wl,--export-dynamic`). `p_abi` is one of the web targets; the
	 * threads variant additionally declares a shared linear memory to match the
	 * engine module of a threads-enabled export template. Returns the path, or
	 * an empty string when the file cannot be written. */
	static String write_web_hxcpp_config(bool p_release, bool p_editor, const String &p_abi);

	/* Editor setting that names the `emcc` executable used for web builds. It
	 * is machine state, not project state (like Android's SDK path), because
	 * the editor does not necessarily inherit the PATH of the shell where
	 * emsdk was activated. */
	static const char *SETTING_WEB_EMCC;

	/* Resolves the `emcc` executable of the web target.
	 *
	 * `p_setting` is the editor setting (the executable, `<emsdk>/upstream/emscripten`
	 * or an emsdk root), `p_emsdk` is the `EMSDK` variable and `p_path` is the
	 * PATH to search. They are parameters rather than reads so the resolution
	 * can be tested without changing the editor's environment. `r_error`
	 * explains what is missing; it is empty on success. */
	static bool resolve_web_emcc(const String &p_setting, const String &p_emsdk, const String &p_path, String &r_emcc, String &r_error);

	/* Checks the external tools the web target needs (`emcc`). `r_emcc` receives
	 * the resolved executable, which `web_toolchain_environment()` turns into
	 * the child process environment. `r_error` is filled in when the toolchain
	 * is unavailable. */
	static bool check_web_toolchain(String &r_error, String *r_emcc = nullptr);

	/* Environment variables that make hxcpp build with the `emcc` that
	 * `check_web_toolchain()` resolved instead of whatever it would find on its
	 * own. */
	static Vector<Pair<String, String>> web_toolchain_environment(const String &p_emcc);

	/* The character that separates the entries of `PATH`-style environment
	 * variables: `;` on Windows, `:` everywhere else. `find_in_path()` and
	 * `web_toolchain_environment()` both split or join a list with it, and the
	 * engine tests build one to exercise them. */
	static String path_list_separator();

	/* Identity of a web toolchain: the executable and its modification time.
	 * Objects and libraries produced by one Emscripten are not interchangeable
	 * with another's (the dynamic-linking encoding and the shared-memory
	 * declaration differ), so the identity is part of the build stamp and the
	 * recorded hxcpp working directory. Exposed for the engine tests. */
	static String web_toolchain_identity(const String &p_emcc);

#ifdef TOOLS_ENABLED
	/* Registers `SETTING_WEB_EMCC` with a file dialog in the Editor Settings,
	 * so the Emscripten installation can be selected the way Android's SDK
	 * path is. */
	static void register_web_editor_settings();
#endif

	/* Builds the compiler argument list. Class paths that the platform
	 * generator cannot receive verbatim (a path containing a space) are
	 * replaced by a space-free alias; the alias -> real mapping is appended to
	 * `r_class_path_aliases` so diagnostics can be translated back. Exposed so
	 * the class-path aliasing can be tested without spawning a compiler. */
	static Vector<String> collect_arguments(bool p_release, bool p_editor, const Vector<Library> &p_libraries, const Vector<HaxeClassInfo> &p_script_classes, Vector<Pair<String, String>> &r_class_path_aliases, const String &p_abi = String(), const String &p_ndk_root = String());

	/* haxelib repository used while compiling. The project-local repository is
	 * used when it contains hxcpp; otherwise the machine wide repository that
	 * provides the Haxe toolchain is used. */
	static String get_compile_haxelib_path();

	/* Returns true when a build is needed for the current sources. */
	static bool is_build_required(bool p_release, bool p_editor, const String &p_abi = String());

	/* Deterministic stamp of the current build inputs. Used to version the
	 * produced shared library so a rebuild never overwrites a library the
	 * process has already loaded. */
	static String current_stamp(bool p_release, bool p_editor, const String &p_abi = String());

	/* Installs a library into the project-local haxelib directory. Uses the
	 * haxelib executable with HAXELIB_PATH pointing at the project, so it
	 * never touches the machine wide repository. With
	 * `p_skip_dependencies` the library's own dependencies are not installed
	 * either (`haxelib install --skip-dependencies`). */
	static Error install_library(const String &p_name, const String &p_version, bool p_skip_dependencies, String &r_output);
	static Error update_library(const String &p_name, String &r_output);
	static Error remove_library(const String &p_name, String &r_output);

	/* The haxelib command lines behind the three calls above, exposed so the
	 * library manager can run them without waiting for them (they are the
	 * ones that download, and therefore take a while). */
	static Error prepare_haxelib_repository(String &r_output);
	static List<String> install_arguments(const String &p_name, const String &p_version, bool p_skip_dependencies);
	static List<String> update_arguments(const String &p_name);
	static List<String> remove_arguments(const String &p_name);
	/* Records a finished `haxelib install` in haxe.json, so the build resolves
	 * the library it just downloaded. */
	static Error record_installed_library(const String &p_name, const String &p_version, String &r_output);
	/* Forgets a library that was removed from the repository. */
	static Error forget_removed_library(const String &p_name, String &r_output);
	/* Percentage ([0, 100]) of the download reported by the last progress
	 * line of haxelib output, or -1 when the output holds none. haxelib
	 * rewrites a single line (" 42%\r") instead of printing new lines. */
	static int parse_progress(const String &p_output);

	/* Checks that the external tools are usable. The returned string is empty
	 * when everything is available, otherwise it describes what is missing. */
	static String check_toolchain();

	/* Splits the contents of an `extraParams.hxml` file into compiler
	 * arguments. The hxml syntax separates arguments with whitespace, uses
	 * single or double quotes to group a token that contains spaces, and
	 * starts a comment with `#`. `-lib`, `-main` and `-cp` are dropped
	 * together with their value, because this module resolves the libraries,
	 * the class paths and the entry point itself. */
	static Vector<String> parse_extra_params(const String &p_contents);

	/* Normalizes the project's `haxe/compiler/extra_arguments` setting into
	 * individual compiler arguments. An entry such as
	 * `--macro keep('IntIterator')` contains the option and its value, which
	 * the compiler would otherwise reject as one unknown option, so it is
	 * split at the first space like an hxml line. */
	static Vector<String> normalize_extra_arguments(const Vector<String> &p_arguments);

	/* The compiler diagnostics of a failed build, one per line (`path:line:
	 * message`). `Result::message` only carries a count for a compile failure,
	 * so a caller that cannot show the parsed errors (an exported game's
	 * runtime build, the export panel) uses this to tell the user which file
	 * and line to fix. */
	static String format_errors(const Result &p_result);

	static String get_last_build_log();

private:
	friend class HaxeBuildSession;

	static String build_stamp(bool p_release, bool p_editor, const String &p_abi);
	static String generate_boot_class(const Vector<HaxeClassInfo> &p_classes);
	static void parse_diagnostics(const String &p_output, const Vector<Pair<String, String>> &p_class_path_aliases, Result &r_result);
	static String read_library_version(const String &p_library_dir, const String &p_requested);
};

/* One compilation, split into start / poll / finish so the editor can drive it
 * from a timer instead of blocking on the compiler.
 *
 * `prepare()` does the work in front of the compiler (resolving libraries,
 * writing the generated entry point, checking the build stamp) and then starts
 * the compiler through `HaxeProcess`. `poll()` collects output while it runs.
 * `finish()` parses the diagnostics, installs the versioned library and prunes
 * old outputs. `HaxeBuild::build()` is the blocking wrapper around the same
 * steps, so both paths share one implementation. */
class HaxeBuildSession {
	bool running = false;
	bool finished = false;

	HaxeProcess process;
	HaxeBuild::Result result;

	String stamp;
	String stamp_path;
	String output_path;
	String versioned_path;
	String abi;
	String ndk_root;
	/* Set for a web build: the `emcc` that was resolved and its identity. The
	 * identity is recorded next to the hxcpp working directory so a build made
	 * by another toolchain is never reused. */
	String web_emcc;
	String web_toolchain;
	Vector<Pair<String, String>> class_path_aliases;
	/* Variables the compiler process needs on top of the editor's environment
	 * (the web target points `HXCPP_CONFIG` at the generated hxcpp config). */
	Vector<Pair<String, String>> child_environment;

	/* Runs every step before the compiler is needed. Sets `finished` when the
	 * build is already complete (up to date) or cannot start. */
	void prepare(bool p_release, bool p_editor, bool p_force);
	/* Post-processes the finished compiler run. */
	void finish();
	/* Records the toolchain a finished web build used, next to the stamp, so
	 * the next build can tell whether its objects are reusable. */
	void record_web_toolchain();

public:
	~HaxeBuildSession();

	/* Prepares and starts the build. `is_running()` is false when it is
	 * already finished, in which case `get_result()` is final. */
	void start(bool p_release, bool p_editor, bool p_force = false, const String &p_abi = String(), const String &p_ndk_root = String());
	/* Reads the compiler output and, once it exited, post-processes it.
	 * Returns true while the build is still running. */
	bool poll();
	bool is_running() const { return running; }
	bool is_finished() const { return finished; }
	/* Output of the compiler, available while it runs. */
	String get_output() const { return process.get_output(); }
	const HaxeBuild::Result &get_result() const { return result; }
	/* Terminates the compiler and finishes the build with an error. */
	void cancel();
};
