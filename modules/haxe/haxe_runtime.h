/**************************************************************************/
/*  haxe_runtime.h                                                        */
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

#include "haxe_abi.h"
#include "haxe_value.h"

#include "core/object/object.h"
#include "core/os/thread_safe.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rid.h"
#include "core/variant/callable.h"

class HaxeCallable;

/* A compiled Haxe project library (one .dso per build configuration).
 *
 * Libraries own the embedded hxcpp runtime instance. Godot keeps the library
 * alive while any script instance created from it still exists. Builds never
 * unload a library: hxcpp has no safe teardown path, and reloading code while
 * callbacks are installed would leave dangling function pointers. A new build
 * is loaded as a new library and picked up by newly created instances.
 */
class HaxeLibrary {
public:
	String path;
	String info;
	uint64_t build_stamp = 0;

	/* On the web `handle` is the null RTLD_DEFAULT, which still means the
	 * library was resolved, so the resolved entry points mark a loaded
	 * library instead. */
	bool is_loaded() const { return fn_install != nullptr; }
	int refcount() const { return references; }

	void add_reference() { references++; }
	void release_reference();

	const String &get_last_error() const { return last_error; }
	const String &get_last_stack_trace() const { return last_stack_trace; }

	bool has_class(const String &p_class);
	int create_instance(int64_t p_object_id, const String &p_class);
	int invoke(int64_t p_object_id, const String &p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	int has_method(int64_t p_object_id, const String &p_method);
	int get_field(int64_t p_object_id, const String &p_name, HxValue *r_ret);
	int set_field(int64_t p_object_id, const String &p_name, const HxValue *p_value);
	void destroy_instance(int64_t p_object_id);

	/* Haxe closures used as engine Callable values. */
	int invoke_closure(int64_t p_closure, const HxValue *p_args, int p_argc, HxValue *r_ret);
	void retain_closure(int64_t p_closure);
	void release_closure(int64_t p_closure);

	/* Drops the memory behind the last value returned by this library on the
	 * current thread. Called after the engine converted the value. */
	void release_return();

	static HaxeLibrary *load(const String &p_path, String &r_error);
	~HaxeLibrary();

private:
	friend class HaxeRuntime;

	HaxeLibrary() = default;

	int references = 0;
	void *handle = nullptr;
	String last_error;
	String last_stack_trace;

	int (*fn_install)(const HxHostApi *) = nullptr;
	int (*fn_start)() = nullptr;
	const char *(*fn_last_error)() = nullptr;
	const char *(*fn_library_info)() = nullptr;
	int (*fn_has_class)(const char *) = nullptr;
	int (*fn_create)(int64_t, const char *) = nullptr;
	int (*fn_has_method)(int64_t, const char *) = nullptr;
	int (*fn_invoke)(int64_t, const char *, const HxValue *, int, HxValue *) = nullptr;
	int (*fn_get_field)(int64_t, const char *, HxValue *) = nullptr;
	int (*fn_set_field)(int64_t, const char *, const HxValue *) = nullptr;
	void (*fn_destroy)(int64_t) = nullptr;
	const char *(*fn_stack_trace)() = nullptr;
	int (*fn_invoke_closure)(int64_t, const HxValue *, int, HxValue *) = nullptr;
	void (*fn_retain_closure)(int64_t) = nullptr;
	void (*fn_release_closure)(int64_t) = nullptr;
	void (*fn_release_return)() = nullptr;

	void capture_error();
};

/* Owns process wide Haxe state: the host API handed to libraries, the object
 * pins that keep RefCounted values alive while Haxe wrappers reference them,
 * the callable handle table and the set of loaded libraries.
 */
class HaxeRuntime {
	_THREAD_SAFE_CLASS_

public:
	static HaxeRuntime *get_singleton();

	HaxeLibrary *load_library(const String &p_path, String &r_error);
	void unload_all();

	Vector<HaxeLibrary *> get_loaded_libraries() const { return libraries; }
	HaxeLibrary *get_library_for_path(const String &p_path) const;

	/* True when a library file can be loaded by this engine build. */
	static bool is_library_abi_compatible(const String &p_path, String &r_error);

