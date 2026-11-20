/**************************************************************************/
/*  test_haxe.cpp                                                         */
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

#include "test_haxe.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace TestHaxe {

static int scratch_counter = 0;

String make_scratch_dir() {
	String base = OS::get_singleton()->get_temp_path();
	String path = base.path_join("godot_haxe_tests").path_join(String::num_int64(++scratch_counter));
	DirAccess::make_dir_recursive_absolute(path);
	return path;
}

String write_source(const String &p_dir, const String &p_name, const String &p_contents) {
	DirAccess::make_dir_recursive_absolute(p_dir);
	String path = p_dir.path_join(p_name);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	// `Ref<T>` deletes comparison against `nullptr`, unlike `ERR_FAIL_NULL_V`.
	ERR_FAIL_COND_V_MSG(file.is_null(), String(), "Cannot open the scratch file for writing.");
	file->store_string(p_contents);
	return path;
}

void remove_scratch_dir(const String &p_dir) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_valid()) {
		// The module writes hidden files into the directories it manages
		// (`.gdignore`, haxelib's `.current`), and they have to be removed as
		// well or the directory cannot be deleted.
		dir->set_include_hidden(true);
		// Collect the entries before deleting any: removing an entry while the
		// live listing is being iterated skips the one that takes its slot.
		// The tests create packages below the scratch directory.
		Vector<String> directories = dir->get_directories();
		Vector<String> files = dir->get_files();
		for (const String &name : directories) {
			remove_scratch_dir(p_dir.path_join(name));
		}
		for (const String &name : files) {
			dir->remove(name);
		}
	}
	DirAccess::remove_absolute(p_dir);
	// Drop the cached parse results and the name index along with the files.
	HaxeProject::invalidate();
}

SourceDirScope::SourceDirScope(const String &p_dir) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	had_setting = settings->has_setting(HaxeProject::SETTING_SOURCE_DIR);
	if (had_setting) {
		previous = settings->get(HaxeProject::SETTING_SOURCE_DIR);
	}
	settings->set(HaxeProject::SETTING_SOURCE_DIR, p_dir);
	HaxeProject::invalidate();
}

SourceDirScope::~SourceDirScope() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (had_setting) {
		settings->set(HaxeProject::SETTING_SOURCE_DIR, previous);
	} else {
		settings->clear(HaxeProject::SETTING_SOURCE_DIR);
	}
	HaxeProject::invalidate();
}

HashSet<String> completion_names(const String &p_code, const String &p_path) {
	HashSet<String> names;
	List<ScriptLanguage::CodeCompletionOption> options;
	bool force = false;
	String call_hint;
	if (haxe_complete_code(p_code, p_path, nullptr, &options, force, call_hint) != OK) {
		return names;
	}
	for (const ScriptLanguage::CodeCompletionOption &option : options) {
		names.insert(option.display);
	}
	return names;
}

} // namespace TestHaxe
