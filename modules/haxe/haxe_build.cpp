/**************************************************************************/
/*  haxe_build.cpp                                                        */
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

#include "haxe_build.h"

#include "haxe_project.h"
#include "haxe_runtime.h"

#include "core/crypto/hashing_context.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"

#ifdef TOOLS_ENABLED
#include "editor/settings/editor_settings.h"
#endif

const char *HaxeBuild::SETTING_WEB_EMCC = "haxe/web/emcc_path";

namespace {

String last_build_log;

/* The C ABI entry points every project library defines (`dist/toolchain/glue.cpp`).
 * The web build has to name them explicitly so they end up in the side module's
 * export table; the other targets export them through the platform's default
 * symbol visibility. Kept in the same order as `dist/toolchain/hxgodot_abi.h`. */
const char *const HXGODOT_WEB_EXPORTS[] = {
	"hxgodot_install",
	"hxgodot_start",
	"hxgodot_last_error",
	"hxgodot_last_stack_trace",
	"hxgodot_library_info",
	"hxgodot_has_class",
	"hxgodot_create",
	"hxgodot_has_method",
	"hxgodot_invoke",
	"hxgodot_get_field",
	"hxgodot_set_field",
	"hxgodot_get_members",
	"hxgodot_destroy",
	"hxgodot_release_return",
	"hxgodot_invoke_closure",
	"hxgodot_retain_closure",
	"hxgodot_release_closure",
	nullptr,
};

String sanitize_json_version(const Variant &p_value) {
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = p_value;
		if (dictionary.has("version")) {
			return dictionary["version"];
		}
		return String();
	}
	return p_value;
}

bool version_satisfies(const String &p_available, const String &p_requirement) {
	if (p_requirement.is_empty()) {
		return true;
	}
	if (p_requirement == p_available) {
		return true;
	}
	if (p_requirement.begins_with(">=")) {
		String minimum = p_requirement.substr(2).strip_edges();
		PackedStringArray available_parts = p_available.split(".");
		PackedStringArray minimum_parts = minimum.split(".");
		for (int i = 0; i < MAX(available_parts.size(), minimum_parts.size()); i++) {
			int available = i < available_parts.size() ? available_parts[i].to_int() : 0;
			int required = i < minimum_parts.size() ? minimum_parts[i].to_int() : 0;
			if (available != required) {
				return available > required;
			}
		}
		return true;
	}
	// Plain requirement: treat it as an exact version. Haxelib itself has no
	// version solver, so refusing ambiguous requirements keeps builds
	// reproducible instead of silently picking something.
	return false;
}

bool is_version_greater(const String &p_a, const String &p_b) {
	PackedStringArray a = p_a.split(".");
	PackedStringArray b = p_b.split(".");
	for (int i = 0; i < MAX(a.size(), b.size()); i++) {
		int left = i < a.size() ? a[i].to_int() : 0;
		int right = i < b.size() ? b[i].to_int() : 0;
		if (left != right) {
			return left > right;
		}
	}
	return p_a > p_b;
}

Vector<String> read_directory(const String &p_path) {
	Vector<String> entries;
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return entries;
	}
	dir->list_dir_begin();
	String name = dir->get_next();
	while (!name.is_empty()) {
		if (!name.begins_with(".") && dir->current_is_dir()) {
			entries.push_back(name);
		}
		name = dir->get_next();
	}
	dir->list_dir_end();
	return entries;
}

bool write_if_changed(const String &p_path, const String &p_contents) {
	if (FileAccess::exists(p_path)) {
		Ref<FileAccess> existing = FileAccess::open(p_path, FileAccess::READ);
		if (existing.is_valid() && existing->get_as_utf8_string() == p_contents) {
			return true;
		}
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_string(p_contents);
	return true;
}

/* Transient compiler state (launcher scripts, class path aliases) lives in
 * `haxe/build_directory` so a project keeps everything it generates together.
 * Only when there is no usable project directory (no project open, or a
 * read-only location) does it fall back to the user data directory. */
String compiler_state_dir(const String &p_leaf, const String &p_legacy_name) {
	String build = HaxeProject::get_build_dir();
	// Haxe hands every class path to hxcpp as an unquoted `-I<path>`
	// argument, so a class-path alias has to live on a path without spaces.
	// Projects stored below `~/My Games/` (or any other path with a space)
	// cannot host the alias themselves; hxcpp splits the path and treats the
	// remainder as a build target ("Could not find build target ...").
	if (HaxeProject::project_state_is_space_free()) {
		return build.path_join(p_leaf);
	}
	String base = HaxeProject::get_space_free_state_dir(p_legacy_name);
	if (base.is_empty()) {
		return String();
	}
	// Keyed by the project: class-path aliases and work files are per
	// project, and two spaced projects must not retarget each other's links.
	return base.path_join(HaxeProject::get_project_root().md5_text().substr(0, 12));
}

/* `OS::execute()` builds a shell command line whenever it captures output and
 * wraps every argument in double quotes without escaping its contents. A Haxe
 * argument may contain a quote, a dollar sign or a backtick (macro calls such
 * as `keep("IntIterator")` do), which the shell would otherwise mangle.
 * Escaping the argument here keeps it intact inside the generated quotes. */
#ifdef UNIX_ENABLED
String escape_shell_argument(const String &p_argument) {
	String escaped = p_argument.replace("\\", "\\\\");
	escaped = escaped.replace("\"", "\\\"");
	escaped = escaped.replace("$", "\\$");
	escaped = escaped.replace("`", "\\`");
	return escaped;
}
#endif

/* Prepares `p_executable` and `r_arguments` for the platform's process API.
 *
 * `CreateProcess` can only start an executable image, so a `.bat`/`.cmd`
 * wrapper has to be run by the command interpreter. Windows installations of
 * the Haxe toolchain and of Emscripten ship such wrappers (`emcc.bat` is the
 * only `emcc` in an emsdk installation), and the editor setting may name one
 * directly. Everywhere else the command is passed through unchanged. */
void prepare_process(const String &p_executable, const List<String> &p_arguments, String &r_executable, List<String> &r_arguments) {
	r_executable = p_executable;
	r_arguments = p_arguments;
#ifdef WINDOWS_ENABLED
	const String extension = p_executable.get_extension().to_lower();
	if (extension == "bat" || extension == "cmd") {
		r_arguments.push_front(p_executable);
		r_arguments.push_front("/c");
		r_executable = "cmd.exe";
	}
#endif
}

Error run_process(const String &p_executable, const List<String> &p_arguments, String &r_output, int &r_exit_code, const String &p_haxelib_path = String()) {
	String previous_haxelib;
	bool had_haxelib = OS::get_singleton()->has_environment("HAXELIB_PATH");
	if (had_haxelib) {
		previous_haxelib = OS::get_singleton()->get_environment("HAXELIB_PATH");
	}
	if (!p_haxelib_path.is_empty()) {
		OS::get_singleton()->set_environment("HAXELIB_PATH", p_haxelib_path);
	}
	String executable;
	List<String> arguments;
	prepare_process(p_executable, p_arguments, executable, arguments);
#ifdef UNIX_ENABLED
	arguments.clear();
	for (const String &argument : p_arguments) {
		arguments.push_back(escape_shell_argument(argument));
	}
#endif
	Error error = OS::get_singleton()->execute(executable, arguments, &r_output, &r_exit_code, true);
	if (had_haxelib) {
		OS::get_singleton()->set_environment("HAXELIB_PATH", previous_haxelib);
	} else {
		OS::get_singleton()->unset_environment("HAXELIB_PATH");
	}
	return error;
}

/* Resolves the command that runs the Haxe compiler. Every path handed to the
 * compiler is absolute, so changing the working directory does not change the
 * result; leaving the project directory avoids the shadowing described above. */
void resolve_haxe_command(const List<String> &p_arguments, String &r_executable, List<String> &r_arguments) {
	r_executable = HaxeProject::get_haxe_executable();
	r_arguments = p_arguments;
#ifdef UNIX_ENABLED
	Ref<DirAccess> cwd_access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	String cwd = cwd_access.is_valid() ? cwd_access->get_current_dir() : String();
	if (cwd.is_empty() || (!DirAccess::dir_exists_absolute(cwd.path_join("haxelib")) && !FileAccess::exists(cwd.path_join("haxelib")))) {
		return;
	}
	String work_dir = compiler_state_dir("work", "haxe_work");
	if (!work_dir.is_empty()) {
		work_dir = work_dir.path_join(cwd.md5_text().substr(0, 12));
		DirAccess::make_dir_recursive_absolute(work_dir);
	}
	String haxe_executable = HaxeProject::get_haxe_executable().replace("\"", "\\\"");
	if (!work_dir.is_empty()) {
		String launcher = work_dir.path_join("run.sh");
		// The script is passed to `sh` as a file, so its contents are not
		// reparsed by the shell `OS::execute()` uses to capture the output.
		String script = "#!/bin/sh\n";
		script += "# Generated by the Godot Haxe module. Do not edit.\n";
		script += "# Haxe resolves the haxelib program from the working directory\n";
		script += "# before PATH, so the compiler is started where the project's\n";
		script += "# haxelib/ directory cannot shadow the real one.\n";
		script += "cd \"" + work_dir.replace("\"", "\\\"") + "\" || exit 1\n";
		script += "exec \"" + haxe_executable + "\" \"$@\"\n";
		// Written through a temporary file and renamed, so a concurrently
		// starting build cannot read a partially written script.
		String temporary = launcher + ".tmp";
		Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(script);
			file.unref();
			DirAccess::rename_absolute(temporary, launcher);
			r_executable = "/bin/sh";
			r_arguments.clear();
			r_arguments.push_back(launcher);
			for (const String &argument : p_arguments) {
				r_arguments.push_back(argument);
			}
			return;
		}
	}
	// No launcher file could be written (a read-only or not yet existing data
	// directory, for example). Start the compiler through an inline `sh -c`
	// script instead: it changes the directory without touching the
	// filesystem, so the project's `haxelib/` directory can never be picked
	// up as the haxelib executable.
	if (!work_dir.is_empty()) {
		r_executable = "/bin/sh";
		r_arguments.clear();
		r_arguments.push_back("-c");
		r_arguments.push_back("cd \"" + work_dir.replace("\"", "\\\"") + "\" || exit 1; exec \"" + haxe_executable + "\" \"$@\"");
		r_arguments.push_back("haxe");
		for (const String &argument : p_arguments) {
			r_arguments.push_back(argument);
		}
	}
#endif
}

