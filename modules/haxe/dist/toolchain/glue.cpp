/**************************************************************************/
/*  glue.cpp                                                              */
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

/*  glue.cpp                                                              */
/*                                                                        */
/*  Bridge between the Godot engine (plain C ABI) and Haxe/hxcpp (C++      */
/*  ABI). It is compiled into every generated project library, so it      */
/*  shares the library's hxcpp runtime and can allocate Haxe objects      */
/*  directly.                                                             */
/*                                                                        */
/*  The engine never sees hxcpp types: everything crosses the boundary as */
/*  HxValue payloads described by hxgodot_abi.h.                          */

/* `hxcpp.h` must be included before `hxgodot_glue.h`, which declares
 * functions using hxcpp's `Dynamic`, `String` and `Array` types and refuses to
 * compile otherwise. clang-format would sort the quoted headers first, so the
 * block is kept as written. */
// clang-format off
#include <hxcpp.h>

#include "hxgodot_abi.h"
#include "hxgodot_glue.h"
// clang-format on

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void __hxcpp_lib_main();

namespace {

/* ---------------------------------------------------------------- state -- */

HxHostApi g_host = {};
bool g_booted = false;
std::string g_last_error;
std::string g_last_trace;

std::mutex g_state_mutex; /* guards the maps below */
std::mutex g_closure_mutex;

struct Instance {
	Dynamic value;
	hx::Object *root = nullptr; /* GC root, keeps the Haxe object alive */
};

std::unordered_map<int64_t, Instance> g_instances;

struct ClosureEntry {
	Dynamic fn;
	int refs = 0;
};

std::unordered_map<int64_t, ClosureEntry> g_closures;
int64_t g_next_closure = 1;

/* Resources whose release has to happen outside a GC finalizer. Finalizers
 * only queue ids; the queues are drained at the start of every exported and
 * Haxe-callable entry point. */
thread_local std::vector<int64_t> t_release_closures;
thread_local std::vector<int64_t> t_free_callables;
thread_local std::vector<int64_t> t_unpin_objects;

/* Set while hxgodot_create() runs a Haxe constructor for a script instance:
 * the engine object already exists, so generated `new()` must not create
 * another one. The id and class of the object under construction are kept so
 * the generated `super()` chain can bind the existing node to the wrapper
 * before the constructor body runs; a constructor is allowed to call engine
 * methods (`centered = false`, ...) on `this`. */
thread_local bool t_creating_script_instance = false;
thread_local int64_t t_script_object_id = 0;
thread_local void *t_script_class = nullptr;

/* hxcpp finds the Haxe objects that are still in use by conservatively
 * scanning the native stack, from the current stack pointer up to the "top of
 * stack" frame it recorded when the runtime booted. That frame belongs to the
 * startup call into the library and is long gone by the time the engine calls
 * back into Haxe, so a collection triggered from an engine entry point could
 * scan an empty range and free objects that are still live on the stack.
 * Constructing this at the top of every entry point re-registers the current
 * frame; hxcpp keeps the highest frame it has seen, so the scan always covers
 * the running code. */
struct StackTopScope {
	int marker = 0;
	StackTopScope() { hx::SetTopOfStack(&marker, false); }
};

/* Maps a wrapper object to the native resource it owns. Finalizers must not
 * allocate or read GC strings, so ownership is tracked here instead of in
 * Haxe fields. */
std::mutex g_owner_mutex;
std::unordered_map<hx::Object *, int64_t> g_closure_owners;
std::unordered_map<hx::Object *, int64_t> g_handle_owners;
std::unordered_map<hx::Object *, int64_t> g_pin_owners;

/* Return values point into this arena. The engine consumes a returned HxValue
 * immediately and then calls hxgodot_release_return(), which drops the
 * memory. */
struct Arena {
	std::deque<std::string> strings;
	std::deque<std::vector<uint8_t>> blobs;

	void clear() {
		strings.clear();
		blobs.clear();
	}

	const char *store(const String &p_text) {
		const char *utf8 = p_text.utf8_str();
		strings.emplace_back(utf8 ? utf8 : "");
		return strings.back().c_str();
	}

	const uint8_t *store(const std::vector<uint8_t> &p_blob) {
		blobs.push_back(p_blob);
		return blobs.back().data();
	}
};

thread_local Arena t_arena;

void set_error(const char *p_message) {
	g_last_error = p_message ? p_message : "";
	g_last_trace.clear();
}

void set_error(const String &p_message) {
	g_last_error = p_message.utf8_str();
	g_last_trace.clear();
}

void capture_exception(Dynamic &p_exception) {
	g_last_error = p_exception.mPtr ? p_exception->toString().utf8_str() : "unknown exception";
	g_last_trace.clear();
#ifdef HXCPP_STACK_TRACE
	hx::StackContext *context = hx::StackContext::getCurrent();
	if (context != nullptr) {
		context->beginCatch(true);
		Array<String> frames = Array_obj<String>::__new();
		context->getCurrentExceptionStackAsStrings(frames);
		std::string trace;
		for (int i = 0; i < frames->length; i++) {
			if (i > 0) {
				trace += "\n";
			}
			trace += frames->__get(i).utf8_str();
		}
		g_last_trace = trace;
	}
#endif
}

/* --------------------------------------------------------------- values -- */

void put_u8(std::vector<uint8_t> &r_blob, uint8_t p_value) {
	r_blob.push_back(p_value);
}

void put_u32(std::vector<uint8_t> &r_blob, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		r_blob.push_back((uint8_t)((p_value >> (i * 8)) & 0xFF));
	}
}

void put_u64(std::vector<uint8_t> &r_blob, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		r_blob.push_back((uint8_t)((p_value >> (i * 8)) & 0xFF));
	}
}

void put_f64(std::vector<uint8_t> &r_blob, double p_value) {
	uint64_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	put_u64(r_blob, bits);
}

void put_text(std::vector<uint8_t> &r_blob, const String &p_text) {
	const char *utf8 = p_text.utf8_str();
	size_t length = utf8 ? strlen(utf8) : 0;
	put_u32(r_blob, (uint32_t)length);
	for (size_t i = 0; i < length; i++) {
		r_blob.push_back((uint8_t)utf8[i]);
	}
}

struct Reader {
	const uint8_t *data = nullptr;
	int64_t size = 0;
	int64_t pos = 0;

	Reader(const uint8_t *p_data, int64_t p_size) :
			data(p_data), size(p_size) {}

	bool read_u8(uint8_t &r_value) {
		if (pos >= size) {
			return false;
		}
		r_value = data[pos++];
		return true;
	}

	bool read_u32(uint32_t &r_value) {
		if (size - pos < 4) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 4; i++) {
			r_value |= (uint32_t)data[pos + i] << (i * 8);
		}
		pos += 4;
		return true;
	}

	bool read_u64(uint64_t &r_value) {
		if (size - pos < 8) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 8; i++) {
			r_value |= (uint64_t)data[pos + i] << (i * 8);
		}
		pos += 8;
		return true;
	}

	bool read_f64(double &r_value) {
		uint64_t bits = 0;
		if (!read_u64(bits)) {
			return false;
		}
		memcpy(&r_value, &bits, sizeof(r_value));
		return true;
	}

	bool read_text(String &r_text) {
		uint32_t length = 0;
		if (!read_u32(length)) {
			return false;
		}
		if ((int64_t)length > size - pos) {
			return false;
		}
		r_text = String::create((const char *)data + pos, (int)length);
		pos += length;
		return true;
	}
};

/* -------------------------------------------------------- class helpers -- */

std::unordered_map<std::string, hx::Class> g_class_cache;
std::unordered_map<void *, int> g_kind_cache;
std::unordered_map<void *, bool> g_object_class_cache;

hx::Class resolve_class(const char *p_name) {
	std::string key(p_name);
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<std::string, hx::Class>::iterator it = g_class_cache.find(key);
		if (it != g_class_cache.end()) {
			return it->second;
		}
	}
	hx::Class result = hx::Class_obj::Resolve(String(p_name));
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		g_class_cache[key] = result;
	}
	return result;
}

String parent_class_name(const std::string &p_name) {
	if (g_host.utility == nullptr) {
		return String();
	}
	HxValue value = {};
	if (g_host.utility("class_parent", 0, p_name.c_str(), &value) != 0) {
		return String();
	}
	return value.s ? String::create(value.s) : String();
}

/* Resolves the Haxe wrapper for a native class name, walking up the engine
 * hierarchy until a generated binding exists. Editor-only classes are not
 * part of exported games, so the fallback matters. */
hx::Class resolve_object_wrapper(const char *p_class) {
	std::string key(p_class);
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<std::string, hx::Class>::iterator it = g_class_cache.find("object:" + key);
		if (it != g_class_cache.end()) {
			return it->second;
		}
	}
	std::string current = key;
	hx::Class result;
	int guard = 0;
	while (!current.empty() && guard++ < 128) {
		result = hx::Class_obj::Resolve(String("godot.") + String(current.c_str()));
		if (result.mPtr == nullptr) {
			// A GDScript stub (`class Hud extends godot.Node2D`) lives in the
			// top level package, next to the project's own classes.
			result = hx::Class_obj::Resolve(String(current.c_str()));
		}
		if (result.mPtr != nullptr) {
			break;
		}
		String parent = parent_class_name(current);
		current = parent.utf8_str() ? parent.utf8_str() : "";
	}
	if (result.mPtr == nullptr) {
		result = hx::Class_obj::Resolve(String("godot.Object"));
	}
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		g_class_cache["object:" + key] = result;
	}
	return result;
}

