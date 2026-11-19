/**************************************************************************/
/*  haxe_runtime.cpp                                                      */
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

#include "haxe_runtime.h"

#include "haxe_support.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/os/os.h"
#include "core/variant/callable.h"
#include "core/version.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Return values handed to Haxe must outlive the host callback, because the
// generated glue converts them after the callback returns.
thread_local HxValueHolder return_holder;

HxValueHolder &holder_for(const Variant &p_value) {
	hx_value_from_variant(p_value, return_holder, true);
	return return_holder;
}

int64_t normalize_object_id(int64_t p_id) {
	return p_id;
}

void *hx_library_symbol(void *p_handle, const char *p_name) {
#if defined(WINDOWS_ENABLED)
	return (void *)GetProcAddress((HMODULE)p_handle, p_name);
#else
	return dlsym(p_handle, p_name);
#endif
}

} // namespace

/* A Callable implemented by a method on a Haxe script instance. Created when
 * Haxe code hands a Callable to the engine (signal connections, timers,
 * Tween callbacks, ...). */
class HaxeCallableCustom : public CallableCustom {
public:
	HaxeLibrary *library = nullptr;
	int64_t object_id = 0;
	StringName method;
	bool valid = true;

	HaxeCallableCustom(HaxeLibrary *p_library, int64_t p_object_id, const StringName &p_method) :
			library(p_library), object_id(p_object_id), method(p_method) {
		if (library) {
			library->add_reference();
		}
	}

	~HaxeCallableCustom() override {
		if (library) {
			library->release_reference();
		}
	}

	uint32_t hash() const override {
		return hash_murmur3_one_64((uint64_t)object_id) ^ (uint32_t)method.hash();
	}

	String get_as_text() const override {
		return vformat("HaxeCallable(%d::%s)", object_id, method);
	}

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
		const HaxeCallableCustom *a = static_cast<const HaxeCallableCustom *>(p_a);
		const HaxeCallableCustom *b = static_cast<const HaxeCallableCustom *>(p_b);
		return a->object_id == b->object_id && a->method == b->method;
	}

	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
		const HaxeCallableCustom *a = static_cast<const HaxeCallableCustom *>(p_a);
		const HaxeCallableCustom *b = static_cast<const HaxeCallableCustom *>(p_b);
		if (a->object_id != b->object_id) {
			return a->object_id < b->object_id;
		}
		return a->method < b->method;
	}

	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override { return valid && library != nullptr; }
	ObjectID get_object() const override { return ObjectID((uint64_t)object_id); }
	StringName get_method() const override { return method; }

	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override {
		if (library == nullptr) {
			r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
			return;
		}
		Vector<HxValueHolder> holders;
		holders.resize(p_argcount);
		Vector<HxValue> values;
		values.resize(p_argcount);
		for (int i = 0; i < p_argcount; i++) {
			hx_value_from_variant(*p_arguments[i], holders.write[i]);
			values.write[i] = holders[i].value;
		}
		HxValue ret = {};
		int result = library->invoke(object_id, method, p_argcount ? values.ptr() : nullptr, p_argcount, &ret);
		if (result == 1) {
			r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
			return;
		}
		if (result != 0) {
			r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
			HaxeRuntime::report_error(vformat("Haxe callable %s failed: %s", method, library->get_last_error()));
			return;
		}
		if (!hx_value_to_variant(ret, r_return_value)) {
			r_return_value = Variant();
		}
		library->release_return();
		r_call_error.error = Callable::CallError::CALL_OK;
	}
};

/* A Callable implemented by a Haxe closure. Created when Haxe code hands a
 * plain function to the engine (signal connections, timer callbacks, ...). */
class HaxeClosureCallableCustom : public CallableCustom {
public:
	HaxeLibrary *library = nullptr;
	int64_t closure = 0;

	HaxeClosureCallableCustom(HaxeLibrary *p_library, int64_t p_closure) :
			library(p_library), closure(p_closure) {
		if (library != nullptr) {
			library->add_reference();
			library->retain_closure(closure);
		}
	}

	~HaxeClosureCallableCustom() override {
		if (library != nullptr) {
			library->release_closure(closure);
			library->release_reference();
		}
	}

	uint32_t hash() const override {
		return hash_murmur3_one_64((uint64_t)closure) ^ hash_murmur3_one_64((uint64_t)(uintptr_t)library);
	}

