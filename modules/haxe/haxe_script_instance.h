/**************************************************************************/
/*  haxe_script_instance.h                                                */
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

#include "haxe_runtime.h"
#include "haxe_script.h"

#include "core/object/script_instance.h"

/* Bridges one engine Object to one Haxe class instance.
 *
 * All property and method traffic is routed through the ABI to the compiled
 * Haxe library, so the Haxe object is the single source of truth for script
 * state. Nothing is cached on the engine side except metadata derived from
 * the script source.
 */
class HaxeScriptInstance : public ScriptInstance {
public:
	bool initialize(HaxeLibrary *p_library, HaxeScript *p_script, Object *p_owner);
	~HaxeScriptInstance() override;

	bool is_valid() const { return instance_created && library != nullptr; }
	const String &get_last_error() const { return last_error; }
	int64_t get_object_id() const { return object_id; }
	HaxeLibrary *get_library() const { return library; }
	HaxeScript *get_script_resource() const { return script.ptr(); }

	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	void get_property_list(List<PropertyInfo> *p_properties) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid = nullptr) const override;
	void validate_property(PropertyInfo &p_property) const override {}
	bool property_can_revert(const StringName &p_name) const override;
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;
	Object *get_owner() override;
	void get_method_list(List<MethodInfo> *p_list) const override;
	bool has_method(const StringName &p_method) const override;
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override;
	void notification(int p_notification, bool p_reversed = false) override;
	String to_string(bool *r_valid) override;
	void refcount_incremented() override {}
	bool refcount_decremented() override { return true; }
	Ref<Script> get_script() const override;
	bool is_placeholder() const override { return false; }
	void property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) override;
	Variant property_get_fallback(const StringName &p_name, bool *r_valid) override;
	ScriptLanguage *get_language() override;

private:
	HaxeLibrary *library = nullptr;
	// Holding a reference keeps the script alive: the engine only keeps the
	// attached script alive through the instance, exactly like GDScript's
	// `Ref<GDScript> script`.
	Ref<HaxeScript> script;
	Object *owner = nullptr;
	int64_t object_id = 0;
	bool instance_created = false;
	mutable String last_error;

	bool is_script_member(const StringName &p_name) const;
	void report_failure(const String &p_context);
};