int kind_for_class_name(const char *p_name) {
	if (strcmp(p_name, "godot.Vector2") == 0) {
		return HXG_VECTOR2;
	}
	if (strcmp(p_name, "godot.Vector2i") == 0) {
		return HXG_VECTOR2I;
	}
	if (strcmp(p_name, "godot.Vector3") == 0) {
		return HXG_VECTOR3;
	}
	if (strcmp(p_name, "godot.Vector3i") == 0) {
		return HXG_VECTOR3I;
	}
	if (strcmp(p_name, "godot.Vector4") == 0) {
		return HXG_VECTOR4;
	}
	if (strcmp(p_name, "godot.Vector4i") == 0) {
		return HXG_VECTOR4I;
	}
	if (strcmp(p_name, "godot.Rect2") == 0) {
		return HXG_RECT2;
	}
	if (strcmp(p_name, "godot.Rect2i") == 0) {
		return HXG_RECT2I;
	}
	if (strcmp(p_name, "godot.Plane") == 0) {
		return HXG_PLANE;
	}
	if (strcmp(p_name, "godot.Quaternion") == 0) {
		return HXG_QUATERNION;
	}
	if (strcmp(p_name, "godot.AABB") == 0) {
		return HXG_AABB;
	}
	if (strcmp(p_name, "godot.Basis") == 0) {
		return HXG_BASIS;
	}
	if (strcmp(p_name, "godot.Transform2D") == 0) {
		return HXG_TRANSFORM2D;
	}
	if (strcmp(p_name, "godot.Transform3D") == 0) {
		return HXG_TRANSFORM3D;
	}
	if (strcmp(p_name, "godot.Projection") == 0) {
		return HXG_PROJECTION;
	}
	if (strcmp(p_name, "godot.Color") == 0) {
		return HXG_COLOR;
	}
	if (strcmp(p_name, "godot.RID") == 0) {
		return HXG_RID;
	}
	if (strcmp(p_name, "godot.Callable") == 0) {
		return HXG_CALLABLE;
	}
	if (strcmp(p_name, "godot.Signal") == 0) {
		return HXG_SIGNAL;
	}
	if (strcmp(p_name, "godot.Dictionary") == 0) {
		return HXG_DICTIONARY;
	}
	if (strcmp(p_name, "godot.PackedByteArray") == 0) {
		return HXG_PACKED_BYTE_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedInt32Array") == 0) {
		return HXG_PACKED_INT32_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedInt64Array") == 0) {
		return HXG_PACKED_INT64_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedFloat32Array") == 0) {
		return HXG_PACKED_FLOAT32_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedFloat64Array") == 0) {
		return HXG_PACKED_FLOAT64_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedStringArray") == 0) {
		return HXG_PACKED_STRING_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedVector2Array") == 0) {
		return HXG_PACKED_VECTOR2_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedVector3Array") == 0) {
		return HXG_PACKED_VECTOR3_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedColorArray") == 0) {
		return HXG_PACKED_COLOR_ARRAY;
	}
	if (strcmp(p_name, "godot.PackedVector4Array") == 0) {
		return HXG_PACKED_VECTOR4_ARRAY;
	}
	return -1;
}

int framework_kind(hx::Object *p_object) {
	if (p_object == nullptr) {
		return -1;
	}
	hx::Class cls = p_object->__GetClass();
	void *key = cls.mPtr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<void *, int>::iterator it = g_kind_cache.find(key);
		if (it != g_kind_cache.end()) {
			return it->second;
		}
	}
	const char *name = cls->mName.utf8_str();
	int kind = name ? kind_for_class_name(name) : -1;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		g_kind_cache[key] = kind;
	}
	return kind;
}

bool is_godot_object(hx::Object *p_object) {
	if (p_object == nullptr) {
		return false;
	}
	hx::Class cls = p_object->__GetClass();
	void *key = cls.mPtr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<void *, bool>::iterator it = g_object_class_cache.find(key);
		if (it != g_object_class_cache.end()) {
			return it->second;
		}
	}
	hx::Class base = resolve_class("godot.Object");
	bool result = base.mPtr != nullptr && base->CanCast(p_object);
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		g_object_class_cache[key] = result;
	}
	return result;
}

/* --------------------------------------------------------- field access -- */

Dynamic field_of(const Dynamic &p_object, const char *p_name) {
	if (p_object.mPtr == nullptr) {
		return null();
	}
	// hxcpp only answers property accessors (`var x(get, never)`, ...) when
	// the caller asserts it wants the accessor; with `paccDynamic` it returns
	// null for them. Ordinary fields ignore the mode, so asking for
	// `paccAlways` works for both and needs a single lookup.
	return p_object.mPtr->__Field(String(p_name), HX_PROP_ALWAYS);
}

double number_of(const Dynamic &p_object, const char *p_name) {
	Dynamic value = field_of(p_object, p_name);
	return value.mPtr ? value->__ToDouble() : 0.0;
}

int64_t integer_of(const Dynamic &p_object, const char *p_name) {
	Dynamic value = field_of(p_object, p_name);
	return value.mPtr ? (int64_t)value->__ToInt() : 0;
}

/* Value types expose their components as Haxe properties (so that a write can
 * be stored back into the object the value was read from). hxcpp only calls a
 * property setter with `paccAlways`, hence the mode here: with `paccDynamic`
 * the write would be silently dropped. Ordinary fields ignore the mode. */
void set_number(const Dynamic &p_object, const char *p_name, double p_value) {
	if (p_object.mPtr == nullptr) {
		return;
	}
	p_object.mPtr->__SetField(String(p_name), hx::Val(p_value), HX_PROP_ALWAYS);
}

void set_integer(const Dynamic &p_object, const char *p_name, int64_t p_value) {
	if (p_object.mPtr == nullptr) {
		return;
	}
	p_object.mPtr->__SetField(String(p_name), hx::Val((int)p_value), HX_PROP_ALWAYS);
}

void set_object(const Dynamic &p_object, const char *p_name, const Dynamic &p_value) {
	if (p_object.mPtr == nullptr) {
		return;
	}
	p_object.mPtr->__SetField(String(p_name), hx::Val(p_value), HX_PROP_ALWAYS);
}

Dynamic make_instance(const char *p_class) {
	hx::Class cls = resolve_object_wrapper(p_class);
	if (cls.mPtr == nullptr) {
		return null();
	}
	return cls->ConstructEmpty();
}

Dynamic make_builtin(const char *p_class) {
	hx::Class cls = resolve_class(p_class);
	if (cls.mPtr == nullptr) {
		return null();
	}
	Dynamic value = cls->ConstructEmpty();
	// `ConstructEmpty` skips the Haxe constructor, so every field keeps
	// whatever the collector left in the memory block. That is fine for the
	// fields the caller initializes right after, but the value binding is a
	// pointer: a stale one would be followed on the first component write.
	// Value types declare it as `__hxowner`/`__hxproperty`; asking for the
	// fields on a class that has none is a no-op.
	value.mPtr->__SetField(String("__hxowner"), null(), HX_PROP_DYNAMIC);
	value.mPtr->__SetField(String("__hxproperty"), null(), HX_PROP_DYNAMIC);
	return value;
}

int64_t parse_object_id(const Dynamic &p_value) {
	if (p_value.mPtr == nullptr) {
		return 0;
	}
	return (int64_t)strtoll(p_value.mPtr->toString().utf8_str(), nullptr, 10);
}

/* --------------------------------------------------- closure bookkeeping -- */

void release_closure_now(int64_t p_id) {
	std::lock_guard<std::mutex> lock(g_closure_mutex);
	std::unordered_map<int64_t, ClosureEntry>::iterator it = g_closures.find(p_id);
	if (it == g_closures.end()) {
		return;
	}
	it->second.refs--;
	if (it->second.refs <= 0) {
		g_closures.erase(it);
	}
}

int64_t create_closure(Dynamic p_function, bool p_haxe_owned) {
	std::lock_guard<std::mutex> lock(g_closure_mutex);
	int64_t id = g_next_closure++;
	ClosureEntry entry;
	entry.fn = p_function;
	entry.refs = p_haxe_owned ? 1 : 0;
	g_closures[id] = entry;
	return id;
}

Dynamic lookup_closure(int64_t p_id) {
	std::lock_guard<std::mutex> lock(g_closure_mutex);
	std::unordered_map<int64_t, ClosureEntry>::iterator it = g_closures.find(p_id);
	if (it == g_closures.end()) {
		return null();
	}
	return it->second.fn;
}

bool closure_exists(int64_t p_id) {
	std::lock_guard<std::mutex> lock(g_closure_mutex);
	return g_closures.find(p_id) != g_closures.end();
}

void drain_pending() {
	if (!t_release_closures.empty()) {
		std::vector<int64_t> ids;
		ids.swap(t_release_closures);
		for (int64_t id : ids) {
			release_closure_now(id);
		}
	}
	if (!t_free_callables.empty()) {
		std::vector<int64_t> handles;
		handles.swap(t_free_callables);
		if (g_host.free_callable != nullptr) {
			for (int64_t handle : handles) {
				g_host.free_callable(handle);
			}
		}
	}
	if (!t_unpin_objects.empty()) {
		std::vector<int64_t> objects;
		objects.swap(t_unpin_objects);
		if (g_host.unref_object != nullptr) {
			for (int64_t id : objects) {
				g_host.unref_object(id);
			}
		}
	}
}

void closure_finalizer(Dynamic p_wrapper) {
	std::lock_guard<std::mutex> lock(g_owner_mutex);
	std::unordered_map<hx::Object *, int64_t>::iterator it = g_closure_owners.find(p_wrapper.mPtr);
	if (it != g_closure_owners.end()) {
		t_release_closures.push_back(it->second);
		g_closure_owners.erase(it);
	}
}

void handle_finalizer(Dynamic p_wrapper) {
	std::lock_guard<std::mutex> lock(g_owner_mutex);
	std::unordered_map<hx::Object *, int64_t>::iterator it = g_handle_owners.find(p_wrapper.mPtr);
	if (it != g_handle_owners.end()) {
		t_free_callables.push_back(it->second);
		g_handle_owners.erase(it);
	}
}

void object_finalizer(Dynamic p_wrapper) {
	std::lock_guard<std::mutex> lock(g_owner_mutex);
	std::unordered_map<hx::Object *, int64_t>::iterator it = g_pin_owners.find(p_wrapper.mPtr);
	if (it != g_pin_owners.end()) {
		t_unpin_objects.push_back(it->second);
		g_pin_owners.erase(it);
	}
}

void attach_closure_owner(Dynamic p_wrapper, int64_t p_closure) {
	{
		std::lock_guard<std::mutex> lock(g_owner_mutex);
		g_closure_owners[p_wrapper.mPtr] = p_closure;
	}
	_hx_set_finalizer(p_wrapper, closure_finalizer);
}

void attach_handle_owner(Dynamic p_wrapper, int64_t p_handle) {
	{
		std::lock_guard<std::mutex> lock(g_owner_mutex);
		g_handle_owners[p_wrapper.mPtr] = p_handle;
	}
	_hx_set_finalizer(p_wrapper, handle_finalizer);
}

void attach_pin_owner(Dynamic p_wrapper, int64_t p_object) {
	{
		std::lock_guard<std::mutex> lock(g_owner_mutex);
		g_pin_owners[p_wrapper.mPtr] = p_object;
	}
	_hx_set_finalizer(p_wrapper, object_finalizer);
}

/* ------------------------------------------------------------ encoding -- */

void encode_value(Dynamic p_value, std::vector<uint8_t> &r_blob);

Dynamic builtin_instance(const char *p_class) {
	return make_builtin(p_class);
}

Dynamic make_vector2(double p_x, double p_y) {
	Dynamic value = builtin_instance("godot.Vector2");
	set_number(value, "x", p_x);
	set_number(value, "y", p_y);
	return value;
}

Dynamic make_vector2i(double p_x, double p_y) {
	Dynamic value = builtin_instance("godot.Vector2i");
	set_integer(value, "x", (int64_t)p_x);
	set_integer(value, "y", (int64_t)p_y);
	return value;
}

Dynamic make_vector3(double p_x, double p_y, double p_z) {
	Dynamic value = builtin_instance("godot.Vector3");
	set_number(value, "x", p_x);
	set_number(value, "y", p_y);
	set_number(value, "z", p_z);
	return value;
}