/* Runs a command and waits for it. Used where blocking is acceptable or
 * required (the version check, the module build callbacks). */
Error run_haxe(const List<String> &p_arguments, String &r_output, int &r_exit_code, const String &p_haxelib_path = String()) {
	String executable;
	List<String> arguments;
	resolve_haxe_command(p_arguments, executable, arguments);
	return run_process(executable, arguments, r_output, r_exit_code, p_haxelib_path);
}

/* Creates the directory alias used to hide a class path containing a space.
 *
 * `DirAccess::create_link()` uses `CreateSymbolicLinkW()` on Windows, which
 * only succeeds when Developer Mode is enabled or the editor is elevated. A
 * directory junction needs neither and is enough here: the compiler only reads
 * through the alias. It is created with `mklink /J`, which is part of the
 * command interpreter on every supported Windows version. */
Error create_class_path_alias(const String &p_real, const String &p_alias) {
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_null()) {
		return FAILED;
	}
	if (dir->create_link(p_real, p_alias) == OK) {
		return OK;
	}
#ifdef WINDOWS_ENABLED
	List<String> arguments;
	arguments.push_back("/c");
	arguments.push_back("mklink");
	arguments.push_back("/J");
	arguments.push_back(p_alias);
	arguments.push_back(p_real);
	String output;
	int exit_code = 0;
	if (run_process("cmd.exe", arguments, output, exit_code) == OK && exit_code == 0) {
		return OK;
	}
	ERR_PRINT(vformat("Could not create a directory junction '%s' -> '%s': %s", p_alias, p_real, output.strip_edges()));
#endif
	return FAILED;
}

/* Haxe hands every `-cp` class path to the platform generator (hxcpp) as an
 * unquoted `-I<path>/` command line argument. A class path containing a space
 * is therefore split into several arguments and hxcpp interprets the stray
 * fragment as a build target, which surfaces as
 * `Error: Could not find build target "..."`. Such a class path is replaced by
 * a symbolic link below the user cache directory whose name contains no space;
 * the compiler then reports diagnostics for the alias and
 * `translate_class_path()` maps them back to the real file. */