	String get_as_text() const override {
		return vformat("HaxeClosure(%s#%d)", library ? library->path : String("<unloaded>"), (int64_t)closure);
	}

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
		const HaxeClosureCallableCustom *a = static_cast<const HaxeClosureCallableCustom *>(p_a);
		const HaxeClosureCallableCustom *b = static_cast<const HaxeClosureCallableCustom *>(p_b);
		return a->library == b->library && a->closure == b->closure;
	}

	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
		const HaxeClosureCallableCustom *a = static_cast<const HaxeClosureCallableCustom *>(p_a);
		const HaxeClosureCallableCustom *b = static_cast<const HaxeClosureCallableCustom *>(p_b);
		if (a->library != b->library) {
			return (uintptr_t)a->library < (uintptr_t)b->library;
		}
		return a->closure < b->closure;
	}

	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override { return library != nullptr; }
	ObjectID get_object() const override { return ObjectID(); }
	StringName get_method() const override { return StringName(); }

	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override {
		if (library == nullptr) {
			r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
			return;
		}
		Vector<HxValueHolder> holders;
		holders.resize(p_argcount);
		Vector<HxValue> values;
		values.resize(p_argcount);
		for (int i = 0; i < p_argcount; i++) {
			hx_value_from_variant(*p_arguments[i], holders.write[i]);
			values.write[i] = holders[i].value;
		}
		HxValue returned = {};
		int result = library->invoke_closure(closure, p_argcount ? values.ptr() : nullptr, p_argcount, &returned);
		if (result != 0) {
			HaxeRuntime::report_error(vformat("Haxe closure failed: %s", library->get_last_error()));
			r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
			return;
		}
		if (!hx_value_to_variant(returned, r_return_value)) {
			r_return_value = Variant();
		}
		library->release_return();
		r_call_error.error = Callable::CallError::CALL_OK;
	}
};

namespace {

/* Tracks which library is executing on the current thread. Nested engine
 * callbacks (Haxe calling an engine method that calls back into Haxe) are
 * supported through a stack. */
thread_local Vector<HaxeLibrary *> t_library_stack;

struct ActiveLibraryScope {
	ActiveLibraryScope(HaxeLibrary *p_library) {
		if (p_library != nullptr) {
			t_library_stack.push_back(p_library);
		}
	}

	~ActiveLibraryScope() {
		if (!t_library_stack.is_empty()) {
			t_library_stack.remove_at(t_library_stack.size() - 1);
		}
	}
};

} // namespace

HaxeRuntime *HaxeRuntime::singleton = nullptr;

HaxeRuntime *HaxeRuntime::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(HaxeRuntime);
	}
	return singleton;
}

HaxeLibrary *HaxeRuntime::get_active_library() {
	return t_library_stack.is_empty() ? nullptr : t_library_stack[t_library_stack.size() - 1];
}

void HaxeRuntime::push_active_library(HaxeLibrary *p_library) {
	if (p_library != nullptr) {
		t_library_stack.push_back(p_library);
	}
}

void HaxeRuntime::pop_active_library() {
	if (!t_library_stack.is_empty()) {
		t_library_stack.remove_at(t_library_stack.size() - 1);
	}
}

Callable haxe_make_closure_callable(HaxeLibrary *p_library, int64_t p_closure) {
	if (p_library == nullptr) {
		return Callable();
	}
	return Callable(memnew(HaxeClosureCallableCustom(p_library, p_closure)));
}

HaxeRuntime::HaxeRuntime() {
}

const String &HaxeRuntime::resolve_support_dir() const {
	if (support_resolved) {
		return support_dir;
	}
	support_resolved = true;

	String exec_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	Vector<String> candidates;
	// A directory next to the executable still wins: it lets a developer
	// iterate on the support files without rebuilding the engine.
	candidates.push_back(exec_dir.path_join("haxe"));
#ifdef TOOLS_ENABLED
	candidates.push_back(exec_dir.path_join("../modules/haxe/dist"));
	candidates.push_back(exec_dir.path_join("../../modules/haxe/dist"));
#endif
	for (const String &candidate : candidates) {
		if (DirAccess::dir_exists_absolute(candidate)) {
			support_dir = candidate;
			return support_dir;
		}
	}

	// Only an exported game falls back to the files embedded in the binary;
	// extracting them writes to the user data directory, so it is deferred
	// until nothing else can serve the support directory.
	String extracted = HaxeSupport::get_dir();
	if (!extracted.is_empty() && DirAccess::dir_exists_absolute(extracted)) {
		support_dir = extracted;
	}
	return support_dir;
}

