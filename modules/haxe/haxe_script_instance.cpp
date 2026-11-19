/**************************************************************************/
/*  haxe_script_instance.cpp                                              */
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

#include "haxe_script_instance.h"

#include "haxe_language.h"
#include "haxe_project.h"
#include "haxe_script.h"

bool HaxeScriptInstance::initialize(HaxeLibrary *p_library, HaxeScript *p_script, Object *p_owner) {
	ERR_FAIL_NULL_V(p_library, false);
	ERR_FAIL_NULL_V(p_script, false);
	ERR_FAIL_NULL_V(p_owner, false);

	library = p_library;
	script = p_script;
	owner = p_owner;
	object_id = (int64_t)p_owner->get_instance_id();
	library->add_reference();

	int result = library->create_instance(object_id, script->get_haxe_class_name());
	if (result != 0) {
		last_error = vformat("Cannot create Haxe instance of '%s': %s",
				script->get_haxe_class_name(),
				library->get_last_error().is_empty() ? String("unknown error") : library->get_last_error());
		report_failure("_init");
		library->release_reference();
		library = nullptr;
		script = Ref<HaxeScript>();
		owner = nullptr;
		return false;
	}

	instance_created = true;
	return true;
}

HaxeScriptInstance::~HaxeScriptInstance() {
	if (library != nullptr) {
		if (instance_created) {
			library->destroy_instance(object_id);
		}
		library->release_reference();
	}
}

namespace {

/* Looks up a member declared by the class or inherited from one of its project
 * base classes. The returned member is copied out because the base class
 * information is a temporary value. */
bool find_script_member(const HaxeClassInfo &p_info, const StringName &p_name, HaxeMemberInfo &r_member) {
	if (const HaxeMemberInfo *member = p_info.find_member(p_name)) {
		r_member = *member;
		return true;
	}
	HashSet<String> visited;
	String base = p_info.base;
	while (!base.is_empty() && !visited.has(base)) {
		visited.insert(base);
		HaxeClassInfo parent;
		if (!HaxeProject::get_class_info_by_name(base, parent)) {
			break;
		}
		if (const HaxeMemberInfo *member = parent.find_member(p_name)) {
			r_member = *member;
			return true;
		}
		base = parent.base;
	}
	return false;
}

} // namespace

void HaxeScriptInstance::report_failure(const String &p_context) {
	String message = vformat("Haxe script '%s' failed in %s: %s",
			script.is_valid() ? script->get_path() : String("<unknown>"), p_context,
			last_error.is_empty() ? String("unknown error") : last_error);
	if (library != nullptr && !library->get_last_stack_trace().is_empty()) {
		message += "\nHaxe stack trace:\n" + library->get_last_stack_trace();
	}
	HaxeRuntime::report_error(message);
	last_error = String();
}

bool HaxeScriptInstance::is_script_member(const StringName &p_name) const {
	if (script.is_null()) {
		return false;
	}
	HaxeMemberInfo member;
	return find_script_member(script->get_class_info(), p_name, member);
}

bool HaxeScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	if (!instance_created || library == nullptr || script.is_null()) {
		return false;
	}
	if (!is_script_member(p_name)) {
		return false;
	}

	HxValueHolder holder;
	hx_value_from_variant(p_value, holder);
	int result = library->set_field(object_id, p_name, &holder.value);
	if (result == 1) {
		// The member exists in the parsed source but not in the compiled
		// class. Accept the value so scene loading does not fail; the next
		// build stores it for real.
		return true;
	}
	if (result != 0) {
		last_error = library->get_last_error();
		report_failure(vformat("property set '%s'", p_name));
		return false;
	}
	return true;
}

bool HaxeScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (!instance_created || library == nullptr || script.is_null()) {
		return false;
	}
	if (!is_script_member(p_name)) {
		return false;
	}

	HxValue value = {};
	int result = library->get_field(object_id, p_name, &value);
	if (result == 1) {
		// The parsed source declares the member but the compiled library does
		// not expose it (a stale build, or a member kind hxcpp keeps outside
		// the instance field table). Reporting an error on every inspector
		// refresh floods the console, and returning nothing made the engine
		// show a zero value. Answer with the value written in the source
		// instead; the next build makes the lookup succeed for real.
		HaxeMemberInfo member;
		if (script.is_valid() && find_script_member(script->get_class_info(), p_name, member) &&
				member.default_value.get_type() != Variant::NIL) {
			r_ret = member.default_value;
			return true;
		}
		return false;
	}
	if (result != 0) {
		last_error = library->get_last_error();
		const_cast<HaxeScriptInstance *>(this)->report_failure(vformat("property get '%s'", p_name));
		return false;
	}
	if (!hx_value_to_variant(value, r_ret)) {
		r_ret = Variant();
	}
	library->release_return();
	return true;
}