	/* Directory that holds the Haxe facing support code shipped with the
	 * engine (bindings, runtime classes, build helpers). */
	String get_stdlib_dir() const;
	String get_bindings_dir() const;
	String get_toolchain_dir() const;
	/* Whether the support files are available at all, without extracting the
	 * embedded copies. Used to distinguish "engine built without Haxe" from
	 * "project has not been compiled yet". */
	bool has_support_files() const;

	/* Reports an error to the editor and the console. */
	static void report_error(const String &p_message);
	static void report_warning(const String &p_message);
	static void report_info(const String &p_message);

	// Host API callbacks, exposed for the ABI table.
	static void host_log(int p_level, const char *p_message);
	static int host_call(int64_t p_object, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	static int host_get_property(int64_t p_object, const char *p_name, HxValue *r_ret);
	static int host_set_property(int64_t p_object, const char *p_name, const HxValue *p_value);
	static int host_has_method(int64_t p_object, const char *p_method);
	static int host_is_instance_valid(int64_t p_object);
	static int host_get_class_name(int64_t p_object, char *r_out, int p_out_size);
	static void host_ref_object(int64_t p_object);
	static void host_unref_object(int64_t p_object);
	static int host_call_callable(int64_t p_handle, const HxValue *p_args, int p_argc, HxValue *r_ret);
	static void host_free_callable(int64_t p_handle);
	static int host_call_static(const char *p_class, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	static int host_get_singleton(const char *p_name, int64_t *r_object);
	static int host_utility(const char *p_op, int64_t p_object, const char *p_text, HxValue *r_ret);
	static int host_variant_call(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	static int host_variant_call_static(const char *p_type, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	static int host_variant_get_member(const HxValue *p_target, const char *p_name, HxValue *r_ret);
	static int host_variant_set_member(const HxValue *p_target, const char *p_name, const HxValue *p_value, HxValue *r_updated);
	static int host_create_object(const char *p_class, int64_t *r_object);
	static int host_is_ref_counted(int64_t p_object);
	static int host_has_property(int64_t p_object, const char *p_name);
	static int host_variant_call_mutating(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret, HxValue *r_updated);
	static int host_value_equal(const HxValue *p_a, const HxValue *p_b, int *r_equal);
	static int host_value_compare(const HxValue *p_a, const HxValue *p_b, int *r_compare);
	static int host_update_texture(int64_t p_texture, const void *p_pixels, int64_t p_size, int p_width, int p_height, int p_format);
	static int host_push_audio_frames(int64_t p_playback, const void *p_samples, int p_frames, int p_channels);

	/* Library whose code is currently running on this thread. Values that only
	 * make sense in the context of a library (Haxe closures, for example)
	 * resolve against it. */
	static HaxeLibrary *get_active_library();
	static void push_active_library(HaxeLibrary *p_library);
	static void pop_active_library();

	/* Keeps a Callable alive while Haxe holds it. */
	static int64_t store_callable(const Callable &p_callable);
	static Callable get_callable(int64_t p_handle);
	static void drop_callable(int64_t p_handle);

	static HxHostApi make_host_api();

	// Object helpers used by the host callbacks and by script instances.
	static Object *resolve_object(int64_t p_id);
	static String get_native_class_name(Object *p_object);

	/* Keeps a RefCounted object alive and marks the value that hands it to a
	 * library as carrying that reference (see the ABI `owned` field). Used for
	 * values the engine returns to a project library. */
	static void pin_object(Object *p_object);

private:
	HaxeRuntime();

	static HaxeRuntime *singleton;

	Vector<HaxeLibrary *> libraries;
	HashMap<uint64_t, int> pinned_objects;
	HashMap<uint64_t, Ref<RefCounted>> pinned_refs;
	HashMap<int64_t, Callable> callable_handles;
	int64_t next_callable_handle = 1;

	/* The support files (std, bindings, toolchain) are resolved on first use:
	 * the embedded copies are only extracted when they are actually needed. */
	mutable String support_dir;
	mutable bool support_resolved = false;
	const String &resolve_support_dir() const;
};

/* Creates a Callable that dispatches to a Haxe closure owned by p_library.
 * Used by HxValue conversion for HXG_CLOSURE values. */
Callable haxe_make_closure_callable(HaxeLibrary *p_library, int64_t p_closure);
