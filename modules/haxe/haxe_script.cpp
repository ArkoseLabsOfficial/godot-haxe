/**************************************************************************/
/*  haxe_script.cpp                                                       */
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

#include "haxe_script.h"

#include "haxe_language.h"
#include "haxe_project.h"
#include "haxe_script_instance.h"

#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

namespace {

/* Exported members of a class, followed by the exported members inherited from
 * its project base classes. Exports declared by the class itself win when a
 * base class repeats a name. */
Vector<HaxeMemberInfo> collect_exported_members(const HaxeClassInfo &p_info) {
	Vector<HaxeMemberInfo> members = p_info.members;
	HashSet<String> seen;
	for (const HaxeMemberInfo &member : members) {
		seen.insert(member.name);
	}
	HashSet<String> visited;
	String base = p_info.base;
	while (!base.is_empty() && !visited.has(base)) {
		visited.insert(base);
		HaxeClassInfo parent;
		if (!HaxeProject::get_class_info_by_name(base, parent)) {
			break;
		}
		for (const HaxeMemberInfo &member : parent.members) {
			if (seen.has(member.name) || member.is_static) {
				continue;
			}
			seen.insert(member.name);
			members.push_back(member);
		}
		base = parent.base;
	}
	return members;
}

} // namespace

void HaxeScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_source_code", "path"), &HaxeScript::load_source_code);
}

void HaxeScript::_notification(int p_what) {
	if (p_what == NOTIFICATION_POSTINITIALIZE) {
		// Nothing to do: the loader calls set_path()/load_source_code().
	}
}

HaxeScript::HaxeScript() {
	if (HaxeLanguage::get_singleton() != nullptr) {
		HaxeLanguage::get_singleton()->register_script(this);
	}
}

HaxeScript::~HaxeScript() {
	if (HaxeLanguage::get_singleton() != nullptr) {
		HaxeLanguage::get_singleton()->unregister_script(this);
	}
}

void HaxeScript::set_path(const String &p_path, bool p_take_over) {
	Script::set_path(p_path, p_take_over);
	if (!p_path.is_empty()) {
		if (template_package_pending) {
			template_package_pending = false;
			_apply_template_package(p_path);
		}
		load_source_code(p_path);
	}
}

/* Replaces the leading `package;` of a templated script with the package its
 * destination implies (`res://source/game/Coin.hx` → `package game;`). Only a
 * package statement in the file header is touched, so a custom script template
 * that starts with something else is left alone. */
void HaxeScript::_apply_template_package(const String &p_path) {
	if (source_code.is_empty()) {
		return;
	}
	String package = HaxeProject::get_package_for_path(p_path);
	String wanted = package.is_empty() ? String("package;") : "package " + package + ";";
	PackedStringArray lines = source_code.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String trimmed = String(lines[i]).strip_edges();
		if (trimmed.begins_with("package;") || (trimmed.begins_with("package ") && trimmed.ends_with(";"))) {
			lines.set(i, wanted);
			source_code = String("\n").join(lines);
			return;
		}
		bool is_header = trimmed.is_empty() || trimmed.begins_with("//") || trimmed.begins_with("/*") ||
				trimmed.begins_with("*") || trimmed.ends_with("*/");
		if (!is_header) {
			return;
		}
	}
}

Error HaxeScript::load_source_code(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		load_error = vformat("Cannot open Haxe script '%s'.", p_path);
		class_info = HaxeClassInfo();
		native_base_type = String();
		return ERR_CANT_OPEN;
	}

	source_code = file->get_as_utf8_string();
	// An exported game ships empty `.hx` stubs and the class descriptions in
	// the packaged metadata manifest, the same way Godot's C# export stubs out
	// its scripts. Parsing the stub would report a parse error, so the manifest
	// is the source of truth there.
	if (HaxeProject::uses_packaged_metadata() && source_code.strip_edges().is_empty()) {
		if (!HaxeProject::get_class_info(p_path, class_info) || !class_info.valid) {
			load_error = class_info.error.is_empty()
					? vformat("This exported game has no metadata for the Haxe script '%s'.", p_path)
					: class_info.error;
			class_info.valid = false;
			native_base_type = String();
			ERR_PRINT(vformat("%s: %s", p_path, load_error));
			return ERR_FILE_CORRUPT;
		}
	} else {
		Error error = parse_haxe_source(source_code, p_path, class_info);
		if (error != OK) {
			load_error = class_info.error.is_empty() ? "Failed to parse Haxe script." : class_info.error;
			class_info.valid = false;
			native_base_type = String();
			ERR_PRINT(vformat("%s: %s", p_path, load_error));
			return error;
		}
	}

	native_base_type = HaxeProject::resolve_native_base(class_info);
	if (native_base_type.is_empty()) {
		load_error = vformat("Cannot resolve the native base class of '%s' (extends %s). The base must be a Godot class or another Haxe script in this project.",
				class_info.name, class_info.base);
		class_info.valid = false;
		ERR_PRINT(vformat("%s: %s", p_path, load_error));
		return ERR_PARSE_ERROR;
	}

	load_error = String();
	emit_changed();
	return OK;
}