Dynamic make_vector3i(double p_x, double p_y, double p_z) {
	Dynamic value = builtin_instance("godot.Vector3i");
	set_integer(value, "x", (int64_t)p_x);
	set_integer(value, "y", (int64_t)p_y);
	set_integer(value, "z", (int64_t)p_z);
	return value;
}

Dynamic make_vector4(double p_x, double p_y, double p_z, double p_w) {
	Dynamic value = builtin_instance("godot.Vector4");
	set_number(value, "x", p_x);
	set_number(value, "y", p_y);
	set_number(value, "z", p_z);
	set_number(value, "w", p_w);
	return value;
}

Dynamic make_vector4i(double p_x, double p_y, double p_z, double p_w) {
	Dynamic value = builtin_instance("godot.Vector4i");
	set_integer(value, "x", (int64_t)p_x);
	set_integer(value, "y", (int64_t)p_y);
	set_integer(value, "z", (int64_t)p_z);
	set_integer(value, "w", (int64_t)p_w);
	return value;
}

Dynamic make_color(double p_r, double p_g, double p_b, double p_a) {
	Dynamic value = builtin_instance("godot.Color");
	set_number(value, "r", p_r);
	set_number(value, "g", p_g);
	set_number(value, "b", p_b);
	set_number(value, "a", p_a);
	return value;
}

Dynamic make_callable_from_object(int64_t p_object, const String &p_method);
Dynamic make_callable_from_handle(int64_t p_handle);
Dynamic make_callable_from_closure(int64_t p_closure);
Dynamic make_signal(int64_t p_object, const String &p_name);
Dynamic wrap_engine_object(int64_t p_object, bool p_pin, bool p_take_reference);
Dynamic decode_value(Reader &p_reader);

void encode_math(Dynamic p_value, int p_kind, std::vector<uint8_t> &r_blob) {
	put_u8(r_blob, (uint8_t)p_kind);
	switch (p_kind) {
		case HXG_VECTOR2: {
			put_f64(r_blob, number_of(p_value, "x"));
			put_f64(r_blob, number_of(p_value, "y"));
		} break;
		case HXG_VECTOR2I: {
			put_f64(r_blob, (double)integer_of(p_value, "x"));
			put_f64(r_blob, (double)integer_of(p_value, "y"));
		} break;
		case HXG_VECTOR3: {
			put_f64(r_blob, number_of(p_value, "x"));
			put_f64(r_blob, number_of(p_value, "y"));
			put_f64(r_blob, number_of(p_value, "z"));
		} break;
		case HXG_VECTOR3I: {
			put_f64(r_blob, (double)integer_of(p_value, "x"));
			put_f64(r_blob, (double)integer_of(p_value, "y"));
			put_f64(r_blob, (double)integer_of(p_value, "z"));
		} break;
		case HXG_VECTOR4:
		case HXG_QUATERNION: {
			put_f64(r_blob, number_of(p_value, "x"));
			put_f64(r_blob, number_of(p_value, "y"));
			put_f64(r_blob, number_of(p_value, "z"));
			put_f64(r_blob, number_of(p_value, "w"));
		} break;
		case HXG_VECTOR4I: {
			put_f64(r_blob, (double)integer_of(p_value, "x"));
			put_f64(r_blob, (double)integer_of(p_value, "y"));
			put_f64(r_blob, (double)integer_of(p_value, "z"));
			put_f64(r_blob, (double)integer_of(p_value, "w"));
		} break;
		case HXG_RECT2:
		case HXG_RECT2I: {
			Dynamic position = field_of(p_value, "position");
			Dynamic size = field_of(p_value, "size");
			put_f64(r_blob, number_of(position, "x"));
			put_f64(r_blob, number_of(position, "y"));
			put_f64(r_blob, number_of(size, "x"));
			put_f64(r_blob, number_of(size, "y"));
		} break;
		case HXG_PLANE: {
			put_f64(r_blob, number_of(p_value, "x"));
			put_f64(r_blob, number_of(p_value, "y"));
			put_f64(r_blob, number_of(p_value, "z"));
			put_f64(r_blob, number_of(p_value, "d"));
		} break;
		case HXG_AABB: {
			Dynamic position = field_of(p_value, "position");
			Dynamic size = field_of(p_value, "size");
			put_f64(r_blob, number_of(position, "x"));
			put_f64(r_blob, number_of(position, "y"));
			put_f64(r_blob, number_of(position, "z"));
			put_f64(r_blob, number_of(size, "x"));
			put_f64(r_blob, number_of(size, "y"));
			put_f64(r_blob, number_of(size, "z"));
		} break;
		case HXG_BASIS: {
			const char *rows[3] = { "x", "y", "z" };
			for (int row = 0; row < 3; row++) {
				Dynamic row_value = field_of(p_value, rows[row]);
				put_f64(r_blob, number_of(row_value, "x"));
				put_f64(r_blob, number_of(row_value, "y"));
				put_f64(r_blob, number_of(row_value, "z"));
			}
		} break;
		case HXG_TRANSFORM2D: {
			Dynamic x = field_of(p_value, "x");
			Dynamic y = field_of(p_value, "y");
			Dynamic origin = field_of(p_value, "origin");
			put_f64(r_blob, number_of(x, "x"));
			put_f64(r_blob, number_of(x, "y"));
			put_f64(r_blob, number_of(y, "x"));
			put_f64(r_blob, number_of(y, "y"));
			put_f64(r_blob, number_of(origin, "x"));
			put_f64(r_blob, number_of(origin, "y"));
		} break;
		case HXG_TRANSFORM3D: {
			Dynamic basis = field_of(p_value, "basis");
			Dynamic origin = field_of(p_value, "origin");
			const char *rows[3] = { "x", "y", "z" };
			for (int row = 0; row < 3; row++) {
				Dynamic row_value = field_of(basis, rows[row]);
				put_f64(r_blob, number_of(row_value, "x"));
				put_f64(r_blob, number_of(row_value, "y"));
				put_f64(r_blob, number_of(row_value, "z"));
			}
			put_f64(r_blob, number_of(origin, "x"));
			put_f64(r_blob, number_of(origin, "y"));
			put_f64(r_blob, number_of(origin, "z"));
		} break;
		case HXG_PROJECTION: {
			const char *columns[4] = { "x", "y", "z", "w" };
			for (int column = 0; column < 4; column++) {
				Dynamic column_value = field_of(p_value, columns[column]);
				put_f64(r_blob, number_of(column_value, "x"));
				put_f64(r_blob, number_of(column_value, "y"));
				put_f64(r_blob, number_of(column_value, "z"));
				put_f64(r_blob, number_of(column_value, "w"));
			}
		} break;
		case HXG_COLOR: {
			put_f64(r_blob, number_of(p_value, "r"));
			put_f64(r_blob, number_of(p_value, "g"));
			put_f64(r_blob, number_of(p_value, "b"));
			put_f64(r_blob, number_of(p_value, "a"));
		} break;
		case HXG_RID: {
			put_u64(r_blob, (uint64_t)integer_of(p_value, "id"));
		} break;
		default:
			break;
	}
}

void encode_container(Dynamic p_value, int p_kind, std::vector<uint8_t> &r_blob) {
	put_u8(r_blob, (uint8_t)p_kind);
	switch (p_kind) {
		case HXG_ARRAY: {
			Array<Dynamic> array = p_value;
			put_u32(r_blob, (uint32_t)array->length);
			for (int i = 0; i < array->length; i++) {
				encode_value(array->__get(i), r_blob);
			}
		} break;
		case HXG_DICTIONARY: {
			Array<Dynamic> keys = field_of(p_value, "__keys");
			Array<Dynamic> values = field_of(p_value, "__values");
			int count = keys.mPtr ? keys->length : 0;
			put_u32(r_blob, (uint32_t)count);
			for (int i = 0; i < count; i++) {
				encode_value(keys->__get(i), r_blob);
				encode_value(i < values->length ? values->__get(i) : null(), r_blob);
			}
		} break;
		case HXG_PACKED_BYTE_ARRAY:
		case HXG_PACKED_INT32_ARRAY:
		case HXG_PACKED_INT64_ARRAY:
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY:
		case HXG_PACKED_STRING_ARRAY:
		case HXG_PACKED_VECTOR2_ARRAY:
		case HXG_PACKED_VECTOR3_ARRAY:
		case HXG_PACKED_COLOR_ARRAY:
		case HXG_PACKED_VECTOR4_ARRAY: {
			Array<Dynamic> data = field_of(p_value, "__data");
			int count = data.mPtr ? data->length : 0;
			put_u32(r_blob, (uint32_t)count);
			for (int i = 0; i < count; i++) {
				Dynamic item = data->__get(i);
				switch (p_kind) {
					case HXG_PACKED_BYTE_ARRAY:
						put_u8(r_blob, (uint8_t)(item.mPtr ? item->__ToInt() : 0));
						break;
					case HXG_PACKED_INT32_ARRAY:
					case HXG_PACKED_INT64_ARRAY:
						put_u64(r_blob, (uint64_t)(item.mPtr ? (int64_t)item->__ToInt() : 0));
						break;
					case HXG_PACKED_FLOAT32_ARRAY:
					case HXG_PACKED_FLOAT64_ARRAY:
						put_f64(r_blob, item.mPtr ? item->__ToDouble() : 0.0);
						break;
					case HXG_PACKED_STRING_ARRAY:
						put_text(r_blob, item.mPtr ? item->toString() : String());
						break;
					case HXG_PACKED_VECTOR2_ARRAY:
						put_f64(r_blob, number_of(item, "x"));
						put_f64(r_blob, number_of(item, "y"));
						break;
					case HXG_PACKED_VECTOR3_ARRAY:
						put_f64(r_blob, number_of(item, "x"));
						put_f64(r_blob, number_of(item, "y"));
						put_f64(r_blob, number_of(item, "z"));
						break;
					case HXG_PACKED_COLOR_ARRAY:
						put_f64(r_blob, number_of(item, "r"));
						put_f64(r_blob, number_of(item, "g"));
						put_f64(r_blob, number_of(item, "b"));
						put_f64(r_blob, number_of(item, "a"));
						break;
					default:
						put_f64(r_blob, number_of(item, "x"));
						put_f64(r_blob, number_of(item, "y"));
						put_f64(r_blob, number_of(item, "z"));
						put_f64(r_blob, number_of(item, "w"));
						break;
				}
			}
		} break;
		default:
			break;
	}
}

