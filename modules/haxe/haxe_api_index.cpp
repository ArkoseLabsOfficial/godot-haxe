/**************************************************************************/
/*  haxe_api_index.cpp                                                    */
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

#include "haxe_api_index.h"

#include "haxe_runtime.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/templates/hash_set.h"

HaxeApiIndex *HaxeApiIndex::singleton = nullptr;

HaxeApiIndex *HaxeApiIndex::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(HaxeApiIndex);
	}
	return singleton;
}

bool HaxeApiIndex::load() {
	error = String();
	version = String();
	classes.clear();
	global_enums.clear();
	bases.clear();
	class_names.clear();
	member_cache.clear();
	inherited_cache.clear();

	String path = HaxeRuntime::get_singleton()->get_bindings_dir();
	if (path.is_empty()) {
		error = "Haxe support files are missing from this Godot build.";
		return false;
	}
	path = path.path_join("api.json");
	if (!FileAccess::exists(path)) {
		error = vformat("Binding metadata is missing at %s. Rebuild the engine with the Haxe module, or run dist/tools/generate_bindings.py.", path);
		return false;
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		error = vformat("Cannot read %s.", path);
		return false;
	}
	JSON json;
	Error parse_error = json.parse(file->get_as_utf8_string());
	if (parse_error != OK) {
		error = vformat("%s is not valid JSON: %s", path, json.get_error_message());
		return false;
	}
	Dictionary root = json.get_data();
	version = root.get("version", String());
	classes = root.get("classes", Dictionary());
	global_enums = root.get("global_enums", Dictionary());

	for (const KeyValue<Variant, Variant> &entry : classes) {
		String name = entry.key;
		class_names.push_back(name);
		Dictionary info = entry.value;
		String base = info.get("base", String());
		if (!base.is_empty()) {
			bases[name] = base;
		}
	}
	class_names.sort();
	return true;
}

bool HaxeApiIndex::ensure_loaded() {
	MutexLock lock(mutex);
	if (loaded) {
		return true;
	}
	if (failed) {
		return false;
	}
	if (!load()) {
		failed = true;
		return false;
	}
	loaded = true;
	return true;
}

String HaxeApiIndex::get_error() const {
	MutexLock lock(mutex);
	return error;
}

String HaxeApiIndex::get_version() const {
	MutexLock lock(mutex);
	return version;
}

bool HaxeApiIndex::has_class(const String &p_name) {
	if (!ensure_loaded()) {
		return false;
	}
	MutexLock lock(mutex);
	return classes.has(p_name);
}

String HaxeApiIndex::get_base(const String &p_name) {
	if (!ensure_loaded()) {
		return String();
	}
	MutexLock lock(mutex);
	return bases.has(p_name) ? bases[p_name] : String();
}

Vector<String> HaxeApiIndex::get_class_names() {
	if (!ensure_loaded()) {
		return Vector<String>();
	}
	MutexLock lock(mutex);
	return class_names;
}

Dictionary HaxeApiIndex::get_class(const String &p_name) {
	if (!ensure_loaded() || !has_class(p_name)) {
		return Dictionary();
	}
	MutexLock lock(mutex);
	return classes[p_name];
}

