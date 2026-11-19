/**************************************************************************/
/*  haxe_script.h                                                         */
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

#include "core/object/script_language.h"

class HaxeScript : public Script {
	GDCLASS(HaxeScript, Script);

	String source_code;
	HaxeClassInfo class_info;
	String native_base_type;
	String load_error;
	bool reloading = false;
	/* Hash of the exported members the placeholders currently show. The
	 * editor calls `update_exports()` on every debounced keystroke; comparing
	 * this keeps an unrelated edit from walking the class chain and touching
	 * every dependent's Inspector. */
	String export_signature;
	/* A script created from a template still carries the placeholder package;
	 * `set_path()` replaces it with the package the chosen path implies. */
	bool template_package_pending = false;

	void _apply_template_package(const String &p_path);

#ifdef TOOLS_ENABLED
	HashSet<PlaceHolderScriptInstance *> placeholders;

	void _update_placeholders();
#endif

protected:
	static void _bind_methods();

	bool editor_can_reload_from_file() override { return true; }
	void _notification(int p_what);

public:
	HaxeScript();
	~HaxeScript();

	virtual void set_path(const String &p_path, bool p_take_over = false) override;

	Error load_source_code(const String &p_path);
	void set_source_code(const String &p_code) override;
	String get_source_code() const override { return source_code; }
	bool has_source_code() const override { return !source_code.is_empty(); }
	/* Marks a freshly templated script so `set_path()` can insert the package
	 * derived from the destination path. */
	void set_template_package_pending(bool p_pending) { template_package_pending = p_pending; }
	void update_exports() override;
	/* Re-collects the exported members and refreshes the placeholder
	 * instances (the Inspector's view), regardless of whether the signature
	 * changed. A subclass uses this when a base class edit changed what it
	 * inherits. */
	void refresh_exported_members();

	bool can_instantiate() const override;
	Ref<Script> get_base_script() const override;
	StringName get_global_name() const override;
	bool inherits_script(const Ref<Script> &p_script) const override;
	StringName get_instance_base_type() const override;
	ScriptInstance *instance_create(Object *p_this) override;
	PlaceHolderScriptInstance *placeholder_instance_create(Object *p_this) override;
	void _placeholder_erased(PlaceHolderScriptInstance *p_placeholder) override;

	virtual Error reload(bool p_keep_state = false) override;

#ifdef TOOLS_ENABLED
	virtual StringName get_doc_class_name() const override;
	virtual Vector<DocData::ClassDoc> get_documentation() const override;
	virtual String get_class_icon_path() const override;
#endif

	bool has_method(const StringName &p_method) const override;
	bool has_static_method(const StringName &p_method) const override;
	MethodInfo get_method_info(const StringName &p_method) const override;
	bool is_tool() const override;
	bool is_valid() const override;
	bool is_abstract() const override;
	ScriptLanguage *get_language() const override;

	bool has_script_signal(const StringName &p_signal) const override;
	void get_script_signal_list(List<MethodInfo> *r_signals) const override;
	bool get_property_default_value(const StringName &p_property, Variant &r_value) const override;
	void get_script_method_list(List<MethodInfo> *p_list) const override;
	void get_script_property_list(List<PropertyInfo> *p_list) const override;
	int get_member_line(const StringName &p_member) const override;
	const Variant get_rpc_config() const override;

	const HaxeClassInfo &get_class_info() const { return class_info; }
	String get_haxe_class_name() const { return class_info.fq_name; }
	String get_native_base_type() const { return native_base_type; }
	String get_load_error() const { return load_error; }
	uint64_t get_source_hash() const { return source_code.hash(); }
};