void encode_value(Dynamic p_value, std::vector<uint8_t> &r_blob) {
	if (p_value.mPtr == nullptr) {
		put_u8(r_blob, HXG_NIL);
		return;
	}
	int type = p_value->__GetType();
	if (type == vtBool) {
		put_u8(r_blob, HXG_BOOL);
		put_u8(r_blob, p_value->__ToInt() ? 1 : 0);
		return;
	}
	if (type == vtInt) {
		put_u8(r_blob, HXG_INT);
		put_u64(r_blob, (uint64_t)(int64_t)p_value->__ToInt());
		return;
	}
	if (type == vtFloat) {
		put_u8(r_blob, HXG_FLOAT);
		put_f64(r_blob, p_value->__ToDouble());
		return;
	}
	if (type == vtString) {
		put_u8(r_blob, HXG_STRING);
		put_text(r_blob, p_value->toString());
		return;
	}
	if (type == vtArray) {
		encode_container(p_value, HXG_ARRAY, r_blob);
		return;
	}
	if (type == vtFunction) {
		int64_t id = create_closure(p_value, false);
		put_u8(r_blob, HXG_CLOSURE);
		put_u64(r_blob, (uint64_t)id);
		return;
	}

	int kind = framework_kind(p_value.mPtr);
	switch (kind) {
		case HXG_VECTOR2:
		case HXG_VECTOR2I:
		case HXG_VECTOR3:
		case HXG_VECTOR3I:
		case HXG_VECTOR4:
		case HXG_VECTOR4I:
		case HXG_RECT2:
		case HXG_RECT2I:
		case HXG_PLANE:
		case HXG_QUATERNION:
		case HXG_AABB:
		case HXG_BASIS:
		case HXG_TRANSFORM2D:
		case HXG_TRANSFORM3D:
		case HXG_PROJECTION:
		case HXG_COLOR:
		case HXG_RID:
			encode_math(p_value, kind, r_blob);
			return;
		case HXG_ARRAY:
		case HXG_DICTIONARY:
		case HXG_PACKED_BYTE_ARRAY:
		case HXG_PACKED_INT32_ARRAY:
		case HXG_PACKED_INT64_ARRAY:
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY:
		case HXG_PACKED_STRING_ARRAY:
		case HXG_PACKED_VECTOR2_ARRAY:
		case HXG_PACKED_VECTOR3_ARRAY:
		case HXG_PACKED_COLOR_ARRAY:
		case HXG_PACKED_VECTOR4_ARRAY:
			encode_container(p_value, kind, r_blob);
			return;
		case HXG_CALLABLE: {
			int callable_kind = (int)integer_of(p_value, "kind");
			put_u8(r_blob, HXG_CALLABLE);
			if (callable_kind == 1) {
				r_blob.pop_back();
				put_u8(r_blob, HXG_CALLABLE_HANDLE);
				put_u64(r_blob, (uint64_t)integer_of(p_value, "handle"));
			} else if (callable_kind == 2) {
				r_blob.pop_back();
				put_u8(r_blob, HXG_CLOSURE);
				put_u64(r_blob, (uint64_t)integer_of(p_value, "closureId"));
			} else {
				put_u64(r_blob, (uint64_t)parse_object_id(field_of(p_value, "objectId")));
				Dynamic method = field_of(p_value, "method");
				put_text(r_blob, method.mPtr ? method->toString() : String());
			}
		} break;
		case HXG_SIGNAL: {
			put_u8(r_blob, HXG_SIGNAL);
			put_u64(r_blob, (uint64_t)parse_object_id(field_of(p_value, "objectId")));
			Dynamic name = field_of(p_value, "name");
			put_text(r_blob, name.mPtr ? name->toString() : String());
		} break;
		default: {
			if (is_godot_object(p_value.mPtr)) {
				put_u8(r_blob, HXG_OBJECT);
				put_u64(r_blob, (uint64_t)parse_object_id(field_of(p_value, "__hxgodotId")));
			} else {
				put_u8(r_blob, HXG_NIL);
			}
		} break;
	}
}

/* Converts a Haxe value to the engine-facing representation. Fixed size values
 * are inlined; everything else points into the return arena. */
HxValue to_abi(Dynamic p_value, Arena &r_arena) {
	HxValue value;
	memset(&value, 0, sizeof(value));
	value.type = HXG_NIL;

	if (p_value.mPtr == nullptr) {
		return value;
	}
	int type = p_value->__GetType();
	if (type == vtBool) {
		value.type = HXG_BOOL;
		value.i = p_value->__ToInt() ? 1 : 0;
		return value;
	}
	if (type == vtInt) {
		value.type = HXG_INT;
		value.i = (int64_t)p_value->__ToInt();
		return value;
	}
	if (type == vtFloat) {
		value.type = HXG_FLOAT;
		value.f[0] = p_value->__ToDouble();
		return value;
	}
	if (type == vtString) {
		value.type = HXG_STRING;
		value.s = r_arena.store(p_value->toString());
		return value;
	}
	if (type == vtFunction) {
		value.type = HXG_CLOSURE;
		value.i = create_closure(p_value, false);
		return value;
	}
	if (type == vtArray) {
		std::vector<uint8_t> blob;
		encode_container(p_value, HXG_ARRAY, blob);
		value.type = blob[0];
		value.blob = r_arena.store(blob);
		value.blob_size = (int64_t)blob.size();
		return value;
	}

	int kind = framework_kind(p_value.mPtr);
	switch (kind) {
		case HXG_VECTOR2:
			value.type = HXG_VECTOR2;
			value.f[0] = number_of(p_value, "x");
			value.f[1] = number_of(p_value, "y");
			return value;
		case HXG_VECTOR2I:
			value.type = HXG_VECTOR2I;
			value.f[0] = (double)integer_of(p_value, "x");
			value.f[1] = (double)integer_of(p_value, "y");
			return value;
		case HXG_VECTOR3:
			value.type = HXG_VECTOR3;
			value.f[0] = number_of(p_value, "x");
			value.f[1] = number_of(p_value, "y");
			value.f[2] = number_of(p_value, "z");
			return value;
		case HXG_VECTOR3I:
			value.type = HXG_VECTOR3I;
			value.f[0] = (double)integer_of(p_value, "x");
			value.f[1] = (double)integer_of(p_value, "y");
			value.f[2] = (double)integer_of(p_value, "z");
			return value;
		case HXG_VECTOR4:
			value.type = HXG_VECTOR4;
			value.f[0] = number_of(p_value, "x");
			value.f[1] = number_of(p_value, "y");
			value.f[2] = number_of(p_value, "z");
			value.f[3] = number_of(p_value, "w");
			return value;
		case HXG_VECTOR4I:
			value.type = HXG_VECTOR4I;
			value.f[0] = (double)integer_of(p_value, "x");
			value.f[1] = (double)integer_of(p_value, "y");
			value.f[2] = (double)integer_of(p_value, "z");
			value.f[3] = (double)integer_of(p_value, "w");
			return value;
		case HXG_RECT2:
		case HXG_RECT2I: {
			Dynamic position = field_of(p_value, "position");
			Dynamic size = field_of(p_value, "size");
			value.type = kind;
			value.f[0] = number_of(position, "x");
			value.f[1] = number_of(position, "y");
			value.f[2] = number_of(size, "x");
			value.f[3] = number_of(size, "y");
			return value;
		}
		case HXG_PLANE:
			value.type = HXG_PLANE;
			value.f[0] = number_of(p_value, "x");
			value.f[1] = number_of(p_value, "y");
			value.f[2] = number_of(p_value, "z");
			value.f[3] = number_of(p_value, "d");
			return value;
		case HXG_QUATERNION:
			value.type = HXG_QUATERNION;
			value.f[0] = number_of(p_value, "x");
			value.f[1] = number_of(p_value, "y");
			value.f[2] = number_of(p_value, "z");
			value.f[3] = number_of(p_value, "w");
			return value;
		case HXG_COLOR:
			value.type = HXG_COLOR;
			value.f[0] = number_of(p_value, "r");
			value.f[1] = number_of(p_value, "g");
			value.f[2] = number_of(p_value, "b");
			value.f[3] = number_of(p_value, "a");
			return value;
		case HXG_RID:
			value.type = HXG_RID;
			value.i = integer_of(p_value, "id");
			return value;
		case HXG_CALLABLE: {
			int callable_kind = (int)integer_of(p_value, "kind");
			if (callable_kind == 1) {
				value.type = HXG_CALLABLE_HANDLE;
				value.i = integer_of(p_value, "handle");
			} else if (callable_kind == 2) {
				value.type = HXG_CLOSURE;
				value.i = integer_of(p_value, "closureId");
			} else {
				value.type = HXG_CALLABLE;
				value.i = parse_object_id(field_of(p_value, "objectId"));
				Dynamic method = field_of(p_value, "method");
				value.s = r_arena.store(method.mPtr ? method->toString() : String());
			}
			return value;
		}
		case HXG_SIGNAL: {
			value.type = HXG_SIGNAL;
			value.i = parse_object_id(field_of(p_value, "objectId"));
			Dynamic name = field_of(p_value, "name");
			value.s = r_arena.store(name.mPtr ? name->toString() : String());
			return value;
		}
		case HXG_ARRAY:
		case HXG_DICTIONARY:
		case HXG_PACKED_BYTE_ARRAY:
		case HXG_PACKED_INT32_ARRAY:
		case HXG_PACKED_INT64_ARRAY:
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY:
		case HXG_PACKED_STRING_ARRAY:
		case HXG_PACKED_VECTOR2_ARRAY:
		case HXG_PACKED_VECTOR3_ARRAY:
		case HXG_PACKED_COLOR_ARRAY:
		case HXG_PACKED_VECTOR4_ARRAY:
		case HXG_AABB:
		case HXG_BASIS:
		case HXG_TRANSFORM2D:
		case HXG_TRANSFORM3D:
		case HXG_PROJECTION: {
			std::vector<uint8_t> blob;
			if (kind == HXG_AABB || kind == HXG_BASIS || kind == HXG_TRANSFORM2D || kind == HXG_TRANSFORM3D || kind == HXG_PROJECTION) {
				encode_math(p_value, kind, blob);
			} else {
				encode_container(p_value, kind, blob);
			}
			value.type = blob[0];
			value.blob = r_arena.store(blob);
			value.blob_size = (int64_t)blob.size();
			return value;
		}
		default:
			break;
	}

	if (is_godot_object(p_value.mPtr)) {
		value.type = HXG_OBJECT;
		value.i = parse_object_id(field_of(p_value, "__hxgodotId"));
		return value;
	}
	return value;
}

/* ------------------------------------------------------------ decoding -- */

/* Drops a reference the sender handed over with a value that no wrapper took
 * over, so a value the library could not convert does not pin the object for
 * the rest of the run. */
void release_handed_reference(int64_t p_object, bool p_take_reference) {
	if (p_take_reference && p_object != 0 && g_host.unref_object != nullptr) {
		g_host.unref_object(p_object);
	}
}

/* Wraps an engine object in its generated Haxe class.
 *
 * `p_take_reference` says that the sender already handed over one owned
 * reference for the object (the `owned` flag); it is moved to the wrapper so
 * that losing the wrapper releases the object. Without it the wrapper takes a
 * reference of its own, which is what objects the library created need. */