Array HaxeApiIndex::get_declared_members(const String &p_name) {
	Array result;
	if (!ensure_loaded()) {
		return result;
	}
	Dictionary info;
	{
		MutexLock lock(mutex);
		if (!classes.has(p_name)) {
			return result;
		}
		info = classes[p_name];
	}

	for (const Variant &item : Array(info.get("properties", Array()))) {
		Array property = item;
		Dictionary member;
		member["name"] = property.size() > 0 ? property[0] : Variant();
		member["kind"] = "property";
		member["type"] = property.size() > 1 ? property[1] : Variant();
		member["writable"] = property.size() > 2 ? property[2] : Variant();
		member["owner"] = p_name;
		result.push_back(member);
	}
	for (const String &key : { String("methods"), String("static_methods") }) {
		for (const Variant &item : Array(info.get(key, Array()))) {
			Array method = item;
			Dictionary member;
			member["name"] = method.size() > 0 ? method[0] : Variant();
			member["args"] = method.size() > 1 ? method[1] : Variant(Array());
			member["type"] = method.size() > 2 ? method[2] : Variant();
			member["static"] = method.size() > 3 ? method[3] : Variant();
			member["virtual"] = method.size() > 4 ? method[4] : Variant();
			member["vararg"] = method.size() > 5 ? method[5] : Variant();
			member["kind"] = (member["static"].operator bool()) ? "static_method" : "method";
			member["owner"] = p_name;
			result.push_back(member);
		}
	}
	for (const Variant &item : Array(info.get("constants", Array()))) {
		Array constant = item;
		Dictionary member;
		member["name"] = constant.size() > 0 ? constant[0] : Variant();
		member["kind"] = "constant";
		member["type"] = constant.size() > 1 ? constant[1] : Variant();
		member["static"] = true;
		member["owner"] = p_name;
		result.push_back(member);
	}
	for (const Variant &item : Array(info.get("signals", Array()))) {
		Array signal = item;
		Dictionary member;
		member["name"] = signal.size() > 0 ? signal[0] : Variant();
		member["kind"] = "signal";
		member["args"] = signal.size() > 1 ? signal[1] : Variant(Array());
		member["type"] = "Signal";
		member["owner"] = p_name;
		result.push_back(member);
	}
	// A singleton class is not constructed with `new`: the generated bindings
	// expose both `instance()` and the `singleton` property, both resolving the
	// engine-owned object through `Native.singleton`, so they are offered as
	// static members here.
	const String singleton_name = info.get("singleton", String());
	if (!singleton_name.is_empty()) {
		Dictionary instance_member;
		instance_member["name"] = "instance";
		instance_member["kind"] = "static_method";
		instance_member["static"] = true;
		instance_member["type"] = p_name;
		instance_member["args"] = Array();
		instance_member["owner"] = p_name;
		result.push_back(instance_member);

		Dictionary singleton_member;
		singleton_member["name"] = "singleton";
		singleton_member["kind"] = "static_property";
		singleton_member["static"] = true;
		singleton_member["type"] = p_name;
		singleton_member["owner"] = p_name;
		result.push_back(singleton_member);
	}
	Dictionary enums = info.get("enums", Dictionary());
	for (const KeyValue<Variant, Variant> &entry : enums) {
		Dictionary member;
		member["name"] = entry.key;
		member["kind"] = "enum";
		member["type"] = "Int";
		member["values"] = entry.value;
		member["static"] = true;
		member["owner"] = p_name;
		result.push_back(member);
		for (const Variant &value : Array(entry.value)) {
			Dictionary enum_value;
			enum_value["name"] = value;
			enum_value["kind"] = "enum_value";
			enum_value["type"] = "Int";
			enum_value["static"] = true;
			enum_value["enum"] = entry.key;
			enum_value["owner"] = p_name;
			result.push_back(enum_value);
		}
	}
	return result;
}

Dictionary HaxeApiIndex::get_class_enums(const String &p_name) {
	Dictionary info = get_class(p_name);
	return info.get("enums", Dictionary());
}

Vector<String> HaxeApiIndex::get_member_names(const String &p_name) {
	Vector<String> names;
	if (!ensure_loaded()) {
		return names;
	}
	{
		MutexLock lock(mutex);
		if (member_cache.has(p_name)) {
			return member_cache[p_name];
		}
	}
	HashSet<String> seen;
	String current = p_name;
	int guard = 0;
	while (!current.is_empty() && guard++ < 64) {
		for (const Variant &item : get_declared_members(current)) {
			Dictionary member = item;
			String name = member.get("name", String());
			if (!name.is_empty() && !seen.has(name)) {
				seen.insert(name);
				names.push_back(name);
			}
		}
		current = get_base(current);
	}
	MutexLock lock(mutex);
	member_cache[p_name] = names;
	return names;
}

Vector<String> HaxeApiIndex::get_global_enum_names() {
	Vector<String> names;
	if (!ensure_loaded()) {
		return names;
	}
	MutexLock lock(mutex);
	for (const KeyValue<Variant, Variant> &entry : global_enums) {
		names.push_back(entry.key);
	}
	names.sort();
	return names;
}

Vector<String> HaxeApiIndex::get_global_enum_values(const String &p_name) {
	Vector<String> values;
	if (!ensure_loaded()) {
		return values;
	}
	MutexLock lock(mutex);
	if (!global_enums.has(p_name)) {
		return values;
	}
	for (const Variant &value : Array(global_enums[p_name])) {
		values.push_back(value);
	}
	return values;
}

bool HaxeApiIndex::find_member(const String &p_class, const String &p_member, Dictionary &r_member, String &r_owner) {
	String current = p_class;
	int guard = 0;
	while (!current.is_empty() && guard++ < 64) {
		for (const Variant &item : get_declared_members(current)) {
			Dictionary member = item;
			if (String(member.get("name", String())) == p_member) {
				r_member = member;
				r_owner = current;
				return true;
			}
		}
		current = get_base(current);
	}
	return false;
}

void HaxeApiIndex::invalidate() {
	MutexLock lock(mutex);
	loaded = false;
	failed = false;
}