void HaxeScriptInstance::get_property_list(List<PropertyInfo> *p_properties) const {
	if (script.is_valid()) {
		script->get_script_property_list(p_properties);
	}
}

Variant::Type HaxeScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	const HaxeMemberInfo *member = script.is_valid() ? script->get_class_info().find_member(p_name) : nullptr;
	if (member == nullptr) {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return Variant::NIL;
	}
	if (r_is_valid) {
		*r_is_valid = true;
	}
	String class_name;
	return haxe_type_to_variant(member->type, class_name);
}

bool HaxeScriptInstance::property_can_revert(const StringName &p_name) const {
	const HaxeMemberInfo *member = script.is_valid() ? script->get_class_info().find_member(p_name) : nullptr;
	return member != nullptr && member->default_value.get_type() != Variant::NIL;
}

bool HaxeScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	const HaxeMemberInfo *member = script.is_valid() ? script->get_class_info().find_member(p_name) : nullptr;
	if (member == nullptr) {
		return false;
	}
	r_ret = member->default_value;
	return true;
}

Object *HaxeScriptInstance::get_owner() {
	return owner;
}

void HaxeScriptInstance::get_method_list(List<MethodInfo> *p_list) const {
	if (script.is_valid()) {
		script->get_script_method_list(p_list);
	}
}

bool HaxeScriptInstance::has_method(const StringName &p_method) const {
	return script.is_valid() && script->has_method(p_method);
}

Variant HaxeScriptInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	r_error.error = Callable::CallError::CALL_OK;
	if (!instance_created || library == nullptr) {
		r_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return Variant();
	}
	// Object::callp dispatches here first for every method, including engine
	// ones (a Haxe `position` getter calls back into `get_position`). Only
	// methods the script itself declares may be answered here; anything else
	// must fall through to the native implementation, exactly like GDScript.
	if (!script.is_valid() || !script->has_method(p_method)) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Vector<HxValueHolder> holders;
	holders.resize(p_argcount);
	Vector<HxValue> values;
	values.resize(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		hx_value_from_variant(*p_args[i], holders.write[i]);
		values.write[i] = holders[i].value;
	}

	HxValue result = {};
	int status = library->invoke(object_id, p_method, p_argcount ? values.ptr() : nullptr, p_argcount, &result);
	if (status == 1) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	if (status != 0) {
		last_error = library->get_last_error();
		report_failure(vformat("call %s()", p_method));
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Variant ret;
	if (!hx_value_to_variant(result, ret)) {
		ret = Variant();
	}
	library->release_return();
	return ret;
}

void HaxeScriptInstance::notification(int p_notification, bool p_reversed) {
	if (!instance_created || library == nullptr) {
		return;
	}
	if (p_notification == Object::NOTIFICATION_PREDELETE) {
		// The engine is tearing the object down. Haxe still receives the
		// notification so it can release resources, but the instance is
		// destroyed right afterwards by the script instance destructor.
	}
	if (p_reversed) {
		return;
	}
	Variant value = p_notification;
	const Variant *argument = &value;
	Callable::CallError error;
	callp(SNAME("_notification"), &argument, 1, error);
	// A missing _notification is normal: most classes only implement
	// lifecycle methods that the engine calls through the virtual dispatch
	// path (_ready, _process, ...).
}

String HaxeScriptInstance::to_string(bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	if (script.is_null()) {
		return String();
	}
	if (r_valid) {
		*r_valid = true;
	}
	return vformat("<Haxe %s#%d>", script->get_haxe_class_name(), (int64_t)object_id);
}

Ref<Script> HaxeScriptInstance::get_script() const {
	return Ref<Script>(script.ptr());
}

void HaxeScriptInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
}

Variant HaxeScriptInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

ScriptLanguage *HaxeScriptInstance::get_language() {
	return HaxeLanguage::get_singleton();
}