Dynamic wrap_engine_object(int64_t p_object, bool p_pin, bool p_take_reference) {
	if (p_object == 0) {
		return null();
	}
	char class_name[512] = {};
	if (g_host.get_class_name == nullptr || g_host.get_class_name(p_object, class_name, sizeof(class_name)) != 0) {
		release_handed_reference(p_object, p_take_reference);
		return null();
	}
	/* A GDScript class instance is a plain engine object with a script
	 * attached. When the script has a `class_name`, wrap the object as the
	 * stub class generated for it, so `get_node("x", MyClass)` and instances
	 * returned from a call are typed as `MyClass` and expose its members. */
	char script_name[512] = {};
	if (g_host.utility != nullptr) {
		HxValue name_value = {};
		if (g_host.utility("script_class_name", p_object, nullptr, &name_value) == 0 && name_value.s != nullptr && name_value.s[0] != '\0') {
			snprintf(script_name, sizeof(script_name), "%s", name_value.s);
		}
	}
	hx::Class cls = resolve_object_wrapper(script_name[0] != '\0' ? script_name : class_name);
	if (cls.mPtr == nullptr) {
		release_handed_reference(p_object, p_take_reference);
		return null();
	}
	Dynamic wrapper = cls->ConstructEmpty();
	if (wrapper.mPtr == nullptr) {
		release_handed_reference(p_object, p_take_reference);
		return null();
	}
	wrapper->__SetField(String("__hxgodotId"), hx::Val(String(p_object)), HX_PROP_DYNAMIC);
	if (p_pin && g_host.is_ref_counted != nullptr && g_host.is_ref_counted(p_object)) {
		if (!p_take_reference) {
			g_host.ref_object(p_object);
		}
		attach_pin_owner(wrapper, p_object);
	} else {
		release_handed_reference(p_object, p_take_reference);
	}
	return wrapper;
}

Dynamic make_callable_from_object(int64_t p_object, const String &p_method) {
	hx::Class cls = resolve_class("godot.Callable");
	if (cls.mPtr == nullptr) {
		return null();
	}
	Dynamic callable = cls->ConstructEmpty();
	set_integer(callable, "kind", 0);
	set_object(callable, "objectId", String(p_object));
	set_object(callable, "method", p_method);
	return callable;
}

Dynamic make_callable_from_handle(int64_t p_handle) {
	hx::Class cls = resolve_class("godot.Callable");
	if (cls.mPtr == nullptr) {
		return null();
	}
	Dynamic callable = cls->ConstructEmpty();
	set_integer(callable, "kind", 1);
	set_integer(callable, "handle", p_handle);
	attach_handle_owner(callable, p_handle);
	return callable;
}

Dynamic make_callable_from_closure(int64_t p_closure) {
	hx::Class cls = resolve_class("godot.Callable");
	if (cls.mPtr == nullptr) {
		return null();
	}
	Dynamic callable = cls->ConstructEmpty();
	set_integer(callable, "kind", 2);
	set_integer(callable, "closureId", p_closure);
	attach_closure_owner(callable, p_closure);
	return callable;
}

Dynamic make_signal(int64_t p_object, const String &p_name) {
	hx::Class cls = resolve_class("godot.Signal");
	if (cls.mPtr == nullptr) {
		return null();
	}
	Dynamic signal = cls->ConstructEmpty();
	set_object(signal, "objectId", String(p_object));
	set_object(signal, "name", p_name);
	return signal;
}

Dynamic make_packed(const char *p_class, Array<Dynamic> p_data) {
	Dynamic packed = builtin_instance(p_class);
	set_object(packed, "__data", p_data);
	return packed;
}

Dynamic decode_math(Reader &p_reader, int p_kind) {
	switch (p_kind) {
		case HXG_VECTOR2: {
			double x = 0;
			double y = 0;
			p_reader.read_f64(x);
			p_reader.read_f64(y);
			return make_vector2(x, y);
		}
		case HXG_VECTOR2I: {
			double x = 0;
			double y = 0;
			p_reader.read_f64(x);
			p_reader.read_f64(y);
			return make_vector2i(x, y);
		}
		case HXG_VECTOR3: {
			double x = 0;
			double y = 0;
			double z = 0;
			p_reader.read_f64(x);
			p_reader.read_f64(y);
			p_reader.read_f64(z);
			return make_vector3(x, y, z);
		}
		case HXG_VECTOR3I: {
			double x = 0;
			double y = 0;
			double z = 0;
			p_reader.read_f64(x);
			p_reader.read_f64(y);
			p_reader.read_f64(z);
			return make_vector3i(x, y, z);
		}
		case HXG_VECTOR4:
		case HXG_QUATERNION: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				p_reader.read_f64(values[i]);
			}
			if (p_kind == HXG_QUATERNION) {
				Dynamic value = builtin_instance("godot.Quaternion");
				set_number(value, "x", values[0]);
				set_number(value, "y", values[1]);
				set_number(value, "z", values[2]);
				set_number(value, "w", values[3]);
				return value;
			}
			return make_vector4(values[0], values[1], values[2], values[3]);
		}
		case HXG_VECTOR4I: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				p_reader.read_f64(values[i]);
			}
			return make_vector4i(values[0], values[1], values[2], values[3]);
		}
		case HXG_RECT2:
		case HXG_RECT2I: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				p_reader.read_f64(values[i]);
			}
			const char *cls = p_kind == HXG_RECT2 ? "godot.Rect2" : "godot.Rect2i";
			Dynamic value = builtin_instance(cls);
			set_object(value, "position", p_kind == HXG_RECT2 ? make_vector2(values[0], values[1]) : make_vector2i(values[0], values[1]));
			set_object(value, "size", p_kind == HXG_RECT2 ? make_vector2(values[2], values[3]) : make_vector2i(values[2], values[3]));
			return value;
		}
		case HXG_PLANE: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic value = builtin_instance("godot.Plane");
			set_number(value, "x", values[0]);
			set_number(value, "y", values[1]);
			set_number(value, "z", values[2]);
			set_number(value, "d", values[3]);
			return value;
		}
		case HXG_AABB: {
			double values[6] = {};
			for (int i = 0; i < 6; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic value = builtin_instance("godot.AABB");
			set_object(value, "position", make_vector3(values[0], values[1], values[2]));
			set_object(value, "size", make_vector3(values[3], values[4], values[5]));
			return value;
		}
		case HXG_BASIS: {
			double values[9] = {};
			for (int i = 0; i < 9; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic value = builtin_instance("godot.Basis");
			set_object(value, "x", make_vector3(values[0], values[1], values[2]));
			set_object(value, "y", make_vector3(values[3], values[4], values[5]));
			set_object(value, "z", make_vector3(values[6], values[7], values[8]));
			return value;
		}
		case HXG_TRANSFORM2D: {
			double values[6] = {};
			for (int i = 0; i < 6; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic value = builtin_instance("godot.Transform2D");
			set_object(value, "x", make_vector2(values[0], values[1]));
			set_object(value, "y", make_vector2(values[2], values[3]));
			set_object(value, "origin", make_vector2(values[4], values[5]));
			return value;
		}
		case HXG_TRANSFORM3D: {
			double values[12] = {};
			for (int i = 0; i < 12; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic basis = builtin_instance("godot.Basis");
			set_object(basis, "x", make_vector3(values[0], values[1], values[2]));
			set_object(basis, "y", make_vector3(values[3], values[4], values[5]));
			set_object(basis, "z", make_vector3(values[6], values[7], values[8]));
			Dynamic value = builtin_instance("godot.Transform3D");
			set_object(value, "basis", basis);
			set_object(value, "origin", make_vector3(values[9], values[10], values[11]));
			return value;
		}
		case HXG_PROJECTION: {
			double values[16] = {};
			for (int i = 0; i < 16; i++) {
				p_reader.read_f64(values[i]);
			}
			Dynamic value = builtin_instance("godot.Projection");
			set_object(value, "x", make_vector4(values[0], values[1], values[2], values[3]));
			set_object(value, "y", make_vector4(values[4], values[5], values[6], values[7]));
			set_object(value, "z", make_vector4(values[8], values[9], values[10], values[11]));
			set_object(value, "w", make_vector4(values[12], values[13], values[14], values[15]));
			return value;
		}
		case HXG_COLOR: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				p_reader.read_f64(values[i]);
			}
			return make_color(values[0], values[1], values[2], values[3]);
		}
		case HXG_RID: {
			uint64_t id = 0;
			p_reader.read_u64(id);
			Dynamic value = builtin_instance("godot.RID");
			set_integer(value, "id", (int64_t)id);
			return value;
		}
		default:
			return null();
	}
}

Dynamic decode_container(Reader &p_reader, int p_kind) {
	switch (p_kind) {
		case HXG_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Array_obj<Dynamic>::__new();
			}
			Array<Dynamic> array = Array_obj<Dynamic>::__new();
			for (uint32_t i = 0; i < count; i++) {
				array->push(decode_value(p_reader));
			}
			return array;
		}
		case HXG_DICTIONARY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				count = 0;
			}
			Array<Dynamic> keys = Array_obj<Dynamic>::__new();
			Array<Dynamic> values = Array_obj<Dynamic>::__new();
			for (uint32_t i = 0; i < count; i++) {
				keys->push(decode_value(p_reader));
				values->push(decode_value(p_reader));
			}
			Dynamic dictionary = builtin_instance("godot.Dictionary");
			set_object(dictionary, "__keys", keys);
			set_object(dictionary, "__values", values);
			return dictionary;
		}
		default:
			break;
	}
	uint32_t count = 0;
	if (!p_reader.read_u32(count)) {
		count = 0;
	}
	Array<Dynamic> data = Array_obj<Dynamic>::__new();
	for (uint32_t i = 0; i < count; i++) {
		switch (p_kind) {
			case HXG_PACKED_BYTE_ARRAY:
			case HXG_PACKED_INT32_ARRAY:
			case HXG_PACKED_INT64_ARRAY: {
				uint64_t value = 0;
				p_reader.read_u64(value);
				data->push(Dynamic((int)(int64_t)value));
			} break;
			case HXG_PACKED_FLOAT32_ARRAY:
			case HXG_PACKED_FLOAT64_ARRAY: {
				double value = 0;
				p_reader.read_f64(value);
				data->push(Dynamic(value));
			} break;
			case HXG_PACKED_STRING_ARRAY: {
				String text;
				p_reader.read_text(text);
				data->push(text);
			} break;
			case HXG_PACKED_VECTOR2_ARRAY: {
				double x = 0;
				double y = 0;
				p_reader.read_f64(x);
				p_reader.read_f64(y);
				data->push(make_vector2(x, y));
			} break;
			case HXG_PACKED_VECTOR3_ARRAY: {
				double x = 0;
				double y = 0;
				double z = 0;
				p_reader.read_f64(x);
				p_reader.read_f64(y);
				p_reader.read_f64(z);
				data->push(make_vector3(x, y, z));
			} break;
			case HXG_PACKED_COLOR_ARRAY: {
				double r = 0;
				double g = 0;
				double b = 0;
				double a = 0;
				p_reader.read_f64(r);
				p_reader.read_f64(g);
				p_reader.read_f64(b);
				p_reader.read_f64(a);
				data->push(make_color(r, g, b, a));
			} break;
			case HXG_PACKED_VECTOR4_ARRAY: {
				double x = 0;
				double y = 0;
				double z = 0;
				double w = 0;
				p_reader.read_f64(x);
				p_reader.read_f64(y);
				p_reader.read_f64(z);
				p_reader.read_f64(w);
				data->push(make_vector4(x, y, z, w));
			} break;
			default:
				break;
		}
	}
	switch (p_kind) {
		case HXG_PACKED_BYTE_ARRAY:
			return make_packed("godot.PackedByteArray", data);
		case HXG_PACKED_INT32_ARRAY:
			return make_packed("godot.PackedInt32Array", data);
		case HXG_PACKED_INT64_ARRAY:
			return make_packed("godot.PackedInt64Array", data);
		case HXG_PACKED_FLOAT32_ARRAY:
			return make_packed("godot.PackedFloat32Array", data);
		case HXG_PACKED_FLOAT64_ARRAY:
			return make_packed("godot.PackedFloat64Array", data);
		case HXG_PACKED_STRING_ARRAY:
			return make_packed("godot.PackedStringArray", data);
		case HXG_PACKED_VECTOR2_ARRAY:
			return make_packed("godot.PackedVector2Array", data);
		case HXG_PACKED_VECTOR3_ARRAY:
			return make_packed("godot.PackedVector3Array", data);
		case HXG_PACKED_COLOR_ARRAY:
			return make_packed("godot.PackedColorArray", data);
		case HXG_PACKED_VECTOR4_ARRAY:
			return make_packed("godot.PackedVector4Array", data);
		default:
			return null();
	}
}