bool HaxeRuntime::has_support_files() const {
	if (HaxeSupport::is_available()) {
		return true;
	}
	String exec_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	if (DirAccess::dir_exists_absolute(exec_dir.path_join("haxe"))) {
		return true;
	}
#ifdef TOOLS_ENABLED
	if (DirAccess::dir_exists_absolute(exec_dir.path_join("../modules/haxe/dist")) ||
			DirAccess::dir_exists_absolute(exec_dir.path_join("../../modules/haxe/dist"))) {
		return true;
	}
#endif
	return false;
}

String HaxeRuntime::get_stdlib_dir() const {
	const String &dir = resolve_support_dir();
	return dir.is_empty() ? String() : dir.path_join("std");
}

String HaxeRuntime::get_bindings_dir() const {
	const String &dir = resolve_support_dir();
	return dir.is_empty() ? String() : dir.path_join("bindings");
}

String HaxeRuntime::get_toolchain_dir() const {
	const String &dir = resolve_support_dir();
	return dir.is_empty() ? String() : dir.path_join("toolchain");
}

void HaxeRuntime::report_error(const String &p_message) {
	ERR_PRINT(p_message);
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		// Editor level reporting is routed through the build/error UI by the
		// caller; printing is the reliable fallback for headless runs.
	}
#endif
}

void HaxeRuntime::report_warning(const String &p_message) {
	WARN_PRINT(p_message);
}

void HaxeRuntime::report_info(const String &p_message) {
	print_line(p_message);
}

HaxeLibrary *HaxeRuntime::get_library_for_path(const String &p_path) const {
	for (HaxeLibrary *library : libraries) {
		if (library->path == p_path) {
			return library;
		}
	}
	return nullptr;
}

HaxeLibrary *HaxeRuntime::load_library(const String &p_path, String &r_error) {
	HaxeLibrary *existing = get_library_for_path(p_path);
	if (existing) {
		existing->add_reference();
		return existing;
	}
	HaxeLibrary *library = HaxeLibrary::load(p_path, r_error);
	if (library == nullptr) {
		return nullptr;
	}
	libraries.push_back(library);
	// Index 0 is the caller's reference; instances add their own.
	library->add_reference();
	return library;
}

void HaxeRuntime::unload_all() {
	/* Libraries are intentionally kept alive: the embedded runtime cannot be
	 * shut down safely, and Callable values may still point into them while
	 * the engine tears down. Not deleting them also keeps those pointers
	 * valid for the rest of the process. */
	libraries.clear();
	pinned_objects.clear();
	pinned_refs.clear();
	callable_handles.clear();
}

HxHostApi HaxeRuntime::make_host_api() {
	HxHostApi api = {};
	api.version = HXGODOT_ABI_VERSION;
	api.log = &HaxeRuntime::host_log;
	api.call = &HaxeRuntime::host_call;
	api.get_property = &HaxeRuntime::host_get_property;
	api.set_property = &HaxeRuntime::host_set_property;
	api.has_method = &HaxeRuntime::host_has_method;
	api.is_instance_valid = &HaxeRuntime::host_is_instance_valid;
	api.get_class_name = &HaxeRuntime::host_get_class_name;
	api.ref_object = &HaxeRuntime::host_ref_object;
	api.unref_object = &HaxeRuntime::host_unref_object;
	api.call_callable = &HaxeRuntime::host_call_callable;
	api.free_callable = &HaxeRuntime::host_free_callable;
	api.call_static = &HaxeRuntime::host_call_static;
	api.get_singleton = &HaxeRuntime::host_get_singleton;
	api.utility = &HaxeRuntime::host_utility;
	api.variant_call = &HaxeRuntime::host_variant_call;
	api.variant_call_static = &HaxeRuntime::host_variant_call_static;
	api.variant_get_member = &HaxeRuntime::host_variant_get_member;
	api.variant_set_member = &HaxeRuntime::host_variant_set_member;
	api.create_object = &HaxeRuntime::host_create_object;
	api.is_ref_counted = &HaxeRuntime::host_is_ref_counted;
	api.has_property = &HaxeRuntime::host_has_property;
	api.variant_call_mutating = &HaxeRuntime::host_variant_call_mutating;
	api.value_equal = &HaxeRuntime::host_value_equal;
	api.value_compare = &HaxeRuntime::host_value_compare;
	return api;
}

Object *HaxeRuntime::resolve_object(int64_t p_id) {
	if (p_id == 0) {
		return nullptr;
	}
	return ObjectDB::get_instance(ObjectID(normalize_object_id(p_id)));
}

String HaxeRuntime::get_native_class_name(Object *p_object) {
	if (p_object == nullptr) {
		return String();
	}
	return p_object->get_class();
}