void HaxeScript::set_source_code(const String &p_code) {
	source_code = p_code;
	parse_haxe_source(source_code, get_path(), class_info);
	native_base_type = HaxeProject::resolve_native_base(class_info);
	// Publish the unsaved parse to the project index so a subclass that
	// inherits from this script resolves the same members the editor is
	// showing, instead of the previous version still on disk.
	HaxeProject::cache_class_info(class_info);
	emit_changed();
}

void HaxeScript::update_exports() {
#ifdef TOOLS_ENABLED
	// An `@:export` edit changes the Inspector; an edit to a method body does
	// not. Compare the exported members first so typing elsewhere does not
	// refresh every dependent script on each debounced validate.
	const Vector<HaxeMemberInfo> exported = collect_exported_members(class_info);
	String signature;
	for (const HaxeMemberInfo &member : exported) {
		signature += member.name;
		signature += "\x1f";
		signature += member.type;
		signature += "\x1f";
		signature += member.hint_string;
		signature += "\x1f";
		signature += member.group;
		signature += "\x1f";
		signature += member.subgroup;
		signature += "\x1f";
		signature += member.default_value.stringify();
		signature += "\x1e";
	}
	if (signature == export_signature) {
		return;
	}
	export_signature = signature;
	_update_placeholders();
	// Members are inherited, so the scripts that extend this one show the new
	// set too. This is the real-time path: the editor calls this while the
	// script is being typed, before it is saved or reloaded.
	if (HaxeLanguage *language = HaxeLanguage::get_singleton()) {
		language->refresh_dependent_exports(this);
	}
#endif
}

void HaxeScript::refresh_exported_members() {
#ifdef TOOLS_ENABLED
	_update_placeholders();
#endif
}

bool HaxeScript::can_instantiate() const {
	if (!class_info.valid || class_info.is_abstract || class_info.fq_name.is_empty()) {
		return false;
	}
#ifdef TOOLS_ENABLED
	// Only `@:tool` classes are compiled into the editor library, so a regular
	// class cannot be instantiated in the editor. A placeholder instance is
	// used instead, which exposes the exported members parsed from the source.
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return false;
	}
	if (Engine::get_singleton()->is_editor_hint()) {
		return is_tool();
	}
	// Like GDScript: outside of `@:tool` classes, project code must not run
	// while the editor has scripting turned off.
	if (!is_tool() && !ScriptServer::is_scripting_enabled()) {
		return false;
	}
#endif
	return true;
}

Ref<Script> HaxeScript::get_base_script() const {
	if (class_info.base.is_empty()) {
		return Ref<Script>();
	}
	HaxeClassInfo parent;
	String parent_path;
	if (!HaxeProject::get_class_info_by_name(class_info.base, parent, &parent_path)) {
		return Ref<Script>();
	}
	if (parent_path == get_path()) {
		return Ref<Script>();
	}
	return ResourceLoader::load(parent_path, "Script");
}

StringName HaxeScript::get_global_name() const {
	// Only `@:globalClass` classes are registered with the editor as named
	// classes (and offered in the "Create Node"/"Add Child Node" dialogs). The
	// other scripts are attached to nodes but have no global type name.
	if (!class_info.is_global_class) {
		return StringName();
	}
	return class_info.name;
}

bool HaxeScript::inherits_script(const Ref<Script> &p_script) const {
	Ref<HaxeScript> other = p_script;
	if (other.is_null()) {
		return false;
	}
	String base = class_info.base;
	HashSet<String> visited;
	while (!base.is_empty()) {
		if (base == other->class_info.fq_name || base == other->class_info.name) {
			return true;
		}
		if (visited.has(base)) {
			return false;
		}
		visited.insert(base);
		HaxeClassInfo parent;
		if (!HaxeProject::get_class_info_by_name(base, parent)) {
			return false;
		}
		base = parent.base;
	}
	return false;
}

StringName HaxeScript::get_instance_base_type() const {
	return native_base_type;
}