Dynamic decode_value(Reader &p_reader) {
	uint8_t tag = 0;
	if (!p_reader.read_u8(tag)) {
		return null();
	}
	switch (tag) {
		case HXG_NIL:
			return null();
		case HXG_BOOL: {
			uint8_t value = 0;
			p_reader.read_u8(value);
			return Dynamic(value != 0);
		}
		case HXG_INT: {
			uint64_t value = 0;
			p_reader.read_u64(value);
			return Dynamic((int)(int64_t)value);
		}
		case HXG_FLOAT: {
			double value = 0;
			p_reader.read_f64(value);
			return Dynamic(value);
		}
		case HXG_STRING:
		case HXG_STRING_NAME:
		case HXG_NODE_PATH: {
			String text;
			p_reader.read_text(text);
			return text;
		}
		case HXG_OBJECT: {
			uint64_t id = 0;
			p_reader.read_u64(id);
			return wrap_engine_object((int64_t)id, true, false);
		}
		case HXG_OBJECT_PINNED: {
			uint64_t id = 0;
			p_reader.read_u64(id);
			return wrap_engine_object((int64_t)id, true, true);
		}
		case HXG_CALLABLE: {
			uint64_t object_id = 0;
			String method;
			p_reader.read_u64(object_id);
			p_reader.read_text(method);
			return make_callable_from_object((int64_t)object_id, method);
		}
		case HXG_CALLABLE_HANDLE: {
			uint64_t handle = 0;
			p_reader.read_u64(handle);
			return make_callable_from_handle((int64_t)handle);
		}
		case HXG_CLOSURE: {
			uint64_t id = 0;
			p_reader.read_u64(id);
			return make_callable_from_closure((int64_t)id);
		}
		case HXG_SIGNAL: {
			uint64_t object_id = 0;
			String name;
			p_reader.read_u64(object_id);
			p_reader.read_text(name);
			return make_signal((int64_t)object_id, name);
		}
		case HXG_ARRAY:
		case HXG_DICTIONARY:
		case HXG_PACKED_BYTE_ARRAY:
		case HXG_PACKED_INT32_ARRAY:
		case HXG_PACKED_INT64_ARRAY:
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY:
		case HXG_PACKED_STRING_ARRAY:
		case HXG_PACKED_VECTOR2_ARRAY:
		case HXG_PACKED_VECTOR3_ARRAY:
		case HXG_PACKED_COLOR_ARRAY:
		case HXG_PACKED_VECTOR4_ARRAY:
			return decode_container(p_reader, tag);
		default:
			return decode_math(p_reader, tag);
	}
}

Dynamic from_abi(const HxValue &p_value) {
	switch (p_value.type) {
		case HXG_NIL:
			return null();
		case HXG_BOOL:
			return Dynamic(p_value.i != 0);
		case HXG_INT:
			return Dynamic((int)p_value.i);
		case HXG_FLOAT:
			return Dynamic(p_value.f[0]);
		case HXG_STRING:
		case HXG_STRING_NAME:
		case HXG_NODE_PATH:
			return String(p_value.s ? p_value.s : "");
		case HXG_VECTOR2:
			return make_vector2(p_value.f[0], p_value.f[1]);
		case HXG_VECTOR2I:
			return make_vector2i(p_value.f[0], p_value.f[1]);
		case HXG_VECTOR3:
			return make_vector3(p_value.f[0], p_value.f[1], p_value.f[2]);
		case HXG_VECTOR3I:
			return make_vector3i(p_value.f[0], p_value.f[1], p_value.f[2]);
		case HXG_VECTOR4:
			return make_vector4(p_value.f[0], p_value.f[1], p_value.f[2], p_value.f[3]);
		case HXG_VECTOR4I:
			return make_vector4i(p_value.f[0], p_value.f[1], p_value.f[2], p_value.f[3]);
		case HXG_RECT2:
		case HXG_RECT2I: {
			Dynamic value = builtin_instance(p_value.type == HXG_RECT2 ? "godot.Rect2" : "godot.Rect2i");
			set_object(value, "position", p_value.type == HXG_RECT2 ? make_vector2(p_value.f[0], p_value.f[1]) : make_vector2i(p_value.f[0], p_value.f[1]));
			set_object(value, "size", p_value.type == HXG_RECT2 ? make_vector2(p_value.f[2], p_value.f[3]) : make_vector2i(p_value.f[2], p_value.f[3]));
			return value;
		}
		case HXG_PLANE: {
			Dynamic value = builtin_instance("godot.Plane");
			set_number(value, "x", p_value.f[0]);
			set_number(value, "y", p_value.f[1]);
			set_number(value, "z", p_value.f[2]);
			set_number(value, "d", p_value.f[3]);
			return value;
		}
		case HXG_QUATERNION: {
			Dynamic value = builtin_instance("godot.Quaternion");
			set_number(value, "x", p_value.f[0]);
			set_number(value, "y", p_value.f[1]);
			set_number(value, "z", p_value.f[2]);
			set_number(value, "w", p_value.f[3]);
			return value;
		}
		case HXG_COLOR:
			return make_color(p_value.f[0], p_value.f[1], p_value.f[2], p_value.f[3]);
		case HXG_RID: {
			Dynamic value = builtin_instance("godot.RID");
			set_integer(value, "id", p_value.i);
			return value;
		}
		case HXG_OBJECT:
			return wrap_engine_object(p_value.i, true, p_value.owned != 0);
		case HXG_OBJECT_PINNED:
			return wrap_engine_object(p_value.i, true, true);
		case HXG_CALLABLE:
			return make_callable_from_object(p_value.i, String(p_value.s ? p_value.s : ""));
		case HXG_CALLABLE_HANDLE:
			return make_callable_from_handle(p_value.i);
		case HXG_CLOSURE:
			return make_callable_from_closure(p_value.i);
		case HXG_SIGNAL:
			return make_signal(p_value.i, String(p_value.s ? p_value.s : ""));
		default: {
			if (p_value.blob == nullptr || p_value.blob_size <= 0) {
				return null();
			}
			Reader reader(p_value.blob, p_value.blob_size);
			return decode_value(reader);
		}
	}
}

/* Builds the HxValue list for a host call. */
struct ArgumentList {
	std::vector<HxValue> values;
	Arena arena;

	explicit ArgumentList(Array<Dynamic> p_args) {
		int count = p_args.mPtr ? p_args->length : 0;
		values.resize(count);
		for (int i = 0; i < count; i++) {
			values[i] = to_abi(p_args->__get(i), arena);
		}
	}
};

/* Turns a host status code into a visible diagnostic. Bindings ignore return
 * values, so failures must never stay silent. */
void report_status(const char *p_operation, const char *p_name, int p_status) {
	if (p_status == 0 || p_status == 1) {
		/* 1 means "no such member"; generated bindings and Haxe code can
		 * legitimately probe for optional members. */
		return;
	}
	if (g_host.log == nullptr) {
		return;
	}
	std::string message = "Haxe: ";
	message += p_operation;
	if (p_name != nullptr) {
		message += " ";
		message += p_name;
	}
	message += p_status == 3 ? " failed: the object no longer exists" : " failed";
	if (!g_last_error.empty()) {
		message += ": ";
		message += g_last_error;
	}
	g_host.log(2, message.c_str());
}

int call_host(int (*p_function)(int64_t, const char *, const HxValue *, int, HxValue *), int64_t p_object, const char *p_method, Array<Dynamic> p_args, Dynamic &r_result) {
	if (p_function == nullptr) {
		set_error("host API does not provide this operation");
		return -1;
	}
	ArgumentList arguments(p_args);
	HxValue result = {};
	int status = p_function(p_object, p_method, arguments.values.empty() ? nullptr : arguments.values.data(), (int)arguments.values.size(), &result);
	if (status != 0) {
		report_status("call", p_method, status);
		return status;
	}
	r_result = from_abi(result);
	return 0;
}

/* Creates an engine object and returns its instance id as text. The caller
 * binds the returned id to the Haxe wrapper with hxgodot_haxe_bind_wrapper(). */
String create_engine_object(const String &p_class) {
	if (g_host.create_object == nullptr) {
		return String();
	}
	int64_t id = 0;
	if (g_host.create_object(p_class.utf8_str(), &id) != 0 || id == 0) {
		set_error(String("cannot instantiate engine class ") + p_class);
		if (g_host.log != nullptr) {
			g_host.log(2, g_last_error.c_str());
		}
		return String();
	}
	return String(id);
}

void bind_object_wrapper(Dynamic p_wrapper, int64_t p_object) {
	if (p_wrapper.mPtr == nullptr || p_object == 0) {
		return;
	}
	set_object(p_wrapper, "__hxgodotId", String(p_object));
	if (g_host.is_ref_counted != nullptr && g_host.is_ref_counted(p_object)) {
		g_host.ref_object(p_object);
		attach_pin_owner(p_wrapper, p_object);
	}
}

} // namespace

/* --------------------------------------------------------------- exports -- */

extern "C" HXGODOT_EXPORT int hxgodot_install(const HxHostApi *p_api) {
	if (p_api == nullptr || p_api->version != HXGODOT_ABI_VERSION) {
		set_error("incompatible host API version");
		return -1;
	}
	g_host = *p_api;
	return 0;
}

extern "C" HXGODOT_EXPORT int hxgodot_start(void) {
	if (g_booted) {
		return 0;
	}
	try {
		/* hxcpp canonical shared library entry point: sets up the GC and the
		 * stack context, boots the runtime and registers every generated
		 * class. Calling hx::Boot() directly crashes because the GC has not
		 * been initialized yet. */
		__hxcpp_lib_main();
		g_booted = true;
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return -1;
	} catch (...) {
		set_error("hxcpp runtime boot failed");
		return -1;
	}
}