class ClassPathAlias {
public:
	String map(const String &p_real) {
		if (!needs_alias(p_real)) {
			return p_real;
		}
		for (int i = 0; i < aliases.size(); i++) {
			if (reals[i] == p_real) {
				return aliases[i];
			}
		}

		if (root.is_empty()) {
			root = compiler_state_dir("classpath", "haxe_class_path");
			if (root.is_empty()) {
				return p_real;
			}
			Ref<DirAccess> base = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
			if (base.is_valid()) {
				base->make_dir_recursive(root);
			}
		}

		String alias = root.path_join(vformat("cp%d", aliases.size()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (dir.is_valid()) {
			if (dir->is_link(alias) && dir->read_link(alias) != p_real) {
				dir->remove(alias);
			}
			if (!dir->is_link(alias) && create_class_path_alias(p_real, alias) != OK) {
				// The filesystem refused the link. Fall back to the real path;
				// the build then reports the original class path problem.
				return p_real;
			}
		}
		aliases.push_back(alias);
		reals.push_back(p_real);
		return alias;
	}

	void append_mappings(Vector<Pair<String, String>> &r_mappings) const {
		for (int i = 0; i < aliases.size(); i++) {
			r_mappings.push_back(Pair<String, String>(aliases[i], reals[i]));
		}
	}

private:
	static bool needs_alias(const String &p_path) {
		return p_path.contains(" ") || p_path.contains("\t");
	}

	String root;
	Vector<String> aliases;
	Vector<String> reals;
};

String translate_class_path(const String &p_path, const Vector<Pair<String, String>> &p_mappings) {
	for (const Pair<String, String> &mapping : p_mappings) {
		if (p_path.begins_with(mapping.first)) {
			return mapping.second + p_path.substr(mapping.first.length());
		}
	}
	return p_path;
}

/* The last non-empty lines of a process dump. Compiler errors that the
 * diagnostic parser could not attribute to a file are surfaced through these,
 * so the report names the actual complaint instead of the full tool output
 * (which stays available in the build log). */
String notable_tail(const String &p_output, int p_max_lines = 5) {
	Vector<String> lines;
	for (const String &raw_line : p_output.split("\n")) {
		String line = raw_line.strip_edges();
		if (!line.is_empty()) {
			lines.push_back(line);
		}
	}
	String tail;
	for (int i = MAX(0, lines.size() - p_max_lines); i < lines.size(); i++) {
		tail += lines[i] + "\n";
	}
	return tail.strip_edges();
}

/* haxelib repository used while compiling.
 *
 * Project dependencies are resolved by this module and handed to the compiler
 * as explicit class paths, so the compiler never asks haxelib for them.
 * haxelib is only involved in running hxcpp, the code generator behind the
 * `cpp` target. That generator belongs to the Haxe installation and, like the
 * haxe executable itself, is allowed to live in the machine wide repository; a
 * copy inside the project's haxelib directory takes precedence so a project
 * can pin its own toolchain. */
String compile_haxelib_path() {
	String project_repo = HaxeProject::get_haxelib_dir();
	if (DirAccess::dir_exists_absolute(project_repo.path_join("hxcpp"))) {
		return project_repo;
	}
	if (OS::get_singleton()->has_environment("HAXELIB_PATH")) {
		String value = OS::get_singleton()->get_environment("HAXELIB_PATH").strip_edges();
		if (!value.is_empty()) {
			return value;
		}
	}
	List<String> arguments;
	arguments.push_back("config");
	String output;
	int exit_code = 0;
	if (run_process(HaxeProject::get_haxelib_executable(), arguments, output, exit_code) == OK && exit_code == 0) {
		String value = output.strip_edges();
		if (!value.is_empty()) {
			return value;
		}
	}
	// Empty keeps whatever the environment already provides.
	return String();
}

/* Creates the project haxelib repository if needed. haxelib refuses to install
 * into a directory that does not exist. */
bool ensure_haxelib_repository(String &r_error) {
	String root = HaxeProject::get_haxelib_dir();
	if (DirAccess::dir_exists_absolute(root)) {
		HaxeProject::ensure_editor_ignore(root);
		return true;
	}
	Error error = DirAccess::make_dir_recursive_absolute(root);
	if (error != OK) {
		r_error = vformat("Cannot create the project haxelib directory at %s.", root);
		return false;
	}
	HaxeProject::ensure_editor_ignore(root);
	return true;
}

/* Distinct packages of the script classes.
 *
 * The build marks every type in these packages with `@:keep`. The engine
 * reaches script members by name through reflection, which `-dce full` cannot
 * see, so without the annotation an override such as `_process` would vanish
 * from the compiled library while the parsed script still reports it.
 *
 * `@:tool` classes are part of both the editor and the game library, matching
 * GDScript's `@tool`: the annotation controls whether the code runs while the
 * editor has scripting turned off, not whether the class exists outside the
 * editor. */
Vector<String> keep_packages(const Vector<HaxeClassInfo> &p_classes) {
	Vector<String> packages;
	for (const HaxeClassInfo &info : p_classes) {
		if (info.is_abstract || info.fq_name.is_empty()) {
			continue;
		}
		int separator = info.fq_name.rfind(".");
		String package = separator > 0 ? info.fq_name.substr(0, separator) : String();
		if (!packages.has(package)) {
			packages.push_back(package);
		}
	}
	return packages;
}

/* Strips one pair of matching quotes around a whole hxml token, mirroring the
 * compiler's own hxml loader (`Helper.unquote`). */
String unquote_hxml_token(const String &p_value) {
	int length = p_value.length();
	if (length >= 2) {
		char32_t first = p_value[0];
		if ((first == '"' || first == '\'') && p_value[length - 1] == first) {
			return p_value.substr(1, length - 2);
		}
	}
	return p_value;
}

/* Appends one entry of the project's extra compiler arguments.
 *
 * The setting is a list, but users coming from hxml often type the option and
 * its value in a single entry (`--macro keep('IntIterator')`). Passed through
 * verbatim, the compiler sees one argument and rejects it with
 *   Error: unknown option '--macro keep('IntIterator')'
 * so such an entry is split the way the hxml loader splits a line: at the
 * first space, with matching quotes removed. */
void append_extra_argument(Vector<String> &r_arguments, const String &p_argument) {
	String argument = unquote_hxml_token(p_argument.strip_edges());
	if (argument.is_empty()) {
		return;
	}
	if (argument.begins_with("-") && argument.contains(" ")) {
		int space = argument.find_char(' ');
		r_arguments.push_back(argument.substr(0, space));
		r_arguments.push_back(unquote_hxml_token(argument.substr(space + 1).strip_edges()));
		return;
	}
	r_arguments.push_back(argument);
}

} // namespace

/* ------------------------------------------------------------- processes -- */

void HaxeProcess::_read_pipes() {
	Ref<FileAccess> pipes[2] = { stdout_pipe, stderr_pipe };
	uint8_t buffer[4096];
	for (int i = 0; i < 2; i++) {
		Ref<FileAccess> pipe = pipes[i];
		if (pipe.is_null()) {
			continue;
		}
		// `get_length()` reports how many bytes the pipe holds (FIONREAD on
		// Unix, PeekNamedPipe on Windows), so the read below never waits.
		int available = (int)pipe->get_length();
		while (available > 0) {
			uint64_t wanted = (uint64_t)MIN(available, (int)sizeof(buffer));
			uint64_t read = pipe->get_buffer(buffer, wanted);
			if (read == 0) {
				break;
			}
			int offset = raw_output.size();
			raw_output.resize(offset + (int)read);
			memcpy(raw_output.ptrw() + offset, buffer, read);
			available -= (int)read;
		}
	}
}

Error HaxeProcess::start(const String &p_executable, const List<String> &p_arguments, const String &p_haxelib_path, const Vector<Pair<String, String>> &p_environment) {
	ERR_FAIL_COND_V(started, ERR_ALREADY_IN_USE);
	started = true;

	String previous_haxelib;
	bool had_haxelib = OS::get_singleton()->has_environment("HAXELIB_PATH");
	if (had_haxelib) {
		previous_haxelib = OS::get_singleton()->get_environment("HAXELIB_PATH");
	}
	if (!p_haxelib_path.is_empty()) {
		OS::get_singleton()->set_environment("HAXELIB_PATH", p_haxelib_path);
	}
	// The child receives a copy of the environment when it is created, so these
	// variables are only set for the duration of the spawn.
	Vector<String> previous_values;
	Vector<bool> had_values;
	for (const Pair<String, String> &entry : p_environment) {
		const bool had_value = OS::get_singleton()->has_environment(entry.first);
		had_values.push_back(had_value);
		previous_values.push_back(had_value ? OS::get_singleton()->get_environment(entry.first) : String());
		OS::get_singleton()->set_environment(entry.first, entry.second);
	}
	String executable;
	List<String> arguments;
	prepare_process(p_executable, p_arguments, executable, arguments);
	Dictionary pipes = OS::get_singleton()->execute_with_pipe(executable, arguments, false);
	for (int i = p_environment.size() - 1; i >= 0; i--) {
		if (had_values[i]) {
			OS::get_singleton()->set_environment(p_environment[i].first, previous_values[i]);
		} else {
			OS::get_singleton()->unset_environment(p_environment[i].first);
		}
	}
	if (had_haxelib) {
		OS::get_singleton()->set_environment("HAXELIB_PATH", previous_haxelib);
	} else {
		OS::get_singleton()->unset_environment("HAXELIB_PATH");
	}

	if (!pipes.has("stdio") || !pipes.has("pid")) {
		start_error = ERR_CANT_FORK;
		finished = true;
		return start_error;
	}
	stdout_pipe = pipes["stdio"];
	stderr_pipe = pipes["stderr"];
	pid = pipes["pid"];
	return OK;
}

bool HaxeProcess::poll() {
	if (!started || finished) {
		return false;
	}
	_read_pipes();
	if (OS::get_singleton()->is_process_running(pid)) {
		return true;
	}
	// The process has exited; everything it wrote is still in the pipes.
	// `get_process_exit_code()` works after the exit status was reaped.
	_read_pipes();
	exit_code = OS::get_singleton()->get_process_exit_code(pid);
	finished = true;
	return false;
}

String HaxeProcess::get_output() const {
	if (raw_output.is_empty()) {
		return String();
	}
	return String::utf8((const char *)raw_output.ptr(), raw_output.size());
}

void HaxeProcess::kill() {
	if (!started || finished) {
		return;
	}
	OS::get_singleton()->kill(pid);
	_read_pipes();
	finished = true;
}

Vector<String> HaxeBuild::normalize_extra_arguments(const Vector<String> &p_arguments) {
	Vector<String> arguments;
	for (const String &argument : p_arguments) {
		append_extra_argument(arguments, argument);
	}
	return arguments;
}

/* Reads the contents of an `extraParams.hxml` file.
 *
 * The syntax follows the compiler's own hxml loader: one option per line, a `#`
 * at the beginning of a line starts a comment, and everything after the first
 * space is the option's value. That value may contain spaces and may be wrapped
 * in a pair of quotes that is removed. `-lib`, `-main` and `-cp` are removed
 * together with their value: this module resolves the libraries, the class
 * paths and the entry point itself, and a relative `-cp` from inside a library
 * would point at the wrong directory. */
Vector<String> HaxeBuild::parse_extra_params(const String &p_contents) {
	Vector<String> tokens;
	for (const String &raw_line : p_contents.split("\n")) {
		String line = unquote_hxml_token(raw_line.strip_edges());
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		int space = line.find_char(' ');
		if (line.begins_with("-") && space >= 0) {
			tokens.push_back(line.substr(0, space));
			tokens.push_back(unquote_hxml_token(line.substr(space + 1).strip_edges()));
		} else {
			tokens.push_back(line);
		}
	}

	Vector<String> arguments;
	for (int i = 0; i < tokens.size(); i++) {
		const String &token = tokens[i];
		if (token == "-lib" || token == "-main" || token == "-cp") {
			i++; // Skip the option's value as well.
			continue;
		}
		arguments.push_back(token);
	}
	return arguments;
}

String HaxeBuild::get_last_build_log() {
	return last_build_log;
}

String HaxeBuild::read_library_version(const String &p_library_dir, const String &p_requested) {
	if (!p_requested.is_empty() && DirAccess::dir_exists_absolute(p_library_dir.path_join(p_requested))) {
		return p_requested;
	}
	String current_path = p_library_dir.path_join(".current");
	if (FileAccess::exists(current_path)) {
		Ref<FileAccess> file = FileAccess::open(current_path, FileAccess::READ);
		if (file.is_valid()) {
			String current = file->get_as_utf8_string().strip_edges();
			if (!current.is_empty() && DirAccess::dir_exists_absolute(p_library_dir.path_join(current))) {
				if (p_requested.is_empty() || version_satisfies(current, p_requested)) {
					return current;
				}
			}
		}
	}
	String best;
	// A library that was copied into the haxelib directory by hand carries
	// haxelib.json in the library directory itself rather than in a version
	// subdirectory. Its own class path folders are subdirectories as well, so
	// scanning for a version here would pick a package directory (for example
	// `mylib/` inside `mylib/`). An empty result makes the caller treat the
	// library directory as the library root.
	if (FileAccess::exists(p_library_dir.path_join("haxelib.json"))) {
		return String();
	}
	for (const String &version : read_directory(p_library_dir)) {
		if (p_requested.is_empty() || version_satisfies(version, p_requested)) {
			if (best.is_empty() || is_version_greater(version, best)) {
				best = version;
			}
		}
	}
	return best;
}

Error HaxeBuild::resolve_dependencies(Vector<Library> &r_libraries, String &r_error) {
	Vector<HaxeProject::Dependency> declared;
	Error manifest_error = HaxeProject::load_manifest(declared, r_error);
	if (manifest_error != OK) {
		return manifest_error;
	}

	String haxelib_root = HaxeProject::get_haxelib_dir();
	// Hand-dropped libraries live in the project resource tree; keep the editor
	// importer away from them (no `.uid` files, no import scan).
	HaxeProject::ensure_editor_ignore(haxelib_root);
	Vector<String> pending_names;
	HashMap<String, String> requested_versions;
	for (const HaxeProject::Dependency &dependency : declared) {
		pending_names.push_back(dependency.name);
		requested_versions[dependency.name] = dependency.version;
	}

	// Libraries that were copied into the project haxelib directory by hand
	// (rather than installed through haxelib) are resolved as well, so a
	// project can be set up by unpacking archives. hxcpp is the shared
	// toolchain that provides the cpp generator, not a project library, so it
	// is left to the build pipeline.
	for (const String &name : read_directory(haxelib_root)) {
		if (name == "hxcpp" || requested_versions.has(name)) {
			continue;
		}
		String library_root = haxelib_root.path_join(name);
		bool looks_like_library = FileAccess::exists(library_root.path_join("haxelib.json"));
		if (!looks_like_library) {
			for (const String &version_dir : read_directory(library_root)) {
				if (FileAccess::exists(library_root.path_join(version_dir).path_join("haxelib.json"))) {
					looks_like_library = true;
					break;
				}
			}
		}
		if (!looks_like_library) {
			continue;
		}
		requested_versions[name] = String();
		pending_names.push_back(name);
	}

	HashMap<String, Library> resolved;
	HashMap<String, bool> visiting;

	while (!pending_names.is_empty()) {
		String name = pending_names[pending_names.size() - 1];
		pending_names.remove_at(pending_names.size() - 1);
		if (resolved.has(name) || visiting.has(name)) {
			continue;
		}
		visiting[name] = true;

		String requested = requested_versions.has(name) ? requested_versions[name] : String();
		String library_root = haxelib_root.path_join(name);
		String version = read_library_version(library_root, requested);
		// A library whose directory contains haxelib.json but has no version
		// subdirectory was copied in as-is; treat the directory itself as the
		// library root.
		bool unversioned = false;
		if (version.is_empty() && FileAccess::exists(library_root.path_join("haxelib.json"))) {
			version = "dev";
			unversioned = true;
		}

		Library library;
		library.name = name;
		if (version.is_empty()) {
			if (!HaxeProject::allows_global_haxelib()) {
				r_error = vformat("Haxe library '%s'%s is not installed in the project haxelib directory (%s). Install it with:\n  haxelib install %s%s --local\nor add it to haxe.json and run the Haxelib manager from the editor.",
						name, requested.is_empty() ? String() : vformat(" %s", requested), haxelib_root, name, requested.is_empty() ? String() : " " + requested);
				return ERR_FILE_NOT_FOUND;
			}

			// Explicit opt-in: resolve from the machine wide repository.
			List<String> arguments;
			arguments.push_back("path");
			arguments.push_back(name);
			String output;
			int exit_code = 0;
			if (run_process(HaxeProject::get_haxelib_executable(), arguments, output, exit_code) != OK || exit_code != 0) {
				r_error = vformat("Haxe library '%s' is not installed in the project and the global fallback failed:\n%s", name, output.strip_edges());
				return ERR_FILE_NOT_FOUND;
			}
			String global_path;
			for (const String &line : output.split("\n")) {
				String trimmed = line.strip_edges();
				if (trimmed.is_empty() || trimmed.begins_with("-")) {
					continue;
				}
				if (DirAccess::dir_exists_absolute(trimmed)) {
					global_path = trimmed;
					break;
				}
			}
			if (global_path.is_empty()) {
				r_error = vformat("haxelib path %s did not report a usable directory:\n%s", name, output);
				return ERR_FILE_NOT_FOUND;
			}
			library.global = true;
			library.path = global_path;
			library.class_path = global_path;
			library.version = requested;
			resolved[name] = library;
			continue;
		}

		library.version = version;
		library.path = unversioned ? library_root : library_root.path_join(version);
		library.class_path = library.path;

		String json_path = library.path.path_join("haxelib.json");
		if (FileAccess::exists(json_path)) {
			Ref<FileAccess> file = FileAccess::open(json_path, FileAccess::READ);
			if (file.is_valid()) {
				JSON json;
				if (json.parse(file->get_as_utf8_string()) == OK) {
					Dictionary data = json.get_data();
					if (data.has("classPath")) {
						String class_path = data["classPath"];
						if (!class_path.is_empty()) {
							library.class_path = library.path.path_join(class_path).simplify_path();
						}
					}
					if (data.has("dependencies")) {
						Dictionary dependencies = data["dependencies"];
						for (const KeyValue<Variant, Variant> &entry : dependencies) {
							String dependency_name = entry.key;
							String dependency_version = sanitize_json_version(entry.value);
							if (!requested_versions.has(dependency_name)) {
								requested_versions[dependency_name] = dependency_version;
							} else if (!dependency_version.is_empty() && !requested_versions[dependency_name].is_empty() &&
									requested_versions[dependency_name] != dependency_version) {
								r_error = vformat("Haxe library version conflict for '%s': haxe.json requires %s but '%s' requires %s.",
										dependency_name, requested_versions[dependency_name], name, dependency_version);
								return ERR_INVALID_DATA;
							}
							pending_names.push_back(dependency_name);
						}
					}
				}
			}
		}

		if (!DirAccess::dir_exists_absolute(library.class_path)) {
			r_error = vformat("Haxe library '%s' %s has no class path at %s. Reinstall the library.", name, version, library.class_path);
			return ERR_FILE_CORRUPT;
		}
		resolved[name] = library;
		visiting.erase(name);
	}

	Vector<String> order;
	for (const KeyValue<String, Library> &entry : resolved) {
		order.push_back(entry.key);
	}
	order.sort();
	for (const String &name : order) {
		r_libraries.push_back(resolved[name]);
	}
	return OK;
}

void HaxeBuild::list_installed_libraries(Vector<Library> &r_libraries) {
	r_libraries.clear();
	const String haxelib_root = HaxeProject::get_haxelib_dir();
	// Hand-dropped libraries live in the project resource tree; keep the editor
	// importer away from them (no `.uid` files, no import scan).
	HaxeProject::ensure_editor_ignore(haxelib_root);

	// The list shows what is on disk. The manifest is only consulted to mark
	// which entries are declared, so an installed library never disappears
	// from the panel because haxe.json is incomplete or unreadable.
	Vector<HaxeProject::Dependency> dependencies;
	String manifest_error;
	HaxeProject::load_manifest(dependencies, manifest_error);

	Vector<String> names = read_directory(haxelib_root);
	names.sort();
	for (const String &name : names) {
		const String library_root = haxelib_root.path_join(name);
		String version = read_library_version(library_root, String());
		bool unversioned = false;
		if (version.is_empty() && FileAccess::exists(library_root.path_join("haxelib.json"))) {
			version = "dev";
			unversioned = true;
		}
		if (version.is_empty()) {
			// Neither a version directory nor haxelib.json: not a library.
			continue;
		}

		Library library;
		library.name = name;
		library.version = version;
		library.path = unversioned ? library_root : library_root.path_join(version);
		library.class_path = library.path;
		const String json_path = library.path.path_join("haxelib.json");
		if (FileAccess::exists(json_path)) {
			Ref<FileAccess> file = FileAccess::open(json_path, FileAccess::READ);
			if (file.is_valid()) {
				JSON json;
				if (json.parse(file->get_as_utf8_string()) == OK) {
					Dictionary data = json.get_data();
					if (data.has("classPath")) {
						const String class_path = data["classPath"];
						if (!class_path.is_empty()) {
							library.class_path = library.path.path_join(class_path).simplify_path();
						}
					}
				}
			}
		}
		for (const HaxeProject::Dependency &dependency : dependencies) {
			if (dependency.name == name) {
				library.declared = true;
				break;
			}
		}
		r_libraries.push_back(library);
	}
}

String HaxeBuild::generate_boot_class(const Vector<HaxeClassInfo> &p_classes) {
	String source = "// Generated by the Godot Haxe module. Do not edit.\n";
	source += "// References every project script class so hxcpp keeps them alive.\n\n";
	source += "class __HaxeGodotBoot {\n";
	if (p_classes.is_empty()) {
		source += "\tstatic var __classes:Array<Dynamic> = [];\n";
	} else {
		source += "\tstatic var __classes:Array<Dynamic> = [\n";
		for (const HaxeClassInfo &info : p_classes) {
			// Every project class is referenced, tool classes included: the
			// scene files can attach them in the game as well as the editor.
			if (!info.name.is_empty() && !info.is_abstract) {
				source += "\t\t" + info.fq_name + ",\n";
			}
		}
		source += "\t];\n";
	}
	source += "\n\tstatic function main():Void {}\n}\n";
	return source;
}

String HaxeBuild::build_stamp(bool p_release, bool p_editor, const String &p_abi) {
	Vector<String> lines;
	lines.push_back(vformat("config=%s", p_release ? "release" : "debug"));
	lines.push_back(vformat("target=%s", p_editor ? "editor" : "game"));
	// The same sources produce a different native library per Android ABI, so
	// the target architecture is part of the build identity.
	lines.push_back(vformat("abi=%s", p_abi));
	lines.push_back(vformat("engine=%s", String(VERSION_FULL_NAME)));
	lines.push_back(vformat("project=%s", HaxeProject::get_project_root()));
	// Bumped whenever the module changes which classes or members a library
	// contains, so that projects pick the change up without a manual Rebuild.
	lines.push_back("module=3");

	Vector<String> sources = HaxeProject::scan_sources();
	for (const String &source : sources) {
		lines.push_back(vformat("src=%s:%d", source, (int64_t)FileAccess::get_modified_time(source)));
	}

	Vector<Library> libraries;
	String dependency_error;
	if (resolve_dependencies(libraries, dependency_error) == OK) {
		for (const Library &library : libraries) {
			lines.push_back(vformat("lib=%s:%s:%s", library.name, library.version, library.path));
		}
	} else {
		lines.push_back("lib-error=" + dependency_error);
	}

	for (const String &define : HaxeProject::get_defines()) {
		lines.push_back("define=" + define);
	}
	for (const String &argument : HaxeProject::get_extra_args()) {
		lines.push_back("arg=" + argument);
	}

	// The web library is compiled by whichever `emcc` the toolchain check
	// resolves, and a library built by another Emscripten is not interchangeable
	// with the page (different dylink encoding, shared-memory declaration).
	// Selecting a different installation therefore has to rebuild, or the stale
	// library would be reused and fail at load time.
	if (HaxeProject::is_web_target(p_abi)) {
		String emcc;
		String toolchain_error;
		if (check_web_toolchain(toolchain_error, &emcc)) {
			lines.push_back("emcc=" + web_toolchain_identity(emcc));
		} else {
			lines.push_back("emcc-error=" + toolchain_error);
		}
	}

	// Support code shipped with the engine is part of the build identity.
	String stdlib = HaxeRuntime::get_singleton()->get_stdlib_dir();
	String native_dir = HaxeRuntime::get_singleton()->get_toolchain_dir();
	lines.push_back("stdlib=" + stdlib);
	lines.push_back("native=" + native_dir);
	lines.push_back("debug_symbols=" + String(HaxeProject::debug_symbols_enabled(p_release) ? "1" : "0"));

	String joined;
	for (const String &line : lines) {
		joined += line + "\n";
	}
	return joined.md5_text() + vformat(":%d", joined.length());
}

String HaxeBuild::current_stamp(bool p_release, bool p_editor, const String &p_abi) {
	return build_stamp(p_release, p_editor, p_abi);
}

bool HaxeBuild::is_build_required(bool p_release, bool p_editor, const String &p_abi) {
	String stamp_path = HaxeProject::get_config_dir(p_release, p_editor, p_abi).path_join(".stamp");
	if (!FileAccess::exists(stamp_path)) {
		return true;
	}
	Ref<FileAccess> file = FileAccess::open(stamp_path, FileAccess::READ);
	if (file.is_null()) {
		return true;
	}
	String stamp = file->get_as_utf8_string().strip_edges();
	String expected = build_stamp(p_release, p_editor, p_abi);
	return stamp != expected || !FileAccess::exists(HaxeProject::get_versioned_library_path(p_release, p_editor, expected, p_abi));
}

/* The hxcpp define that selects the target architecture for an Android ABI
 * (`godot.Node2D` style names come from Godot's exporter). Empty for an
 * unknown ABI. */
static String android_arch_define(const String &p_abi) {
	if (p_abi == "armeabi-v7a") {
		return "HXCPP_ARMV7";
	}
	if (p_abi == "arm64-v8a") {
		return "HXCPP_ARM64";
	}
	if (p_abi == "x86") {
		return "HXCPP_X86";
	}
	if (p_abi == "x86_64") {
		return "HXCPP_X86_64";
	}
	return String();
}

/* The lowest Android API the library is linked against. Godot's own minimum
 * is 24, and NDK releases ship a sysroot for every platform from 21 up, so
 * linking against 24 keeps the library loadable on every device the exported
 * game supports. */
static const char *const ANDROID_API_PLATFORM = "android-24";

Vector<String> HaxeBuild::android_compiler_defines(const String &p_abi, const String &p_ndk_root) {
	Vector<String> arguments;
	const String arch_define = android_arch_define(p_abi);
	if (arch_define.is_empty()) {
		return arguments;
	}
	// hxcpp selects its Android toolchain from `android` and the target
	// architecture from one `HXCPP_*` define. The platform is fixed so the
	// link works on NDK releases that no longer ship a `platforms/` directory
	// for hxcpp to scan.
	arguments.push_back("-D");
	arguments.push_back("android");
	arguments.push_back("-D");
	arguments.push_back(arch_define);
	arguments.push_back("-D");
	arguments.push_back(String("PLATFORM=") + ANDROID_API_PLATFORM);
	if (!p_ndk_root.is_empty()) {
		arguments.push_back("-D");
		arguments.push_back("ANDROID_NDK_ROOT=" + p_ndk_root);
	}
	return arguments;
}

/* Looks for an executable on the editor's PATH. Used for the external tools of
 * the web target: `emcc` lives in the emsdk installation, which is on PATH
 * whenever `emsdk_env.sh` has been sourced. */
static String find_in_path(const String &p_name, const String &p_path) {
#ifdef WINDOWS_ENABLED
	const Vector<String> extensions = { "", ".exe", ".bat", ".cmd" };
#else
	const Vector<String> extensions = { "" };
#endif
	for (const String &element : p_path.split(HaxeBuild::path_list_separator(), false)) {
		if (element.is_empty()) {
			continue;
		}
		for (const String &extension : extensions) {
			const String candidate = element.path_join(p_name + extension);
			if (FileAccess::exists(candidate)) {
				return candidate;
			}
		}
	}
	return String();
}

/* `emcc` inside an Emscripten directory, or an empty string.
 *
 * An emsdk installation keeps it below `upstream/emscripten`; a manual
 * Emscripten build has it next to `emcc.py`. Windows uses `emcc.bat`. */
static String emcc_in_directory(const String &p_directory) {
	for (const char *const name : { "emcc", "emcc.py", "emcc.bat", "emcc.cmd" }) {
		const String candidate = p_directory.path_join(name);
		if (FileAccess::exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

/* `<emsdk>/upstream/emscripten/emcc` -> `<emsdk>`, or an empty string when the
 * executable does not look like part of an emsdk installation.
 *
 * hxcpp reads `EMSDK` to find `emar`, `emranlib`, `node` and to prepend that
 * installation to its own search path, so passing the root along keeps the
 * compiler, the archiver and the template on one Emscripten version. */
static String emsdk_root_for(const String &p_emcc) {
	const String directory = p_emcc.get_base_dir();
	if (directory.get_file() != "emscripten" || directory.get_base_dir().get_file() != "upstream") {
		return String();
	}
	const String root = directory.get_base_dir().get_base_dir();
	for (const char *const marker : { "emsdk_env.sh", "emsdk_env.bat", "emsdk", "emsdk.py", ".emscripten" }) {
		if (FileAccess::exists(root.path_join(marker))) {
			return root;
		}
	}
	return String();
}

#ifdef TOOLS_ENABLED
static String get_web_emcc_setting() {
	if (EditorSettings::get_singleton() == nullptr) {
		return String();
	}
	return String(EDITOR_GET(HaxeBuild::SETTING_WEB_EMCC)).strip_edges();
}
#else
static String get_web_emcc_setting() {
	// Only the editor builds and exports projects, so a template build never
	// reads the setting; it keeps the EMSDK/PATH fallbacks.
	return String();
}
#endif

/* The hxcpp build configuration for the web target.
 *
 * hxcpp's Emscripten toolchain links the library with `-shared`, which emcc
 * turns into a *relocatable object* (its compatibility mode) unless
 * `-sSIDE_MODULE` is set. The Godot web export loads the library with dlopen
 * from a runtime module built with `-sMAIN_MODULE=1`, so it has to be a real
 * side module: it imports the host's libc and engine symbols, shares its linear
 * memory, and exports its own C ABI for the dlsym lookups the module performs.
 *
 * hxcpp has no option for extra linker flags, but it includes `$HXCPP_CONFIG`
 * in its `exes` section and *appends* a repeated `<linker>` element to the
 * toolchain's own linker instead of replacing it, so this file adds the flags
 * without patching hxcpp.
 *
 * When the export preset enables thread support, the page's engine module is
 * linked with `-pthread` and its linear memory is shared. A wasm module declares
 * that property at link time, and the browser refuses to instantiate a module
 * whose declaration disagrees with the memory it is given ("mismatch in shared
 * state of memory"), so the threads variant has to be linked with `-pthread`
 * too. The library itself stays single-threaded: it never creates threads, it
 * only declares that it shares the memory.
 *
 * A debug build needs one more fix: see the `vars` section below. */
static String web_hxcpp_config_source(bool p_threads, bool p_release) {
	String source;
	source += "<xml>\n";
	if (!p_release) {
		// hxcpp's Emscripten toolchain links a debug build through
		// `-Xlinker -g` (it reuses the compiler's optimization variable for a
		// wasm-ld option). wasm-ld has no `-g`, so the link stops with
		// `unknown argument: -g`. Fix the variable from the `vars` section,
		// which hxcpp parses before the toolchain, and let the `-g` flag below
		// reach emcc itself, where it means "keep debug information".
		source += "  <section id=\"vars\">\n";
		source += "    <set name=\"HXCPP_LINK_OPTIM_LEVEL\" value=\"-O1\" />\n";
		source += "  </section>\n";
	}
	source += "  <section id=\"exes\">\n";
	if (p_threads) {
		// `-pthread` also selects the atomics/bulk-memory features at compile
		// time; wasm-ld rejects objects compiled without them when the output
		// declares shared memory.
		source += "    <compiler id=\"emscripten\">\n";
		source += "      <flag value=\"-pthread\" />\n";
		source += "    </compiler>\n";
	}
	source += "    <linker id=\"dll\">\n";
	source += "      <flag value=\"-sSIDE_MODULE=2\" />\n";
	source += "      <flag value=\"-Wl,--export-dynamic\" />\n";
	if (!p_release) {
		source += "      <flag value=\"-g\" />\n";
	}
	// Emscripten does *not* export every visible symbol of a `-sSIDE_MODULE=2`
	// module: that mode turns off `--export-dynamic` internally so the linker
	// can drop unused code, and a symbol is only reachable from the host when
	// it is named explicitly. The engine resolves the library's entry points by
	// name from the page's global symbol namespace, so the C ABI functions have
	// to be listed here or the exported game reports that the library does not
	// export `hxgodot_install`.
	String exports;
	for (const char *const *name = HXGODOT_WEB_EXPORTS; *name != nullptr; name++) {
		exports += (exports.is_empty() ? "" : ",");
		exports += "_";
		exports += *name;
	}
	source += "      <flag value=\"-sEXPORTED_FUNCTIONS=" + exports + "\" />\n";
	if (p_threads) {
		source += "      <flag value=\"-pthread\" />\n";
	}
	source += "    </linker>\n";
	source += "  </section>\n";
	source += "</xml>\n";
	return source;
}

String HaxeBuild::write_web_hxcpp_config(bool p_release, bool p_editor, const String &p_abi) {
	const String path = HaxeProject::get_web_hxcpp_config_path(p_release, p_editor, p_abi);
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	if (!write_if_changed(path, web_hxcpp_config_source(HaxeProject::is_web_threads_target(p_abi), p_release))) {
		return String();
	}
	return path;
}

Vector<String> HaxeBuild::web_compiler_defines(const String &p_hxcpp_config_path) {
	Vector<String> arguments;
	// hxcpp selects its Emscripten toolchain from `emscripten`.
	arguments.push_back("-D");
	arguments.push_back("emscripten");
	// Position independent code: the side module is loaded at its own base
	// address inside the host module's memory.
	arguments.push_back("-D");
	arguments.push_back("HXCPP_EMSCRIPTEN_FPIC");
	// The library references symbols it does not define (the engine's C ABI
	// hooks and the host libc); leaving them undefined is what makes the module
	// an import table instead of a link error.
	arguments.push_back("-D");
	arguments.push_back("HXCPP_ALLOW_UNDEFINED");
	if (!p_hxcpp_config_path.is_empty()) {
		arguments.push_back("-D");
		arguments.push_back("HXCPP_CONFIG=" + p_hxcpp_config_path);
	}
	return arguments;
}

bool HaxeBuild::resolve_web_emcc(const String &p_setting, const String &p_emsdk, const String &p_path, String &r_emcc, String &r_error) {
	r_emcc = String();
	r_error = String();

	// An explicit setting always wins: it is the only way to select a version
	// when `EMSDK` points at another one.
	const String setting = p_setting.strip_edges();
	if (!setting.is_empty()) {
		// `~` is not expanded by `simplify_path()`, and it is the obvious way
		// to type an emsdk below the home directory by hand.
		String selected = setting;
		if (selected == "~" || selected.begins_with("~/")) {
			const String home = OS::get_singleton()->get_environment("HOME").strip_edges();
			if (!home.is_empty()) {
				selected = home.path_join(selected.substr(2));
			}
		}
		selected = selected.simplify_path();
		if (FileAccess::exists(selected) && !DirAccess::dir_exists_absolute(selected)) {
			r_emcc = selected;
			return true;
		}
		// A directory is accepted as the emsdk root, as its Emscripten
		// directory, or as any directory that holds `emcc` itself.
		for (const String &directory : { selected, selected.path_join("upstream/emscripten") }) {
			if (!DirAccess::dir_exists_absolute(directory)) {
				continue;
			}
			const String found = emcc_in_directory(directory);
			if (!found.is_empty()) {
				r_emcc = found;
				return true;
			}
		}
		r_error = vformat("The editor setting '%s' is '%s', but no 'emcc' executable was found there. Point it at the executable, at '<emsdk>/upstream/emscripten' or at the emsdk directory, or clear it to use EMSDK and PATH.", SETTING_WEB_EMCC, setting);
		return false;
	}

	// hxcpp locates Emscripten through EMSDK, so honor it before PATH: an
	// editor launched from a desktop icon has no `emcc` on PATH even when
	// `EMSDK` is exported.
	const String emsdk = p_emsdk.strip_edges();
	if (!emsdk.is_empty()) {
		const String found = emcc_in_directory(emsdk.path_join("upstream/emscripten"));
		if (!found.is_empty()) {
			r_emcc = found;
			return true;
		}
	}

	const String on_path = find_in_path("emcc", p_path);
	if (!on_path.is_empty()) {
		r_emcc = on_path;
		return true;
	}

	String hint;
	if (!emsdk.is_empty()) {
		// The user did export EMSDK, but that directory has no emcc.
		hint = vformat(" EMSDK is set to '%s', which does not contain 'upstream/emscripten/emcc'.", emsdk);
	} else {
		// "It works in my terminal" is the usual report: the editor inherited
		// the environment of whatever launched it.
		hint = " The editor only sees the PATH it was started with, so 'emcc --version' working in a terminal only helps if Godot was started from that same shell; otherwise set the 'haxe/web/emcc_path' editor setting.";
	}
	r_error = vformat("The web target needs Emscripten, but 'emcc' was not found (no '%s' editor setting, no EMSDK and no 'emcc' on PATH).%s Install emsdk and activate 4.0.11, the version verified with this module ('emsdk install 4.0.11 && emsdk activate 4.0.11'; 4.0.0 breaks dynamic linking and 4.0.23 breaks the threaded build).", SETTING_WEB_EMCC, hint);
	return false;
}

bool HaxeBuild::check_web_toolchain(String &r_error, String *r_emcc) {
	const OS *os = OS::get_singleton();
	String emcc;
	if (!resolve_web_emcc(get_web_emcc_setting(), os->get_environment("EMSDK"), os->get_environment("PATH"), emcc, r_error)) {
		if (r_emcc != nullptr) {
			r_emcc->clear();
		}
		return false;
	}
	r_error = String();
	if (r_emcc != nullptr) {
		*r_emcc = emcc;
	}
	return true;
}

String HaxeBuild::web_toolchain_identity(const String &p_emcc) {
	// The path matters because two installations can ship the same version,
	// and the modification time catches an in-place update that keeps the
	// path. Spawning `emcc --version` for the stamp would slow every build
	// check down and needs the toolchain to be runnable; a plain stat does
	// not.
	return vformat("%s:%d", p_emcc, (int64_t)FileAccess::get_modified_time(p_emcc));
}

String HaxeBuild::path_list_separator() {
#ifdef WINDOWS_ENABLED
	return ";";
#else
	return ":";
#endif
}

Vector<Pair<String, String>> HaxeBuild::web_toolchain_environment(const String &p_emcc) {
	Vector<Pair<String, String>> environment;
	const String directory = p_emcc.get_base_dir();
	if (directory.is_empty()) {
		return environment;
	}
	// hxcpp runs `emcc`, `emar` and `emranlib` from its own PATH, so the
	// directory of the selected executable has to come first. This is what
	// sourcing emsdk_env.sh does for an interactive shell.
	environment.push_back(Pair<String, String>("PATH", directory + path_list_separator() + OS::get_singleton()->get_environment("PATH")));
	const String emsdk_root = emsdk_root_for(p_emcc);
	if (!emsdk_root.is_empty()) {
		// hxcpp prepends `$EMSDK` itself, so an inherited value pointing at a
		// different version would win over the one selected above.
		environment.push_back(Pair<String, String>("EMSDK", emsdk_root));
	}
	return environment;
}

#ifdef TOOLS_ENABLED
void HaxeBuild::register_web_editor_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	ERR_FAIL_NULL(settings);
	// Editor state, not project state: the Emscripten installation belongs to
	// the machine (like Android's SDK path), and the editor is the process that
	// runs the web build. `EDITOR_DEF` keeps the value out of the settings file
	// until the user changes it.
	EDITOR_DEF(SETTING_WEB_EMCC, "");
	// No name filter: an `emcc` launcher is usually called `emcc`, but emsdk
	// also ships `emcc.py`/`emcc.bat`, and a manual install can name it
	// anything. Godot's other executable settings leave the filter empty too.
	settings->add_property_hint(PropertyInfo(Variant::STRING, SETTING_WEB_EMCC, PROPERTY_HINT_GLOBAL_FILE, ""));
}
#endif

Vector<String> HaxeBuild::collect_arguments(bool p_release, bool p_editor, const Vector<Library> &p_libraries, const Vector<HaxeClassInfo> &p_script_classes, Vector<Pair<String, String>> &r_class_path_aliases, const String &p_abi, const String &p_ndk_root) {
	ClassPathAlias alias;
	Vector<String> arguments;
	arguments.push_back("-cp");
	arguments.push_back(alias.map(HaxeProject::get_source_dir()));
	String stdlib = HaxeRuntime::get_singleton()->get_stdlib_dir();
	if (stdlib.is_empty()) {
		return arguments;
	}
	arguments.push_back("-cp");
	arguments.push_back(alias.map(stdlib));
	arguments.push_back("-cp");
	arguments.push_back(alias.map(HaxeProject::get_generated_dir(p_release, p_editor, p_abi)));

	for (const Library &library : p_libraries) {
		arguments.push_back("-cp");
		arguments.push_back(alias.map(library.class_path));
		arguments.push_back("-D");
		arguments.push_back(vformat("%s=%s", library.name, library.version.is_empty() ? String("dev") : library.version));

		// extraParams.hxml is the standard haxelib way to add compiler
		// options. It uses the hxml syntax: whitespace separated tokens with
		// quotes grouping a token and `#` starting a comment. Library paths
		// and the entry point are resolved by this module, so `-lib`, `-main`
		// and `-cp` (and their values) are dropped.
		String extra = library.path.path_join("extraParams.hxml");
		if (FileAccess::exists(extra)) {
			Ref<FileAccess> file = FileAccess::open(extra, FileAccess::READ);
			if (file.is_valid()) {
				for (const String &token : parse_extra_params(file->get_as_utf8_string())) {
					arguments.push_back(token);
				}
			}
		}
	}

	arguments.push_back("-main");
	arguments.push_back("__HaxeGodotBoot");
	arguments.push_back("-cpp");
	arguments.push_back(HaxeProject::get_config_dir(p_release, p_editor, p_abi).path_join("cpp"));
	arguments.push_back("-D");
	arguments.push_back("dll_link");
	arguments.push_back("-D");
	arguments.push_back("dll_export");
	if (HaxeProject::is_web_target(p_abi)) {
		for (const String &argument : web_compiler_defines(HaxeProject::get_web_hxcpp_config_path(p_release, p_editor, p_abi))) {
			arguments.push_back(argument);
		}
	} else if (!p_abi.is_empty()) {
		for (const String &argument : android_compiler_defines(p_abi, p_ndk_root)) {
			arguments.push_back(argument);
		}
	}
	// hxcpp names the linked file after the entry point (and appends a debug
	// suffix). Pin the name to the one this module loads, so the layout stays
	// independent of hxcpp's naming rules. On Android hxcpp adds its own
	// `lib` prefix to this name.
#ifdef WINDOWS_ENABLED
	// The library is a DLL loaded into the engine process. MinGW's C++ runtime
	// (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) is not
	// part of Windows, so it is linked statically; otherwise the library only
	// loads on a machine that happens to have the same toolchain on PATH.
	arguments.push_back("-D");
	arguments.push_back("no_shared_libs");
#endif
	arguments.push_back("-D");
	arguments.push_back("HAXE_OUTPUT_FILE=" + HaxeProject::get_library_base_name());
	arguments.push_back("-D");
	arguments.push_back("hxgodot_native=" + HaxeRuntime::get_singleton()->get_toolchain_dir());
	arguments.push_back("-D");
	arguments.push_back(p_release ? "hxgodot_release" : "hxgodot_debug");
	if (p_editor) {
		arguments.push_back("-D");
		arguments.push_back("hxgodot_editor");
	}
	if (!p_release && HaxeProject::debug_symbols_enabled(p_release)) {
		arguments.push_back("-debug");
	} else {
		arguments.push_back("-D");
		arguments.push_back("analyzer-optimize");
	}

	// Script members are reached by name at runtime, so dead code elimination
	// must not consider them unused.
	for (const String &package : keep_packages(p_script_classes)) {
		arguments.push_back("--macro");
		arguments.push_back(vformat("addGlobalMetadata('%s', '@:keep', true, true, true)", package));
	}
	arguments.push_back("-dce");
	arguments.push_back("full");

	for (const String &define : HaxeProject::get_defines()) {
		arguments.push_back("-D");
		arguments.push_back(define);
	}
	for (const String &argument : HaxeBuild::normalize_extra_arguments(HaxeProject::get_extra_args())) {
		arguments.push_back(argument);
	}
	alias.append_mappings(r_class_path_aliases);
	return arguments;
}

String HaxeBuild::get_compile_haxelib_path() {
	return compile_haxelib_path();
}

Vector<String> HaxeBuild::collect_display_arguments() {
	Vector<String> arguments;
	// The classpath used for display requests; kept in sync with the build so
	// in-editor analysis and the compiler agree on the source set.
	if (HaxeRuntime::get_singleton()->get_stdlib_dir().is_empty()) {
		return arguments;
	}
	arguments.push_back("-cp");
	arguments.push_back(HaxeProject::get_source_dir());
	arguments.push_back("-cp");
	arguments.push_back(HaxeRuntime::get_singleton()->get_stdlib_dir());

	Vector<Library> libraries;
	String error;
	if (resolve_dependencies(libraries, error) == OK) {
		for (const Library &library : libraries) {
			arguments.push_back("-cp");
			arguments.push_back(library.class_path);
			arguments.push_back("-D");
			arguments.push_back(vformat("%s=%s", library.name, library.version.is_empty() ? String("dev") : library.version));
		}
	}
	// Generated editor sources (the entry point) are not needed for type
	// checking, but depending on the editor configuration keeps the class
	// path identical to the one the build uses.
	for (const String &define : HaxeProject::get_defines()) {
		arguments.push_back("-D");
		arguments.push_back(define);
	}
	return arguments;
}

void HaxeBuild::parse_diagnostics(const String &p_output, const Vector<Pair<String, String>> &p_class_path_aliases, Result &r_result) {
	for (const String &raw_line : p_output.split("\n")) {
		String line = raw_line.strip_edges();
		if (line.is_empty()) {
			continue;
		}
		// Haxe diagnostics look like:
		//   /path/File.hx:12: characters 4-10 : Unknown identifier : foo
		int first_colon = line.find(":");
		if (first_colon <= 0) {
			continue;
		}
		String path = translate_class_path(line.substr(0, first_colon), p_class_path_aliases);
		if (!path.ends_with(".hx") && !path.ends_with(".hxml")) {
			continue;
		}
		String rest = line.substr(first_colon + 1);
		int second_colon = rest.find(":");
		if (second_colon <= 0) {
			continue;
		}
		ScriptLanguage::ScriptError error;
		error.path = path;
		error.line = rest.substr(0, second_colon).strip_edges().to_int();
		String remainder = rest.substr(second_colon + 1);
		int characters = remainder.find("characters");
		if (characters >= 0) {
			String range = remainder.substr(characters + 10);
			int colon = range.find(":");
			if (colon >= 0) {
				String columns = range.substr(0, colon).strip_edges();
				// "4-10" -> column 5 (one based).
				String start = columns.get_slice("-", 0).strip_edges();
				error.column = start.to_int() + 1;
				remainder = range.substr(colon + 1);
			}
		}
		error.message = remainder.strip_edges();
		r_result.errors.push_back(error);
	}
}

HaxeBuild::Result HaxeBuild::build(bool p_release, bool p_editor, bool p_force, const String &p_abi, const String &p_ndk_root) {
	HaxeBuildSession session;
	session.start(p_release, p_editor, p_force, p_abi, p_ndk_root);
	while (session.is_running()) {
		session.poll();
		// The compiler runs in its own process; sleeping here only saves CPU.
		OS::get_singleton()->delay_usec(2000);
	}
	return session.get_result();
}

HaxeBuildSession::~HaxeBuildSession() {
	if (running) {
		process.kill();
	}
}

void HaxeBuildSession::start(bool p_release, bool p_editor, bool p_force, const String &p_abi, const String &p_ndk_root) {
	abi = p_abi;
	ndk_root = p_ndk_root;
	prepare(p_release, p_editor, p_force);
}

void HaxeBuildSession::prepare(bool p_release, bool p_editor, bool p_force) {
	result = HaxeBuild::Result();

	if (!abi.is_empty() && !HaxeProject::is_web_target(abi) && android_arch_define(abi).is_empty()) {
		result = HaxeBuild::Result::failed(vformat("Unknown target ABI '%s'. Supported targets are armeabi-v7a, arm64-v8a, x86, x86_64, %s and %s.", abi, HaxeProject::WEB_TARGET, HaxeProject::WEB_THREADS_TARGET));
		finished = true;
		return;
	}

	if (HaxeRuntime::get_singleton()->get_stdlib_dir().is_empty()) {
		result = HaxeBuild::Result::failed("Haxe support files are missing from this Godot build. The generated bindings, runtime classes and toolchain glue are embedded in the engine; rebuild it with the Haxe module to restore them.");
		finished = true;
		return;
	}
	if (HaxeProject::get_source_dir().is_empty()) {
		result = HaxeBuild::Result::failed("The Haxe source directory is not configured.");
		finished = true;
		return;
	}

	/* Set when the web library has to be produced again because another
	 * Emscripten built the previous one. */
	bool toolchain_changed = false;

	Vector<HaxeBuild::Library> libraries;
	String dependency_error;
	if (HaxeBuild::resolve_dependencies(libraries, dependency_error) != OK) {
		result.message = dependency_error;
		result.raw_output = dependency_error;
		last_build_log = dependency_error;
		finished = true;
		return;
	}

	if (HaxeProject::is_web_target(abi)) {
		String toolchain_error;
		String emcc;
		if (!HaxeBuild::check_web_toolchain(toolchain_error, &emcc)) {
			result = HaxeBuild::Result::failed(toolchain_error);
			last_build_log = toolchain_error;
			finished = true;
			return;
		}
		// Point hxcpp at the same `emcc` the check accepted: the editor's own
		// PATH may not contain it (a desktop launch does not source
		// emsdk_env.sh), and an inherited EMSDK may name another version.
		web_emcc = emcc;
		web_toolchain = HaxeBuild::web_toolchain_identity(emcc);
		// hxcpp decides for itself whether to relink from the generated
		// sources and the objects in its working directory; the compiler is
		// not part of that comparison. A library produced by another
		// Emscripten would therefore be copied as if it were new, and the page
		// then rejects it (different dynamic-linking encoding, different
		// shared-memory declaration). Discard the working directory when the
		// toolchain recorded by the last successful build differs, and never
		// take the up-to-date shortcut in that case: the stamp alone does not
		// prove which compiler produced the library.
		const String record_path = HaxeProject::get_config_dir(p_release, p_editor, abi).path_join(".web-toolchain");
		String recorded;
		if (FileAccess::exists(record_path)) {
			Ref<FileAccess> record = FileAccess::open(record_path, FileAccess::READ);
			if (record.is_valid()) {
				recorded = record->get_as_utf8_string().strip_edges();
			}
		}
		if (recorded != web_toolchain) {
			toolchain_changed = true;
			const String work_dir = HaxeProject::get_config_dir(p_release, p_editor, abi).path_join("cpp");
			if (DirAccess::dir_exists_absolute(work_dir)) {
				print_line(vformat("The Web toolchain changed to '%s'; discarding the objects of the previous build.", web_emcc));
				HaxeProject::remove_directory(work_dir);
			}
		}
		child_environment.append_array(HaxeBuild::web_toolchain_environment(emcc));
		if (HaxeBuild::write_web_hxcpp_config(p_release, p_editor, abi).is_empty()) {
			const String message = vformat("Cannot write the hxcpp build configuration into '%s'.", HaxeProject::get_web_hxcpp_config_path(p_release, p_editor, abi));
			result = HaxeBuild::Result::failed(message);
			last_build_log = message;
			finished = true;
			return;
		}
		// hxcpp only reads `HXCPP_CONFIG` from the environment (`Setup` fills in
		// `$HOME/.hxcpp_config.xml` when the variable is missing) and then
		// includes the file named by the define of the same name.
		child_environment.push_back(Pair<String, String>("HXCPP_CONFIG", HaxeProject::get_web_hxcpp_config_path(p_release, p_editor, abi)));
	}

	Vector<HaxeClassInfo> classes = HaxeProject::get_all_classes();
	// Only classes that actually extend a Godot type can be script classes.
	Vector<HaxeClassInfo> script_classes;
	for (const HaxeClassInfo &info : classes) {
		if (info.valid && !info.base.is_empty() && !info.is_abstract) {
			script_classes.push_back(info);
		}
	}

	String boot_source = HaxeBuild::generate_boot_class(script_classes);
	String generated_dir = HaxeProject::get_generated_dir(p_release, p_editor, abi);
	DirAccess::make_dir_recursive_absolute(generated_dir);
	if (!write_if_changed(generated_dir.path_join("__HaxeGodotBoot.hx"), boot_source)) {
		result = HaxeBuild::Result::failed(vformat("Cannot write generated entry point into %s.", generated_dir));
		finished = true;
		return;
	}

	output_path = HaxeProject::get_output_library_path(p_release, p_editor, abi);
	stamp_path = HaxeProject::get_config_dir(p_release, p_editor, abi).path_join(".stamp");
	stamp = HaxeBuild::build_stamp(p_release, p_editor, abi);
	versioned_path = HaxeProject::get_versioned_library_path(p_release, p_editor, stamp, abi);
	if (!p_force && !toolchain_changed && FileAccess::exists(output_path) && FileAccess::exists(versioned_path) && FileAccess::exists(stamp_path)) {
		Ref<FileAccess> file = FileAccess::open(stamp_path, FileAccess::READ);
		if (file.is_valid() && file->get_as_utf8_string().strip_edges() == stamp) {
			result.success = true;
			result.up_to_date = true;
			result.library_path = versioned_path;
			result.message = "Haxe build is up to date.";
			finished = true;
			return;
		}
	}

	Vector<String> arguments_vector = HaxeBuild::collect_arguments(p_release, p_editor, libraries, script_classes, class_path_aliases, abi, ndk_root);
	if (arguments_vector.is_empty()) {
		result = HaxeBuild::Result::failed("Haxe support files are missing; cannot build.");
		finished = true;
		return;
	}

	List<String> arguments;
	for (const String &argument : arguments_vector) {
		arguments.push_back(argument);
	}

	DirAccess::make_dir_recursive_absolute(HaxeProject::get_config_dir(p_release, p_editor, abi));

	print_line(vformat("Building Haxe project (%s%s)%s...", p_release ? "release" : "debug",
			abi.is_empty() ? String() : (HaxeProject::is_web_target(abi) ? String(" for Web") : " for Android " + abi),
			p_editor ? " for the editor" : ""));

	String executable;
	List<String> command;
	resolve_haxe_command(arguments, executable, command);
	Error error = process.start(executable, command, HaxeBuild::get_compile_haxelib_path(), child_environment);
	if (error != OK) {
		result.message = vformat("Could not run the Haxe compiler '%s'. Install Haxe and make sure it is on PATH, or set %s.",
				HaxeProject::get_haxe_executable(), HaxeProject::SETTING_HAXE_EXECUTABLE);
		result.raw_output = result.message;
		last_build_log = result.message;
		finished = true;
		return;
	}
	running = true;
}

bool HaxeBuildSession::poll() {
	if (!running) {
		return false;
	}
	if (process.poll()) {
		return true;
	}
	running = false;
	finish();
	return false;
}

void HaxeBuildSession::record_web_toolchain() {
	if (web_toolchain.is_empty() || stamp_path.is_empty()) {
		return;
	}
	// Written only after a library was produced (or when the stamp proves the
	// library came from this toolchain), so a failed build leaves the previous
	// record in place and the working directory is still discarded next time.
	Ref<FileAccess> record = FileAccess::open(stamp_path.get_base_dir().path_join(".web-toolchain"), FileAccess::WRITE);
	if (record.is_valid()) {
		record->store_string(web_toolchain + "\n");
	}
}

void HaxeBuildSession::finish() {
	finished = true;
	String process_output = process.get_output();
	int exit_code = process.get_exit_code();
	last_build_log = process_output;
	result.raw_output = process_output;
	result.exit_code = exit_code;
	HaxeBuild::parse_diagnostics(process_output, class_path_aliases, result);

	if (exit_code != 0) {
		if (result.errors.is_empty()) {
			result.message = vformat("Haxe compilation failed with exit code %d.\n%s", exit_code, notable_tail(process_output));
		} else {
			result.message = vformat("Haxe compilation failed with %d error(s).", result.errors.size());
		}
		return;
	}
	if (!FileAccess::exists(output_path)) {
		result.message = vformat("Haxe reported success but no library was produced at %s.\n%s", output_path, notable_tail(process_output));
		return;
	}

	// Loaded shared libraries can never be replaced in place. Every build is
	// copied to a stamp-versioned file so a rebuild produces a new library
	// that new instances pick up while existing instances keep running the
	// code they were created with.
	DirAccess::make_dir_recursive_absolute(versioned_path.get_base_dir());
	{
		Ref<FileAccess> source_file = FileAccess::open(output_path, FileAccess::READ);
		if (source_file.is_null()) {
			result.message = vformat("Cannot read the produced Haxe library at %s.", output_path);
			return;
		}
		Vector<uint8_t> contents = source_file->get_buffer(source_file->get_length());
		Ref<FileAccess> target_file = FileAccess::open(versioned_path, FileAccess::WRITE);
		if (target_file.is_null()) {
			result.message = vformat("Cannot write the versioned Haxe library to %s.", versioned_path);
			return;
		}
		target_file->store_buffer(contents);
	}

	Ref<FileAccess> stamp_file = FileAccess::open(stamp_path, FileAccess::WRITE);
	if (stamp_file.is_valid()) {
		stamp_file->store_string(stamp + "\n");
	}
	record_web_toolchain();

	// Drop outputs nothing loads any more. Every compile writes a new
	// versioned library, so without this a project accumulates a copy of the
	// whole Haxe build per compile.
	String keep_support = HaxeRuntime::get_singleton()->get_stdlib_dir();
	if (!keep_support.is_empty()) {
		keep_support = keep_support.get_base_dir();
	}
	HaxeProject::prune_caches(keep_support);

	result.success = true;
	result.library_path = versioned_path;
	result.message = vformat("Haxe build succeeded: %s", versioned_path);
}

void HaxeBuildSession::cancel() {
	if (running) {
		process.kill();
		running = false;
	}
	if (!finished) {
		result = HaxeBuild::Result::failed("Haxe build was cancelled.");
		finished = true;
	}
}

String HaxeBuild::format_errors(const Result &p_result) {
	String text;
	for (const ScriptLanguage::ScriptError &error : p_result.errors) {
		String where = error.path;
		if (error.line > 0) {
			where += vformat(":%d", error.line);
			if (error.column > 0) {
				where += vformat(":%d", error.column);
			}
		}
		if (!where.is_empty()) {
			text += where + ": ";
		}
		text += error.message.strip_edges() + "\n";
	}
	return text.strip_edges();
}

String HaxeBuild::check_toolchain() {
	Vector<String> missing;

	List<String> arguments;
	arguments.push_back("--version");
	String output;
	int exit_code = 0;
	Error error = run_haxe(arguments, output, exit_code);
	if (error != OK || exit_code != 0) {
		missing.push_back(vformat("Haxe compiler ('%s --version' failed). Install Haxe 4.3 or newer and put it on PATH, or set '%s'.",
				HaxeProject::get_haxe_executable(), HaxeProject::SETTING_HAXE_EXECUTABLE));
	}

	String haxelib_root = HaxeProject::get_haxelib_dir();
	bool local_hxcpp = DirAccess::dir_exists_absolute(haxelib_root.path_join("hxcpp"));
	if (!local_hxcpp) {
		// hxcpp is the code generator/toolchain used by the cpp target. It is
		// allowed to live in the machine wide haxelib repository, like a
		// compiler, but a project local copy always takes precedence.
		String global;
		int hxcpp_exit = 0;
		if (run_process(HaxeProject::get_haxelib_executable(), List<String>({ "path", "hxcpp" }), global, hxcpp_exit) != OK || hxcpp_exit != 0) {
			missing.push_back("hxcpp is not installed. Install it with 'haxelib install hxcpp' or place it in the project haxelib directory.");
		}
	}

	return String("\n").join(missing);
}

Error HaxeBuild::prepare_haxelib_repository(String &r_output) {
	return ensure_haxelib_repository(r_output) ? OK : FAILED;
}

List<String> HaxeBuild::install_arguments(const String &p_name, const String &p_version, bool p_skip_dependencies) {
	List<String> arguments;
	arguments.push_back("install");
	arguments.push_back(p_name);
	if (!p_version.is_empty()) {
		arguments.push_back(p_version);
	}
	if (p_skip_dependencies) {
		arguments.push_back("--skip-dependencies");
	}
	arguments.push_back("--quiet");
	arguments.push_back("--always");
	return arguments;
}

List<String> HaxeBuild::update_arguments(const String &p_name) {
	List<String> arguments;
	arguments.push_back("update");
	if (!p_name.is_empty()) {
		arguments.push_back(p_name);
	}
	arguments.push_back("--quiet");
	arguments.push_back("--always");
	return arguments;
}

List<String> HaxeBuild::remove_arguments(const String &p_name) {
	List<String> arguments;
	arguments.push_back("remove");
	arguments.push_back(p_name);
	arguments.push_back("--quiet");
	arguments.push_back("--always");
	return arguments;
}

/* Runs a haxelib command to completion. The library manager runs the same
 * argument lists through `HaxeProcess` instead, so it can show progress. */
static Error run_haxelib_blocking(const List<String> &p_arguments, String &r_output, int &r_exit_code) {
	HaxeProcess process;
	Error error = process.start(HaxeProject::get_haxelib_executable(), p_arguments, HaxeProject::get_haxelib_dir());
	if (error != OK) {
		r_output = vformat("Cannot run '%s'.", HaxeProject::get_haxelib_executable());
		return error;
	}
	while (process.poll()) {
		OS::get_singleton()->delay_usec(2000);
	}
	r_output = process.get_output();
	r_exit_code = process.get_exit_code();
	return OK;
}

int HaxeBuild::parse_progress(const String &p_output) {
	// haxelib rewrites one line with a carriage return (" 42%\r"), so the
	// current value is the last percentage it printed.
	int percent = p_output.rfind_char('%');
	if (percent < 0) {
		return -1;
	}
	int end = percent - 1;
	while (end >= 0 && (p_output[end] == ' ' || p_output[end] == '\t')) {
		end--;
	}
	int begin = end;
	while (begin >= 0 && p_output[begin] >= '0' && p_output[begin] <= '9') {
		begin--;
	}
	if (begin == end) {
		return -1;
	}
	int value = p_output.substr(begin + 1, end - begin).to_int();
	if (value < 0 || value > 100) {
		return -1;
	}
	return value;
}

Error HaxeBuild::record_installed_library(const String &p_name, const String &p_version, String &r_output) {
	// The build resolves libraries from haxe.json, so a successful install
	// must also become a recorded dependency; otherwise the library would sit
	// in the repository unused.
	Vector<HaxeProject::Dependency> dependencies;
	String manifest_error;
	if (HaxeProject::load_manifest(dependencies, manifest_error) != OK) {
		r_output += "\n" + manifest_error;
		return FAILED;
	}
	bool present = false;
	for (const HaxeProject::Dependency &dependency : dependencies) {
		if (dependency.name == p_name) {
			present = true;
			break;
		}
	}
	if (!present) {
		HaxeProject::Dependency dependency;
		dependency.name = p_name;
		dependency.version = p_version;
		dependencies.push_back(dependency);
		if (HaxeProject::save_manifest(dependencies, manifest_error) != OK) {
			r_output += "\n" + manifest_error;
			return FAILED;
		}
	}
	return OK;
}

Error HaxeBuild::forget_removed_library(const String &p_name, String &r_output) {
	Vector<HaxeProject::Dependency> dependencies;
	String manifest_error;
	if (HaxeProject::load_manifest(dependencies, manifest_error) != OK) {
		r_output += "\n" + manifest_error;
		return FAILED;
	}
	Vector<HaxeProject::Dependency> remaining;
	for (const HaxeProject::Dependency &dependency : dependencies) {
		if (dependency.name != p_name) {
			remaining.push_back(dependency);
		}
	}
	if (remaining.size() != dependencies.size() && HaxeProject::save_manifest(remaining, manifest_error) != OK) {
		r_output += "\n" + manifest_error;
		return FAILED;
	}
	return OK;
}

Error HaxeBuild::install_library(const String &p_name, const String &p_version, bool p_skip_dependencies, String &r_output) {
	if (prepare_haxelib_repository(r_output) != OK) {
		return FAILED;
	}
	int exit_code = 0;
	if (run_haxelib_blocking(install_arguments(p_name, p_version, p_skip_dependencies), r_output, exit_code) != OK) {
		return FAILED;
	}
	if (exit_code != 0) {
		return FAILED;
	}
	return record_installed_library(p_name, p_version, r_output);
}

Error HaxeBuild::update_library(const String &p_name, String &r_output) {
	int exit_code = 0;
	if (run_haxelib_blocking(update_arguments(p_name), r_output, exit_code) != OK) {
		return FAILED;
	}
	return exit_code == 0 ? OK : FAILED;
}

Error HaxeBuild::remove_library(const String &p_name, String &r_output) {
	int exit_code = 0;
	if (run_haxelib_blocking(remove_arguments(p_name), r_output, exit_code) != OK) {
		return FAILED;
	}
	if (exit_code != 0) {
		return FAILED;
	}
	return forget_removed_library(p_name, r_output);
}