ScriptInstance *HaxeScript::instance_create(Object *p_this) {
	if (!can_instantiate()) {
		ERR_PRINT(vformat("Cannot instantiate Haxe script '%s': %s", get_path(), load_error.is_empty() ? String("invalid script") : load_error));
		return nullptr;
	}
	HaxeLanguage *language = Object::cast_to<HaxeLanguage>(get_language());
	HaxeLibrary *library = language ? language->get_script_library(this) : nullptr;
	if (library == nullptr) {
		ERR_PRINT(vformat("Cannot instantiate Haxe script '%s': the project Haxe library is not built or failed to load.\n%s",
				get_path(), language ? language->get_last_build_error() : String()));
		return nullptr;
	}
	if (!library->has_class(class_info.fq_name)) {
		ERR_PRINT(vformat("Haxe class '%s' is missing from the compiled library. Rebuild the project. Classes are compiled from the configured source directory (%s).",
				class_info.fq_name, HaxeProject::get_source_dir()));
		return nullptr;
	}
	HaxeScriptInstance *instance = memnew(HaxeScriptInstance);
	if (!instance->initialize(library, this, p_this)) {
		memdelete(instance);
		return nullptr;
	}
	return instance;
}

#ifdef TOOLS_ENABLED
void HaxeScript::_update_placeholders() {
	const Vector<HaxeMemberInfo> exported = collect_exported_members(class_info);
	for (PlaceHolderScriptInstance *placeholder : placeholders) {
		List<PropertyInfo> properties;
		get_script_property_list(&properties);
		HashMap<StringName, Variant> values;
		for (const HaxeMemberInfo &member : exported) {
			values[member.name] = member.default_value;
		}
		placeholder->update(properties, values);
	}
}
#endif

PlaceHolderScriptInstance *HaxeScript::placeholder_instance_create(Object *p_this) {
#ifdef TOOLS_ENABLED
	// Non-tool scripts do not run in the editor, but their exported members
	// must still show up in the inspector.
	PlaceHolderScriptInstance *placeholder = memnew(PlaceHolderScriptInstance(HaxeLanguage::get_singleton(), Ref<Script>(this), p_this));
	placeholders.insert(placeholder);
	_update_placeholders();
	return placeholder;
#else
	(void)p_this;
	return nullptr;
#endif
}

void HaxeScript::_placeholder_erased(PlaceHolderScriptInstance *p_placeholder) {
#ifdef TOOLS_ENABLED
	placeholders.erase(p_placeholder);
#else
	(void)p_placeholder;
#endif
}

Error HaxeScript::reload(bool p_keep_state) {
	(void)p_keep_state;
	if (get_path().is_empty()) {
		return ERR_UNCONFIGURED;
	}
	reloading = true;
	Error error = load_source_code(get_path());
	reloading = false;
	if (error == OK) {
		HaxeProject::invalidate(get_path());
#ifdef TOOLS_ENABLED
		_update_placeholders();
		// Exported members come from the base chain, so scripts that inherit
		// from this one must be refreshed as well.
		if (HaxeLanguage *language = HaxeLanguage::get_singleton()) {
			language->reload_dependent_scripts(this);
		}
#endif
	}
	return error;
}

#ifdef TOOLS_ENABLED
StringName HaxeScript::get_doc_class_name() const {
	return class_info.name;
}

Vector<DocData::ClassDoc> HaxeScript::get_documentation() const {
	Vector<DocData::ClassDoc> docs;
	if (!class_info.valid) {
		return docs;
	}
	DocData::ClassDoc doc;
	doc.name = class_info.name;
	// A doc comment above the class is its description; without one the editor
	// still gets a line saying what the file is.
	if (!class_info.doc.is_empty()) {
		doc.brief_description = class_info.doc;
	} else if (native_base_type.is_empty()) {
		doc.brief_description = "Haxe script.";
	} else {
		doc.brief_description = vformat("Haxe script extending %s.", native_base_type);
	}
	doc.is_script_doc = true;
	doc.script_path = get_path();
	for (const HaxeMemberInfo &member : class_info.members) {
		DocData::PropertyDoc property;
		property.name = member.name;
		property.type = variant_type_to_haxe(haxe_type_to_variant(member.type, property.enumeration));
		// The description is the `/** ... */` comment of the declaration. The
		// Inspector and the built-in help show it as the property's text; the
		// fallback keeps a property without a comment readable instead of
		// appearing undocumented.
		property.description = member.doc.is_empty()
				? vformat("Exported Haxe property of type %s.", member.type)
				: member.doc;
		doc.properties.push_back(property);
	}
	for (const HaxeFunctionInfo &function : class_info.functions) {
		MethodInfo method = function.to_method_info();
		DocData::MethodDoc method_doc;
		method_doc.name = method.name;
		method_doc.description = function.doc;
		method_doc.return_type = variant_type_to_haxe(method.return_val.type);
		for (const PropertyInfo &argument : method.arguments) {
			DocData::ArgumentDoc argument_doc;
			argument_doc.name = argument.name;
			argument_doc.type = variant_type_to_haxe(argument.type);
			method_doc.arguments.push_back(argument_doc);
		}
		doc.methods.push_back(method_doc);
	}
	docs.push_back(doc);
	return docs;
}