extern "C" HXGODOT_EXPORT const char *hxgodot_last_error(void) {
	return g_last_error.c_str();
}

extern "C" HXGODOT_EXPORT const char *hxgodot_last_stack_trace(void) {
	return g_last_trace.c_str();
}

extern "C" HXGODOT_EXPORT const char *hxgodot_library_info(void) {
	static std::string info;
	if (info.empty()) {
		info = "abi=";
		info += std::to_string(HXGODOT_ABI_VERSION);
		info += "\nruntime=hxcpp\nbuilt=" __DATE__ " " __TIME__ "\n";
	}
	return info.c_str();
}

extern "C" HXGODOT_EXPORT int hxgodot_has_class(const char *p_class_name) {
	StackTopScope stack_top;
	drain_pending();
	if (!g_booted || p_class_name == nullptr) {
		return 0;
	}
	return hx::Class_obj::Resolve(String(p_class_name)).mPtr != nullptr ? 1 : 0;
}

extern "C" HXGODOT_EXPORT int hxgodot_create(int64_t p_object_id, const char *p_class_name) {
	StackTopScope stack_top;
	drain_pending();
	if (!g_booted) {
		set_error("hxgodot_start() was not called");
		return -1;
	}
	if (p_class_name == nullptr) {
		set_error("missing class name");
		return -1;
	}
	try {
		hx::Class cls = hx::Class_obj::Resolve(String(p_class_name));
		if (cls.mPtr == nullptr) {
			set_error(String("unknown Haxe class: ") + String(p_class_name));
			return -1;
		}
		Array<Dynamic> arguments = Array_obj<Dynamic>::__new();
		/* Save and restore instead of clearing: writing the field of a script
		 * instance can create another one (a Haxe constructor that attaches a
		 * script), and the outer construction must resume with its own id. */
		struct CreationFlag {
			int64_t previous_id;
			void *previous_class;
			bool previous_creating;
			CreationFlag(int64_t p_id, void *p_class)
					: previous_id(t_script_object_id), previous_class(t_script_class), previous_creating(t_creating_script_instance) {
				t_script_object_id = p_id;
				t_script_class = p_class;
				t_creating_script_instance = true;
			}
			~CreationFlag() {
				t_script_object_id = previous_id;
				t_script_class = previous_class;
				t_creating_script_instance = previous_creating;
			}
		} creation_flag(p_object_id, cls.mPtr);
		Dynamic instance = cls->ConstructArgs(arguments);
		if (!instance.mPtr) {
			set_error(String("could not instantiate ") + String(p_class_name));
			return -1;
		}
		instance->__SetField(String("__hxgodotId"), hx::Val(String(p_object_id)), HX_PROP_DYNAMIC);

		/* The GC root must live inside the map entry: the address passed to
		 * GCAddRoot is read by the collector on every collection, so it has to
		 * stay valid until GCRemoveRoot. std::unordered_map keeps the address
		 * of an element stable across rehashes. */
		{
			std::lock_guard<std::mutex> lock(g_state_mutex);
			Instance &record = g_instances[p_object_id];
			record.value = instance;
			record.root = instance.mPtr;
			hx::GCAddRoot(&record.root);
		}

		/* Haxe constructors are the equivalent of GDScript's _init. */
		Dynamic init = field_of(instance, "_init");
		if (init.mPtr != nullptr && init->__GetType() == vtFunction) {
			Array<Dynamic> empty = Array_obj<Dynamic>::__new();
			init->__Run(empty);
		}
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return -1;
	} catch (...) {
		set_error("unknown exception in constructor");
		return -1;
	}
}

extern "C" HXGODOT_EXPORT int hxgodot_has_method(int64_t p_object_id, const char *p_method) {
	StackTopScope stack_top;
	Instance *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
		if (it != g_instances.end()) {
			instance = &it->second;
		}
	}
	if (instance == nullptr || p_method == nullptr) {
		return 0;
	}
	Dynamic member = field_of(instance->value, p_method);
	return member.mPtr != nullptr && member->__GetType() == vtFunction ? 1 : 0;
}

extern "C" HXGODOT_EXPORT int hxgodot_invoke(int64_t p_object_id, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	StackTopScope stack_top;
	drain_pending();
	Instance *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
		if (it != g_instances.end()) {
			instance = &it->second;
		}
	}
	if (instance == nullptr) {
		return 1;
	}
	if (p_method == nullptr) {
		set_error("missing method name");
		return 2;
	}
	try {
		Dynamic function = field_of(instance->value, p_method);
		if (function.mPtr == nullptr) {
			return 1;
		}
		if (function->__GetType() != vtFunction) {
			set_error(String("member is not a method: ") + String(p_method));
			return 2;
		}
		Array<Dynamic> arguments = Array_obj<Dynamic>::__new();
		for (int i = 0; i < p_argc; i++) {
			arguments->push(from_abi(p_args[i]));
		}
		Dynamic result = function->__Run(arguments);
		if (r_ret != nullptr) {
			*r_ret = to_abi(result, t_arena);
		}
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return 2;
	} catch (...) {
		set_error("unknown exception in method");
		return 2;
	}
}

extern "C" HXGODOT_EXPORT int hxgodot_get_field(int64_t p_object_id, const char *p_name, HxValue *r_ret) {
	StackTopScope stack_top;
	drain_pending();
	Instance *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
		if (it != g_instances.end()) {
			instance = &it->second;
		}
	}
	if (instance == nullptr) {
		return 1;
	}
	if (p_name == nullptr) {
		set_error("missing member name");
		return 2;
	}
	try {
		Dynamic member = field_of(instance->value, p_name);
		if (member.mPtr == nullptr) {
			return 1;
		}
		if (r_ret != nullptr) {
			*r_ret = to_abi(member, t_arena);
		}
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return 2;
	} catch (...) {
		set_error("unknown exception while reading a member");
		return 2;
	}
}

extern "C" HXGODOT_EXPORT int hxgodot_set_field(int64_t p_object_id, const char *p_name, const HxValue *p_value) {
	StackTopScope stack_top;
	drain_pending();
	Instance *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
		if (it != g_instances.end()) {
			instance = &it->second;
		}
	}
	if (instance == nullptr) {
		return 1;
	}
	if (p_name == nullptr || p_value == nullptr) {
		set_error("missing member name or value");
		return 2;
	}
	try {
		Dynamic member = field_of(instance->value, p_name);
		if (member.mPtr == nullptr) {
			return 1;
		}
		instance->value->__SetField(String(p_name), hx::Val(from_abi(*p_value)), HX_PROP_ALWAYS);
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return 2;
	} catch (...) {
		set_error("unknown exception while writing a member");
		return 2;
	}
}

extern "C" HXGODOT_EXPORT int hxgodot_get_members(int64_t p_object_id, char *r_out, int p_out_size) {
	StackTopScope stack_top;
	Instance *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
		if (it != g_instances.end()) {
			instance = &it->second;
		}
	}
	if (instance == nullptr) {
		return 0;
	}
	Array<String> fields = instance->value->__GetClass()->GetInstanceFields();
	std::string joined;
	for (int i = 0; i < fields->length; i++) {
		if (i > 0) {
			joined += "\n";
		}
		joined += fields->__get(i).utf8_str();
	}
	if (r_out != nullptr && p_out_size > 0) {
		int copy = (int)joined.size();
		if (copy > p_out_size - 1) {
			copy = p_out_size - 1;
		}
		memcpy(r_out, joined.data(), copy);
		r_out[copy] = 0;
	}
	return (int)joined.size() + 1;
}

extern "C" HXGODOT_EXPORT void hxgodot_destroy(int64_t p_object_id) {
	StackTopScope stack_top;
	std::lock_guard<std::mutex> lock(g_state_mutex);
	std::unordered_map<int64_t, Instance>::iterator it = g_instances.find(p_object_id);
	if (it == g_instances.end()) {
		return;
	}
	hx::GCRemoveRoot(&it->second.root);
	g_instances.erase(it);
}

extern "C" HXGODOT_EXPORT void hxgodot_release_return(void) {
	StackTopScope stack_top;
	t_arena.clear();
}

extern "C" HXGODOT_EXPORT int hxgodot_invoke_closure(int64_t p_closure, const HxValue *p_args, int p_argc, HxValue *r_ret) {
	StackTopScope stack_top;
	drain_pending();
	Dynamic function = lookup_closure(p_closure);
	if (function.mPtr == nullptr) {
		set_error("unknown Haxe closure");
		return 1;
	}
	try {
		Array<Dynamic> arguments = Array_obj<Dynamic>::__new();
		for (int i = 0; i < p_argc; i++) {
			arguments->push(from_abi(p_args[i]));
		}
		Dynamic result = function->__Run(arguments);
		if (r_ret != nullptr) {
			*r_ret = to_abi(result, t_arena);
		}
		return 0;
	} catch (Dynamic error) {
		capture_exception(error);
		return 2;
	} catch (...) {
		set_error("unknown exception in closure");
		return 2;
	}
}

extern "C" HXGODOT_EXPORT void hxgodot_retain_closure(int64_t p_closure) {
	StackTopScope stack_top;
	std::lock_guard<std::mutex> lock(g_closure_mutex);
	std::unordered_map<int64_t, ClosureEntry>::iterator it = g_closures.find(p_closure);
	if (it != g_closures.end()) {
		it->second.refs++;
	}
}

extern "C" HXGODOT_EXPORT void hxgodot_release_closure(int64_t p_closure) {
	StackTopScope stack_top;
	release_closure_now(p_closure);
}

/* ----------------------------------------------- Haxe-facing entry points -- */

Dynamic hxgodot_haxe_call(String p_object_id, String p_method, Array<Dynamic> p_args) {
	drain_pending();
	Dynamic result = null();
	call_host(g_host.call, (int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), p_method.utf8_str(), p_args, result);
	return result;
}

Dynamic hxgodot_haxe_call_static(String p_class, String p_method, Array<Dynamic> p_args) {
	drain_pending();
	if (g_host.call_static == nullptr) {
		return null();
	}
	ArgumentList arguments(p_args);
	HxValue result = {};
	if (g_host.call_static(p_class.utf8_str(), p_method.utf8_str(), arguments.values.empty() ? nullptr : arguments.values.data(), (int)arguments.values.size(), &result) != 0) {
		return null();
	}
	return from_abi(result);
}

Dynamic hxgodot_haxe_get_property(String p_object_id, String p_name) {
	drain_pending();
	if (g_host.get_property == nullptr) {
		return null();
	}
	HxValue result = {};
	if (g_host.get_property((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), p_name.utf8_str(), &result) != 0) {
		return null();
	}
	return from_abi(result);
}

void hxgodot_haxe_set_property(String p_object_id, String p_name, Dynamic p_value) {
	drain_pending();
	if (g_host.set_property == nullptr) {
		return;
	}
	Arena arena;
	HxValue value = to_abi(p_value, arena);
	g_host.set_property((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), p_name.utf8_str(), &value);
}

