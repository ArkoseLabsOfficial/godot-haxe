/**************************************************************************/
/*  haxe_language.h                                                       */
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

#include "haxe_build.h"
#include "haxe_runtime.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/script_language.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"

class HaxeScript;

/* The Haxe ScriptLanguage implementation.
 *
 * Script metadata comes from the in-engine source parser (inspector, class
 * discovery, validation). Execution is delegated to the compiled project
 * library through the C ABI. Compilation happens through the external Haxe
 * toolchain, driven by HaxeBuild.
 */
class HaxeLanguage : public ScriptLanguage {
	GDCLASS(HaxeLanguage, ScriptLanguage);

	static HaxeLanguage *singleton;

	mutable Mutex lock;

	HaxeLibrary *cached_library = nullptr;
	String cached_stamp;
	String last_build_error;
	String last_build_log;
	bool build_in_progress = false;
	/* A build started from the editor panel, driven by a timer so the editor
	 * keeps responding while the compiler runs. */
	HaxeBuildSession *build_session = nullptr;

	HashSet<HaxeScript *> scripts;
	HashMap<StringName, Variant> global_constants;

	/* Set while a dependent-script cascade is running, so reloading a
	 * dependent does not start a second cascade. */
	bool reloading_dependents = false;
	bool refreshing_exports = false;

	/* The scripts whose class chain contains `p_script`. */
	Vector<HaxeScript *> collect_dependents(HaxeScript *p_script);

	int profiling_frame = 0;

	// Debugger state derived from the last reported stack trace.
	String debug_error;
	Vector<StackInfo> debug_stack;
	void refresh_debug_info();

public:
	static HaxeLanguage *get_singleton() { return singleton; }

	HaxeLanguage();
	~HaxeLanguage();

	String get_name() const override;
	void init() override;
	String get_type() const override;
	String get_extension() const override;
	void finish() override;

	Vector<String> get_reserved_words() const override;
	bool is_control_flow_keyword(const String &p_string) const override;
	Vector<String> get_comment_delimiters() const override;
	Vector<String> get_doc_comment_delimiters() const override;
	Vector<String> get_string_delimiters() const override;
	Ref<Script> make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const override;
	Vector<ScriptTemplate> get_built_in_templates(const StringName &p_object) override;
	bool is_using_templates() override { return true; }
	bool validate(const String &p_script, const String &p_path = "", List<String> *r_functions = nullptr, List<ScriptError> *r_errors = nullptr, List<Warning> *r_warnings = nullptr, HashSet<int> *r_safe_lines = nullptr) const override;
	bool supports_builtin_mode() const override;
	bool supports_documentation() const override { return true; }
	bool can_inherit_from_file() const override { return true; }
	int find_function(const String &p_function, const String &p_code) const override;
	String make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const override;
	bool can_make_function() const override { return true; }
	String validate_path(const String &p_path) const override;
	ScriptNameCasing preferred_file_name_casing() const override { return SCRIPT_NAME_CASING_PASCAL_CASE; }

	void auto_indent_code(String &p_code, int p_from_line, int p_to_line) const override;
	Error complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) override;
	Error lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result) override;
	void add_global_constant(const StringName &p_variable, const Variant &p_value) override;

	String debug_get_error() const override;
	int debug_get_stack_level_count() const override;
	int debug_get_stack_level_line(int p_level) const override;
	String debug_get_stack_level_function(int p_level) const override;
	String debug_get_stack_level_source(int p_level) const override;
	void debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) override;
	void debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems, int p_max_depth) override;
	void debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) override;
	String debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems, int p_max_depth) override;
	Vector<StackInfo> debug_get_current_stack_info() override;

	void reload_all_scripts() override;
	void reload_scripts(const Array &p_scripts, bool p_soft_reload) override;
	void reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) override;

	/* Reloads the scripts that inherit from `p_script`, directly or through
	 * other project classes, after its source changed. Exported members are
	 * collected from the base chain, so a subclass has to be refreshed too or
	 * its Inspector keeps showing the previous inherited properties. */
	void reload_dependent_scripts(HaxeScript *p_script);
	/* Updates the placeholders (Inspector) of the scripts that inherit from
	 * `p_script`, without reloading them from disk. The real-time script
	 * editor parses unsaved text, so reloading would discard the edit; the
	 * base chain is resolved through the in-memory class info instead. */
	void refresh_dependent_exports(HaxeScript *p_script);

	void get_recognized_extensions(List<String> *p_extensions) const override;
	void get_public_functions(List<MethodInfo> *p_functions) const override;
	void get_public_constants(List<Pair<String, Variant>> *p_constants) const override;
	void get_public_annotations(List<MethodInfo> *p_annotations) const override;

	void profiling_start() override;
	void profiling_stop() override;
	void profiling_set_save_native_calls(bool p_enable) override;
	int profiling_get_accumulated_data(ProfilingInfo *p_info_arr, int p_info_max) override;
	int profiling_get_frame_data(ProfilingInfo *p_info_arr, int p_info_max) override;

	bool handles_global_class_type(const String &p_type) const override;
	String get_global_class_name(const String &p_path, String *r_base_type = nullptr, String *r_icon_path = nullptr, bool *r_is_abstract = nullptr, bool *r_is_tool = nullptr) const override;

	/* Returns the library that contains the compiled script. Builds the
	 * project when needed, following the haxe/build/build_on_run setting. The
	 * returned library is owned by the language and stays valid for the
	 * lifetime of the process. */
	HaxeLibrary *get_script_library(HaxeScript *p_script);

	/* Builds the project explicitly (editor build/run buttons). `p_editor`
	 * selects the editor library, which also contains `@:tool` classes. */
	HaxeBuild::Result build_project(bool p_release, bool p_editor, bool p_force, String &r_error);

	/* Starts a build without waiting for the compiler, for the editor
	 * buttons. Returns false when a build is already running (the running one
	 * keeps going). Drive it with `poll_build()` until it returns false. */
	bool start_build(bool p_release, bool p_editor, bool p_force = false);
	/* Reads the output of the running build and finishes it when the compiler
	 * exited. Returns true while the build is still running. */
	bool poll_build();
	/* True while the compiler of a background build is running. */
	bool is_building() const;
	/* True between `start_build()` and `take_build_result()`: the result of a
	 * finished background build stays available until it is taken. */
	bool has_build_session() const;
	/* Output of the running build, so the panel can show it live. */
	String get_build_output() const;
	/* Result of the background build; empty when none was started. */
	const HaxeBuild::Result &get_build_result() const;
	/* Copies and releases the background build result. */
	HaxeBuild::Result take_build_result();
	/* Terminates the background build, if any. */
	void cancel_build();

	const String &get_last_build_error() const { return last_build_error; }
	const String &get_last_build_log() const { return last_build_log; }

	void register_script(HaxeScript *p_script);
	void unregister_script(HaxeScript *p_script);
};

/* Loads and saves .hx files as HaxeScript resources. */
class ResourceFormatLoaderHaxe : public ResourceFormatLoader {
public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

class ResourceFormatSaverHaxe : public ResourceFormatSaver {
public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
	virtual bool recognize(const Ref<Resource> &p_resource) const override;
};