String HaxeScript::get_class_icon_path() const {
	/* No script icon: like GDScript without `@icon`, a `.hx` file does not
	 * override the icon of the object it is attached to. The Scene dock then
	 * keeps the object's native type icon. */
	return String();
}
#endif

bool HaxeScript::has_method(const StringName &p_method) const {
	return class_info.find_function(p_method) != nullptr;
}

bool HaxeScript::has_static_method(const StringName &p_method) const {
	const HaxeFunctionInfo *function = class_info.find_function(p_method);
	return function != nullptr && function->is_static;
}

MethodInfo HaxeScript::get_method_info(const StringName &p_method) const {
	const HaxeFunctionInfo *function = class_info.find_function(p_method);
	if (function == nullptr) {
		return MethodInfo();
	}
	return function->to_method_info();
}

bool HaxeScript::is_tool() const {
	return class_info.is_tool;
}

bool HaxeScript::is_valid() const {
	return class_info.valid && !native_base_type.is_empty();
}

bool HaxeScript::is_abstract() const {
	return class_info.is_abstract;
}

ScriptLanguage *HaxeScript::get_language() const {
	return HaxeLanguage::get_singleton();
}

bool HaxeScript::has_script_signal(const StringName &p_signal) const {
	for (const HaxeFunctionInfo &function : class_info.functions) {
		if (function.is_signal && function.name == p_signal) {
			return true;
		}
	}
	return false;
}

void HaxeScript::get_script_signal_list(List<MethodInfo> *r_signals) const {
	for (const HaxeFunctionInfo &function : class_info.functions) {
		if (function.is_signal) {
			r_signals->push_back(function.to_method_info());
		}
	}
}

bool HaxeScript::get_property_default_value(const StringName &p_property, Variant &r_value) const {
	const HaxeMemberInfo *member = class_info.find_member(p_property);
	if (member == nullptr && !class_info.base.is_empty()) {
		HashSet<String> visited;
		String base = class_info.base;
		while (!base.is_empty() && !visited.has(base)) {
			visited.insert(base);
			HaxeClassInfo parent;
			if (!HaxeProject::get_class_info_by_name(base, parent)) {
				break;
			}
			member = parent.find_member(p_property);
			if (member != nullptr) {
				break;
			}
			base = parent.base;
		}
	}
	if (member == nullptr) {
		return false;
	}
	r_value = member->default_value;
	return true;
}

void HaxeScript::get_script_method_list(List<MethodInfo> *p_list) const {
	for (const HaxeFunctionInfo &function : class_info.functions) {
		if (function.is_signal) {
			continue;
		}
		p_list->push_back(function.to_method_info());
	}
}

void HaxeScript::get_script_property_list(List<PropertyInfo> *p_list) const {
	// Inspector groups are extra entries in the property list, the same ones
	// the GDScript parser adds for `@export_group`: every property after an
	// entry with PROPERTY_USAGE_GROUP is displayed under that heading.
	String current_group;
	String current_subgroup;
	for (const HaxeMemberInfo &member : collect_exported_members(class_info)) {
		if (member.group != current_group) {
			current_group = member.group;
			current_subgroup = String();
			if (!current_group.is_empty()) {
				PropertyInfo group;
				group.name = current_group;
				group.usage = PROPERTY_USAGE_GROUP;
				p_list->push_back(group);
			}
		}
		if (member.subgroup != current_subgroup) {
			current_subgroup = member.subgroup;
			// A subgroup only exists inside a group, which is how the
			// inspector reads the list.
			if (!current_subgroup.is_empty() && !current_group.is_empty()) {
				PropertyInfo subgroup;
				subgroup.name = current_subgroup;
				subgroup.usage = PROPERTY_USAGE_SUBGROUP;
				p_list->push_back(subgroup);
			}
		}
		p_list->push_back(member.to_property_info());
	}
}

int HaxeScript::get_member_line(const StringName &p_member) const {
	const HaxeMemberInfo *member = class_info.find_member(p_member);
	if (member) {
		return member->line;
	}
	const HaxeFunctionInfo *function = class_info.find_function(p_member);
	if (function) {
		return function->line;
	}
	HashSet<String> visited;
	String base = class_info.base;
	while (!base.is_empty() && !visited.has(base)) {
		visited.insert(base);
		HaxeClassInfo parent;
		if (!HaxeProject::get_class_info_by_name(base, parent)) {
			break;
		}
		if (const HaxeMemberInfo *base_member = parent.find_member(p_member)) {
			return base_member->line;
		}
		if (const HaxeFunctionInfo *base_function = parent.find_function(p_member)) {
			return base_function->line;
		}
		base = parent.base;
	}
	return -1;
}

const Variant HaxeScript::get_rpc_config() const {
	return Variant();
}