bool hxgodot_haxe_has_method(String p_object_id, String p_method) {
	if (g_host.has_method == nullptr) {
		return false;
	}
	return g_host.has_method((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), p_method.utf8_str()) != 0;
}

bool hxgodot_haxe_has_property(String p_object_id, String p_name) {
	if (g_host.has_property == nullptr) {
		return false;
	}
	return g_host.has_property((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), p_name.utf8_str()) != 0;
}

Dynamic hxgodot_haxe_variant_call(Dynamic p_target, String p_method, Array<Dynamic> p_args) {
	drain_pending();
	if (g_host.variant_call == nullptr) {
		return null();
	}
	Arena arena;
	HxValue target = to_abi(p_target, arena);
	ArgumentList arguments(p_args);
	HxValue result = {};
	if (g_host.variant_call(&target, p_method.utf8_str(), arguments.values.empty() ? nullptr : arguments.values.data(), (int)arguments.values.size(), &result) != 0) {
		return null();
	}
	return from_abi(result);
}

Dynamic hxgodot_haxe_variant_call_static(String p_type, String p_method, Array<Dynamic> p_args) {
	drain_pending();
	if (g_host.variant_call_static == nullptr) {
		return null();
	}
	ArgumentList arguments(p_args);
	HxValue result = {};
	if (g_host.variant_call_static(p_type.utf8_str(), p_method.utf8_str(), arguments.values.empty() ? nullptr : arguments.values.data(), (int)arguments.values.size(), &result) != 0) {
		return null();
	}
	return from_abi(result);
}

/* Only used if the host is too old to provide Variant comparison. Hosts are
 * version checked, so this is a defensive identity comparison. */
static bool value_identity_equal(Dynamic p_a, Dynamic p_b) {
	if (p_a.mPtr == nullptr || p_b.mPtr == nullptr) {
		return p_a.mPtr == p_b.mPtr;
	}
	return p_a.mPtr == p_b.mPtr;
}

bool hxgodot_haxe_value_equal(Dynamic p_a, Dynamic p_b) {
	drain_pending();
	if (g_host.value_equal == nullptr) {
		return value_identity_equal(p_a, p_b);
	}
	Arena left_arena;
	Arena right_arena;
	HxValue left = to_abi(p_a, left_arena);
	HxValue right = to_abi(p_b, right_arena);
	int equal = 0;
	if (g_host.value_equal(&left, &right, &equal) != 0) {
		return value_identity_equal(p_a, p_b);
	}
	return equal != 0;
}

int hxgodot_haxe_value_compare(Dynamic p_a, Dynamic p_b) {
	drain_pending();
	if (g_host.value_compare == nullptr) {
		return 0;
	}
	Arena left_arena;
	Arena right_arena;
	HxValue left = to_abi(p_a, left_arena);
	HxValue right = to_abi(p_b, right_arena);
	int comparison = 0;
	if (g_host.value_compare(&left, &right, &comparison) != 0) {
		return 0;
	}
	return comparison;
}

/* Copies every instance field of a freshly decoded built-in value back into
 * the wrapper the Haxe program holds. Only value types reach this: engine
 * object wrappers keep their identity in `__hxgodotId`. */
void copy_value_fields(Dynamic p_target, Dynamic p_source) {
	if (p_target.mPtr == nullptr || p_source.mPtr == nullptr) {
		return;
	}
	Array<String> fields = p_source->__GetClass()->GetInstanceFields();
	for (int i = 0; i < fields->length; i++) {
		String field = fields->__get(i);
		if (field == String("__hxgodotId")) {
			continue;
		}
		p_target->__SetField(field, p_source->__Field(field, HX_PROP_DYNAMIC), HX_PROP_DYNAMIC);
	}
}

Dynamic hxgodot_haxe_variant_call_mutating(Dynamic p_target, String p_method, Array<Dynamic> p_args) {
	drain_pending();
	if (g_host.variant_call_mutating == nullptr || p_target.mPtr == nullptr) {
		return null();
	}
	ArgumentList arguments(p_args);
	Arena target_arena;
	HxValue target = to_abi(p_target, target_arena);
	HxValue result = {};
	HxValue updated = {};
	int status = g_host.variant_call_mutating(&target, p_method.utf8_str(),
			arguments.values.empty() ? nullptr : arguments.values.data(), (int)arguments.values.size(),
			&result, &updated);
	if (status != 0) {
		report_status("call", p_method.utf8_str(), status);
		return null();
	}
	copy_value_fields(p_target, from_abi(updated));
	return from_abi(result);
}

Dynamic hxgodot_haxe_get_member(Dynamic p_target, String p_name) {
	drain_pending();
	if (g_host.variant_get_member == nullptr) {
		return null();
	}
	Arena arena;
	HxValue target = to_abi(p_target, arena);
	HxValue result = {};
	if (g_host.variant_get_member(&target, p_name.utf8_str(), &result) != 0) {
		return null();
	}
	return from_abi(result);
}

void hxgodot_haxe_set_member(Dynamic p_target, String p_name, Dynamic p_value) {
	drain_pending();
	if (g_host.variant_set_member == nullptr || p_target.mPtr == nullptr) {
		return;
	}
	Arena target_arena;
	Arena value_arena;
	HxValue target = to_abi(p_target, target_arena);
	HxValue value = to_abi(p_value, value_arena);
	HxValue updated = {};
	if (g_host.variant_set_member(&target, p_name.utf8_str(), &value, &updated) != 0) {
		return;
	}
	copy_value_fields(p_target, from_abi(updated));
}

Dynamic hxgodot_haxe_create_object(String p_class) {
	drain_pending();
	String id = create_engine_object(p_class);
	if (id.length == 0) {
		return null();
	}
	return wrap_engine_object((int64_t)strtoll(id.utf8_str(), nullptr, 10), true, false);
}

/* Used by generated `new()` before it creates anything: returns the id of the
 * engine object the current script instance is being constructed for, or an
 * empty string when this wrapper is not that object. The wrapper's runtime
 * class has to match the class hxgodot_create() is constructing, so field
 * initializers that build unrelated engine objects never steal the id. */
String hxgodot_haxe_script_self_id(Dynamic p_wrapper) {
	if (!t_creating_script_instance || p_wrapper.mPtr == nullptr ||
			t_script_object_id == 0 || t_script_class == nullptr) {
		return String();
	}
	if ((void *)p_wrapper.mPtr->__GetClass().mPtr != t_script_class) {
		return String();
	}
	return String(t_script_object_id);
}

/* Used by generated `new()`: creates the engine object and returns its id, or
 * an empty string when the object is materialised by the engine instead
 * (script instances). The wrapper is bound by hxgodot_haxe_bind_wrapper(). */
String hxgodot_haxe_new_object(String p_class) {
	drain_pending();
	if (t_creating_script_instance) {
		return String();
	}
	return create_engine_object(p_class);
}

void hxgodot_haxe_bind_wrapper(Dynamic p_wrapper, String p_object_id) {
	bind_object_wrapper(p_wrapper, (int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10));
}

bool hxgodot_haxe_in_script_creation() {
	return t_creating_script_instance;
}

Dynamic hxgodot_haxe_call_closure(int p_closure, Array<Dynamic> p_args) {
	drain_pending();
	Dynamic function = lookup_closure(p_closure);
	if (function.mPtr == nullptr) {
		set_error("unknown Haxe closure");
		return null();
	}
	int count = p_args.mPtr ? p_args->length : 0;
	Array<Dynamic> arguments = Array_obj<Dynamic>::__new();
	for (int i = 0; i < count; i++) {
		arguments->push(p_args->__get(i));
	}
	try {
		return function->__Run(arguments);
	} catch (Dynamic error) {
		capture_exception(error);
		if (g_host.log != nullptr) {
			g_host.log(2, g_last_error.c_str());
		}
		return null();
	}
}

Dynamic hxgodot_haxe_singleton(String p_name) {
	drain_pending();
	if (g_host.get_singleton == nullptr) {
		return null();
	}
	int64_t id = 0;
	if (g_host.get_singleton(p_name.utf8_str(), &id) != 0 || id == 0) {
		return null();
	}
	return wrap_engine_object(id, true, false);
}

bool hxgodot_haxe_is_valid(String p_object_id) {
	if (g_host.is_instance_valid == nullptr) {
		return false;
	}
	return g_host.is_instance_valid((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10)) != 0;
}

String hxgodot_haxe_class_name(String p_object_id) {
	if (g_host.get_class_name == nullptr) {
		return String();
	}
	char buffer[512] = {};
	if (g_host.get_class_name((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10), buffer, sizeof(buffer)) != 0) {
		return String();
	}
	return String::create(buffer);
}

bool hxgodot_haxe_is_ref_counted(String p_object_id) {
	if (g_host.is_ref_counted == nullptr) {
		return false;
	}
	return g_host.is_ref_counted((int64_t)strtoll(p_object_id.utf8_str(), nullptr, 10)) != 0;
}

bool hxgodot_haxe_update_texture(String p_texture_id, const void *p_pixels, int p_size, int p_width, int p_height, int p_format) {
	drain_pending();
	if (g_host.update_texture == nullptr || p_pixels == nullptr) {
		return false;
	}
	return g_host.update_texture((int64_t)strtoll(p_texture_id.utf8_str(), nullptr, 10), p_pixels, (int64_t)p_size, p_width, p_height, p_format) == 0;
}

int hxgodot_haxe_push_audio_frames(String p_playback_id, const void *p_samples, int p_frames, int p_channels) {
	drain_pending();
	if (g_host.push_audio_frames == nullptr || p_samples == nullptr) {
		return -1;
	}
	return g_host.push_audio_frames((int64_t)strtoll(p_playback_id.utf8_str(), nullptr, 10), p_samples, p_frames, p_channels);
}

int hxgodot_haxe_store_closure(Dynamic p_function) {
	return (int)create_closure(p_function, true);
}

void hxgodot_haxe_attach_closure_finalizer(Dynamic p_wrapper, int p_closure) {
	attach_closure_owner(p_wrapper, p_closure);
}

void hxgodot_haxe_attach_handle_finalizer(Dynamic p_wrapper, int p_handle) {
	attach_handle_owner(p_wrapper, p_handle);
}

void hxgodot_haxe_log(int p_level, String p_message) {
	if (g_host.log != nullptr) {
		g_host.log(p_level, p_message.utf8_str());
	}
}

String hxgodot_haxe_engine_version() {
	if (g_host.utility == nullptr) {
		return String();
	}
	HxValue result = {};
	if (g_host.utility("engine_version", 0, nullptr, &result) != 0) {
		return String();
	}
	return result.s ? String::create(result.s) : String();
}

String hxgodot_haxe_type_name(Dynamic p_value) {
	if (p_value.mPtr == nullptr) {
		return String("null");
	}
	hx::Class cls = p_value->__GetClass();
	return cls.mPtr ? cls->mName : String();
}