void HaxeRuntime::pin_object(Object *p_object) {
	if (p_object == nullptr || Object::cast_to<RefCounted>(p_object) == nullptr) {
		return;
	}
	host_ref_object((int64_t)p_object->get_instance_id());
}

void HaxeRuntime::host_log(int p_level, const char *p_message) {
	if (p_message == nullptr) {
		return;
	}
	switch (p_level) {
		case 2:
			ERR_PRINT(p_message);
			break;
		case 1:
			WARN_PRINT(p_message);
			break;
		default:
			print_line(p_message);
			break;
	}
}

int HaxeRuntime::host_call(int64_t p_object, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	Object *object = resolve_object(p_object);
	if (object == nullptr) {
		return 3;
	}
	if (p_method == nullptr) {
		return 2;
	}
	StringName method = StringName(String::utf8(p_method));
	if (!object->has_method(method)) {
		return 1;
	}

	Vector<Variant> args;
	args.resize(p_argc);
	Vector<const Variant *> arg_ptrs;
	arg_ptrs.resize(p_argc);
	for (int i = 0; i < p_argc; i++) {
		if (!hx_value_to_variant(p_args[i], args.write[i])) {
			HaxeRuntime::report_error(vformat("Haxe: cannot convert argument %d of %s::%s", i, get_native_class_name(object), method));
			return 2;
		}
	}
	for (int i = 0; i < p_argc; i++) {
		arg_ptrs.write[i] = &args[i];
	}

	Callable::CallError error;
	Variant result = object->callp(method, arg_ptrs.ptrw(), p_argc, error);
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_get_property(int64_t p_object, const char *p_name, HxValue *r_ret) {
	Object *object = resolve_object(p_object);
	if (object == nullptr) {
		return 3;
	}
	if (p_name == nullptr) {
		return 2;
	}
	StringName name = StringName(String::utf8(p_name));
	// Probe through the property list first so unknown names do not spam the
	// console with engine errors.
	bool found = false;
	List<PropertyInfo> properties;
	object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == name) {
			found = true;
			break;
		}
	}
	if (!found) {
		return 1;
	}
	Variant value = object->get(name);
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(value);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_set_property(int64_t p_object, const char *p_name, const HxValue *p_value) {
	Object *object = resolve_object(p_object);
	if (object == nullptr) {
		return 3;
	}
	if (p_name == nullptr || p_value == nullptr) {
		return 2;
	}
	Variant value;
	if (!hx_value_to_variant(*p_value, value)) {
		return 2;
	}
	bool valid = false;
	object->set(StringName(String::utf8(p_name)), value, &valid);
	return valid ? 0 : 1;
}

int HaxeRuntime::host_has_method(int64_t p_object, const char *p_method) {
	Object *object = resolve_object(p_object);
	if (object == nullptr || p_method == nullptr) {
		return 0;
	}
	return object->has_method(StringName(String::utf8(p_method))) ? 1 : 0;
}

int HaxeRuntime::host_is_instance_valid(int64_t p_object) {
	return resolve_object(p_object) != nullptr ? 1 : 0;
}

int HaxeRuntime::host_get_class_name(int64_t p_object, char *r_out, int p_out_size) {
	Object *object = resolve_object(p_object);
	if (object == nullptr || r_out == nullptr || p_out_size <= 0) {
		return 1;
	}
	CharString class_name = get_native_class_name(object).utf8();
	snprintf(r_out, p_out_size, "%s", class_name.get_data());
	return 0;
}

void HaxeRuntime::host_ref_object(int64_t p_object) {
	Object *object = resolve_object(p_object);
	if (object == nullptr) {
		return;
	}
	RefCounted *ref_counted = Object::cast_to<RefCounted>(object);
	if (ref_counted == nullptr) {
		return;
	}
	HaxeRuntime *runtime = get_singleton();
	uint64_t key = (uint64_t)p_object;
	int &count = runtime->pinned_objects[key];
	if (count == 0) {
		runtime->pinned_refs[key] = Ref<RefCounted>(ref_counted);
	}
	count++;
}

void HaxeRuntime::host_unref_object(int64_t p_object) {
	HaxeRuntime *runtime = get_singleton();
	uint64_t key = (uint64_t)p_object;
	HashMap<uint64_t, int>::Iterator it = runtime->pinned_objects.find(key);
	if (it == runtime->pinned_objects.end()) {
		return;
	}
	it->value--;
	if (it->value <= 0) {
		runtime->pinned_objects.remove(it);
		runtime->pinned_refs.erase(key);
	}
}

int64_t HaxeRuntime::store_callable(const Callable &p_callable) {
	HaxeRuntime *runtime = get_singleton();
	int64_t handle = runtime->next_callable_handle++;
	runtime->callable_handles[handle] = p_callable;
	return handle;
}

Callable HaxeRuntime::get_callable(int64_t p_handle) {
	HaxeRuntime *runtime = get_singleton();
	HashMap<int64_t, Callable>::Iterator it = runtime->callable_handles.find(p_handle);
	return it == runtime->callable_handles.end() ? Callable() : it->value;
}

void HaxeRuntime::drop_callable(int64_t p_handle) {
	get_singleton()->callable_handles.erase(p_handle);
}

int HaxeRuntime::host_call_callable(int64_t p_handle, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	Callable callable = get_callable(p_handle);
	if (!callable.is_valid()) {
		return 1;
	}
	Vector<Variant> args;
	args.resize(p_argc);
	Vector<const Variant *> arg_ptrs;
	arg_ptrs.resize(p_argc);
	for (int i = 0; i < p_argc; i++) {
		if (!hx_value_to_variant(p_args[i], args.write[i])) {
			return 2;
		}
	}
	for (int i = 0; i < p_argc; i++) {
		arg_ptrs.write[i] = &args[i];
	}
	Callable::CallError error;
	Variant result;
	callable.callp(arg_ptrs.ptrw(), p_argc, result, error);
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	return 0;
}

void HaxeRuntime::host_free_callable(int64_t p_handle) {
	drop_callable(p_handle);
}

int HaxeRuntime::host_call_static(const char *p_class, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	if (p_class == nullptr || p_method == nullptr) {
		return 2;
	}
	MethodBind *bind = ClassDB::get_method(StringName(String::utf8(p_class)), StringName(String::utf8(p_method)));
	if (bind == nullptr) {
		return 1;
	}
	if (!bind->is_static()) {
		return 2;
	}
	Vector<Variant> args;
	args.resize(p_argc);
	Vector<const Variant *> arg_ptrs;
	arg_ptrs.resize(p_argc);
	for (int i = 0; i < p_argc; i++) {
		if (!hx_value_to_variant(p_args[i], args.write[i])) {
			return 2;
		}
	}
	for (int i = 0; i < p_argc; i++) {
		arg_ptrs.write[i] = &args[i];
	}
	Callable::CallError error;
	Variant result = bind->call(nullptr, arg_ptrs.ptrw(), p_argc, error);
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_get_singleton(const char *p_name, int64_t *r_object) {
	if (p_name == nullptr || r_object == nullptr) {
		return 2;
	}
	Object *object = Engine::get_singleton()->get_singleton_object(StringName(String::utf8(p_name)));
	if (object == nullptr) {
		return 1;
	}
	*r_object = (int64_t)object->get_instance_id();
	return 0;
}

int HaxeRuntime::host_utility(const char *p_op, int64_t p_object, const char *p_text, HxValue *r_ret) {
	if (p_op == nullptr) {
		return 2;
	}
	String op = String::utf8(p_op);
	if (op == "class_exists") {
		if (r_ret == nullptr || p_text == nullptr) {
			return 2;
		}
		HxValueHolder &holder = holder_for(ClassDB::class_exists(StringName(String::utf8(p_text))));
		*r_ret = holder.value;
		return 0;
	}
	if (op == "class_parent") {
		if (r_ret == nullptr || p_text == nullptr) {
			return 2;
		}
		HxValueHolder &holder = holder_for(ClassDB::get_parent_class(StringName(String::utf8(p_text))).operator String());
		*r_ret = holder.value;
		return 0;
	}
	if (op == "engine_version") {
		if (r_ret == nullptr) {
			return 2;
		}
		HxValueHolder &holder = holder_for(String(VERSION_FULL_NAME));
		*r_ret = holder.value;
		return 0;
	}
	(void)p_object;
	return 1;
}

/* Converts a HxValue argument list into engine Variants. */
static bool hx_values_to_variants(const HxValue *p_args, int p_argc, Vector<Variant> &r_args, Vector<const Variant *> &r_ptrs) {
	r_args.resize(p_argc);
	r_ptrs.resize(p_argc);
	for (int i = 0; i < p_argc; i++) {
		if (!hx_value_to_variant(p_args[i], r_args.write[i])) {
			return false;
		}
	}
	for (int i = 0; i < p_argc; i++) {
		r_ptrs.write[i] = &r_args[i];
	}
	return true;
}

int HaxeRuntime::host_variant_call(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	if (p_target == nullptr || p_method == nullptr) {
		return 2;
	}
	Variant target;
	if (!hx_value_to_variant(*p_target, target)) {
		return 2;
	}
	Vector<Variant> args;
	Vector<const Variant *> arg_ptrs;
	if (!hx_values_to_variants(p_args, p_argc, args, arg_ptrs)) {
		return 2;
	}
	Callable::CallError error;
	Variant result;
	target.callp(StringName(String::utf8(p_method)), p_argc ? arg_ptrs.ptrw() : nullptr, p_argc, result, error);
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_variant_call_static(const char *p_type, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	if (p_type == nullptr || p_method == nullptr) {
		return 2;
	}
	Variant::Type type = Variant::get_type_by_name(String::utf8(p_type));
	if (type == Variant::VARIANT_MAX) {
		return 2;
	}
	Vector<Variant> args;
	Vector<const Variant *> arg_ptrs;
	if (!hx_values_to_variants(p_args, p_argc, args, arg_ptrs)) {
		return 2;
	}
	Callable::CallError error;
	Variant result;
	StringName method = StringName(String::utf8(p_method));
	if (type == Variant::NIL) {
		/* The @GlobalScope utility functions (print, str, load, range, ...)
		 * live in their own table rather than in the built-in method table. */
		Variant::call_utility_function(method, &result, p_argc ? arg_ptrs.ptrw() : nullptr, p_argc, error);
	} else {
		Variant::call_static(type, method, p_argc ? arg_ptrs.ptrw() : nullptr, p_argc, result, error);
	}
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_variant_get_member(const HxValue *p_target, const char *p_name, HxValue *r_ret) {
	if (p_target == nullptr || p_name == nullptr) {
		return 2;
	}
	Variant target;
	if (!hx_value_to_variant(*p_target, target)) {
		return 2;
	}
	bool valid = false;
	Variant value = target.get_named(StringName(String::utf8(p_name)), valid);
	if (!valid) {
		return 1;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(value);
		*r_ret = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_variant_set_member(const HxValue *p_target, const char *p_name, const HxValue *p_value, HxValue *r_updated) {
	if (p_target == nullptr || p_name == nullptr || p_value == nullptr) {
		return 2;
	}
	Variant target;
	if (!hx_value_to_variant(*p_target, target)) {
		return 2;
	}
	Variant value;
	if (!hx_value_to_variant(*p_value, value)) {
		return 2;
	}
	bool valid = false;
	target.set_named(StringName(String::utf8(p_name)), value, valid);
	if (!valid) {
		return 1;
	}
	if (r_updated != nullptr) {
		HxValueHolder &holder = holder_for(target);
		*r_updated = holder.value;
	}
	return 0;
}

int HaxeRuntime::host_create_object(const char *p_class, int64_t *r_object) {
	if (p_class == nullptr || r_object == nullptr) {
		return 2;
	}
	StringName class_name = StringName(String::utf8(p_class));
	if (!ClassDB::can_instantiate(class_name)) {
		return 1;
	}
	Object *object = ClassDB::instantiate(class_name);
	if (object == nullptr) {
		return 1;
	}
	*r_object = (int64_t)object->get_instance_id();
	return 0;
}

int HaxeRuntime::host_is_ref_counted(int64_t p_object) {
	Object *object = resolve_object(p_object);
	return object != nullptr && Object::cast_to<RefCounted>(object) != nullptr ? 1 : 0;
}

int HaxeRuntime::host_has_property(int64_t p_object, const char *p_name) {
	Object *object = resolve_object(p_object);
	if (object == nullptr || p_name == nullptr) {
		return 0;
	}
	StringName name = StringName(String::utf8(p_name));
	List<PropertyInfo> properties;
	object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.name == name) {
			return 1;
		}
	}
	return 0;
}

int HaxeRuntime::host_variant_call_mutating(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret, HxValue *r_updated) {
	if (p_target == nullptr || p_method == nullptr) {
		return 2;
	}
	Variant target;
	if (!hx_value_to_variant(*p_target, target)) {
		return 2;
	}
	Vector<Variant> args;
	Vector<const Variant *> arg_ptrs;
	if (!hx_values_to_variants(p_args, p_argc, args, arg_ptrs)) {
		return 2;
	}
	Callable::CallError error;
	Variant result;
	target.callp(StringName(String::utf8(p_method)), p_argc ? arg_ptrs.ptrw() : nullptr, p_argc, result, error);
	if (error.error != Callable::CallError::CALL_OK) {
		return 2;
	}
	if (r_ret != nullptr) {
		HxValueHolder &holder = holder_for(result);
		*r_ret = holder.value;
	}
	if (r_updated != nullptr) {
		HxValueHolder &updated = holder_for(target);
		*r_updated = updated.value;
	}
	return 0;
}

int HaxeRuntime::host_value_equal(const HxValue *p_a, const HxValue *p_b, int *r_equal) {
	if (p_a == nullptr || p_b == nullptr || r_equal == nullptr) {
		return 2;
	}
	Variant left;
	Variant right;
	if (!hx_value_to_variant(*p_a, left) || !hx_value_to_variant(*p_b, right)) {
		return 2;
	}
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_EQUAL, left, right, result, valid);
	if (!valid) {
		return 1;
	}
	*r_equal = result.operator bool() ? 1 : 0;
	return 0;
}

int HaxeRuntime::host_value_compare(const HxValue *p_a, const HxValue *p_b, int *r_compare) {
	if (p_a == nullptr || p_b == nullptr || r_compare == nullptr) {
		return 2;
	}
	Variant left;
	Variant right;
	if (!hx_value_to_variant(*p_a, left) || !hx_value_to_variant(*p_b, right)) {
		return 2;
	}
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_LESS, left, right, result, valid);
	if (valid && result.operator bool()) {
		*r_compare = -1;
		return 0;
	}
	valid = false;
	Variant::evaluate(Variant::OP_GREATER, left, right, result, valid);
	if (valid && result.operator bool()) {
		*r_compare = 1;
		return 0;
	}
	if (!valid) {
		return 1;
	}
	*r_compare = 0;
	return 0;
}

/* ---- Library ----------------------------------------------------------- */

HaxeLibrary::~HaxeLibrary() {
	// The library handle intentionally stays open. See HaxeRuntime::unload_all.
}

void HaxeLibrary::release_reference() {
	references--;
	if (references <= 0) {
		references = 0;
	}
}

void HaxeLibrary::capture_error() {
	last_error = String::utf8(fn_last_error ? fn_last_error() : "");
	last_stack_trace = String::utf8(fn_stack_trace ? fn_stack_trace() : "");
}

HaxeLibrary *HaxeLibrary::load(const String &p_path, String &r_error) {
	HaxeLibrary *library = memnew(HaxeLibrary);
	library->path = p_path;
	library->references = 0;

#ifdef WINDOWS_ENABLED
	// `String::utf16()` is a `char16_t` string; `LPCWSTR` is `const wchar_t *`.
	// The two are layout-compatible UTF-16 strings, and the engine's own
	// Windows code casts for the same reason.
	library->handle = (void *)LoadLibraryW((LPCWSTR)p_path.utf16().get_data());
	if (library->handle == nullptr) {
		r_error = vformat("Cannot load Haxe library '%s' (error %d).", p_path, (int)GetLastError());
		memdelete(library);
		return nullptr;
	}
#else
#ifdef WEB_ENABLED
	// The browser loads the exported game's dynamic libraries before the engine
	// starts. The Haxe library is one of them, so by the time a Haxe script is
	// loaded its entry points already live in the page's global symbol
	// namespace. Opening it again with dlopen() is unnecessary and, with the
	// threaded template, fatal: dlopen() makes the calling thread wait for
	// every running worker to acknowledge the new module, and the engine's
	// workers cannot answer while they wait for the main thread. Resolve the
	// entry points from the namespace the module was merged into instead.
	library->handle = RTLD_DEFAULT;
#else
	library->handle = dlopen(p_path.utf8().get_data(), RTLD_NOW | RTLD_LOCAL);
	if (library->handle == nullptr) {
		const char *error = dlerror();
		r_error = vformat("Cannot load Haxe library '%s': %s", p_path, error ? String::utf8(error) : String("unknown error"));
		memdelete(library);
		return nullptr;
	}
#endif
#endif

#ifdef WEB_ENABLED
	// A page that was exported without the Haxe library cannot resolve these
	// symbols at all, and that is not a version mismatch, so report it
	// differently from the check below.
	if (hx_library_symbol(library->handle, "hxgodot_install") == nullptr) {
		r_error = vformat("Haxe library '%s' is not loaded by this web page. Export the project from an editor build that has the Haxe module enabled so the library is listed among the page's dynamic libraries.", p_path);
		memdelete(library);
		return nullptr;
	}
#endif

	bool ok = true;
#define HX_RESOLVE(member, name) \
	library->member = (decltype(library->member))hx_library_symbol(library->handle, name); \
	if (library->member == nullptr) { \
		r_error = vformat("Haxe library '%s' does not export '%s'. It was built against an incompatible version.", p_path, name); \
		ok = false; \
	}

	HX_RESOLVE(fn_install, "hxgodot_install");
	HX_RESOLVE(fn_start, "hxgodot_start");
	HX_RESOLVE(fn_last_error, "hxgodot_last_error");
	HX_RESOLVE(fn_library_info, "hxgodot_library_info");
	HX_RESOLVE(fn_has_class, "hxgodot_has_class");
	HX_RESOLVE(fn_create, "hxgodot_create");
	HX_RESOLVE(fn_has_method, "hxgodot_has_method");
	HX_RESOLVE(fn_invoke, "hxgodot_invoke");
	HX_RESOLVE(fn_get_field, "hxgodot_get_field");
	HX_RESOLVE(fn_set_field, "hxgodot_set_field");
	HX_RESOLVE(fn_destroy, "hxgodot_destroy");
	HX_RESOLVE(fn_stack_trace, "hxgodot_last_stack_trace");
	HX_RESOLVE(fn_invoke_closure, "hxgodot_invoke_closure");
	HX_RESOLVE(fn_retain_closure, "hxgodot_retain_closure");
	HX_RESOLVE(fn_release_closure, "hxgodot_release_closure");
	HX_RESOLVE(fn_release_return, "hxgodot_release_return");
#undef HX_RESOLVE

	if (!ok) {
		memdelete(library);
		return nullptr;
	}

	HxHostApi api = HaxeRuntime::make_host_api();
	ActiveLibraryScope scope(library);
	if (library->fn_install(&api) != 0 || library->fn_start() != 0) {
		library->capture_error();
		r_error = vformat("Haxe library '%s' failed to initialize: %s", p_path, library->last_error);
		memdelete(library);
		return nullptr;
	}

	library->info = String::utf8(library->fn_library_info ? library->fn_library_info() : "");
	#ifdef WEB_ENABLED
	// The module is fetched by the browser; it is not a file the engine's file
	// system can stat, and the stamp is only used to detect that a library file
	// changed on disk.
	library->build_stamp = 0;
#else
	library->build_stamp = (uint64_t)FileAccess::get_modified_time(p_path);
#endif
	return library;
}

bool HaxeLibrary::has_class(const String &p_class) {
	if (fn_has_class == nullptr) {
		return false;
	}
	ActiveLibraryScope scope(this);
	return fn_has_class(p_class.utf8().get_data()) != 0;
}

int HaxeLibrary::create_instance(int64_t p_object_id, const String &p_class) {
	ActiveLibraryScope scope(this);
	int result = fn_create(p_object_id, p_class.utf8().get_data());
	if (result != 0) {
		capture_error();
	}
	return result;
}

int HaxeLibrary::invoke(int64_t p_object_id, const String &p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	ActiveLibraryScope scope(this);
	int result = fn_invoke(p_object_id, p_method.utf8().get_data(), p_args, p_argc, r_ret);
	if (result == 2) {
		capture_error();
	}
	return result;
}

int HaxeLibrary::invoke_closure(int64_t p_closure, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	if (fn_invoke_closure == nullptr) {
		return 2;
	}
	ActiveLibraryScope scope(this);
	int result = fn_invoke_closure(p_closure, p_args, p_argc, r_ret);
	if (result == 2) {
		capture_error();
	}
	return result;
}

void HaxeLibrary::retain_closure(int64_t p_closure) {
	if (fn_retain_closure != nullptr) {
		fn_retain_closure(p_closure);
	}
}

void HaxeLibrary::release_closure(int64_t p_closure) {
	if (fn_release_closure != nullptr) {
		fn_release_closure(p_closure);
	}
}

void HaxeLibrary::release_return() {
	if (fn_release_return != nullptr) {
		fn_release_return();
	}
}

int HaxeLibrary::has_method(int64_t p_object_id, const String &p_method) {
	ActiveLibraryScope scope(this);
	return fn_has_method(p_object_id, p_method.utf8().get_data());
}

int HaxeLibrary::get_field(int64_t p_object_id, const String &p_name, HxValue *r_ret) {
	ActiveLibraryScope scope(this);
	int result = fn_get_field(p_object_id, p_name.utf8().get_data(), r_ret);
	if (result == 2) {
		capture_error();
	}
	return result;
}

int HaxeLibrary::set_field(int64_t p_object_id, const String &p_name, const HxValue *p_value) {
	ActiveLibraryScope scope(this);
	int result = fn_set_field(p_object_id, p_name.utf8().get_data(), p_value);
	if (result == 2) {
		capture_error();
	}
	return result;
}

void HaxeLibrary::destroy_instance(int64_t p_object_id) {
	if (fn_destroy == nullptr) {
		return;
	}
	ActiveLibraryScope scope(this);
	fn_destroy(p_object_id);
}
