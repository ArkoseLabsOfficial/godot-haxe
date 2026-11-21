/**************************************************************************/
/*  haxe_libraries_panel.cpp                                              */
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

#include "haxe_libraries_panel.h"

#include "../haxe_library_index.h"
#include "../haxe_project.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/tree.h"
#include "scene/main/timer.h"

HaxeLibrariesPanel::HaxeLibrariesPanel() {
	VBoxContainer *root = memnew(VBoxContainer);
	root->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(root);

	Label *intro = memnew(Label);
	intro->set_text(TTR("Libraries are installed into this project only") + " (" + HaxeProject::get_haxelib_dir() + ").");
	intro->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	root->add_child(intro);

	tree = memnew(Tree);
	tree->set_columns(4);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, TTR("Name"));
	tree->set_column_title(1, TTR("Version"));
	tree->set_column_title(2, TTR("Declared"));
	tree->set_column_title(3, TTR("Path"));
	tree->set_column_expand(0, true);
	tree->set_column_expand(1, false);
	tree->set_column_expand(2, false);
	tree->set_column_expand(3, true);
	tree->set_column_custom_minimum_width(1, 100);
	tree->set_column_custom_minimum_width(2, 110);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->connect("item_selected", callable_mp(this, &HaxeLibrariesPanel::_on_selection_changed));
	root->add_child(tree);

	HBoxContainer *form = memnew(HBoxContainer);
	name_edit = memnew(LineEdit);
	name_edit->set_placeholder(TTR("Library name"));
	name_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	form->add_child(name_edit);
	version_edit = memnew(LineEdit);
	version_edit->set_placeholder(TTR("Version (optional)"));
	version_edit->set_custom_minimum_size(Size2(140, 0));
	form->add_child(version_edit);
	install_button = memnew(Button);
	install_button->set_text(TTR("Install"));
	install_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_install));
	form->add_child(install_button);
	skip_dependencies = memnew(CheckBox);
	skip_dependencies->set_text(TTR("Skip dependencies"));
	skip_dependencies->set_tooltip_text(TTR("Pass --skip-dependencies to haxelib and install only this library."));
	form->add_child(skip_dependencies);
	root->add_child(form);

	// Installing from a Git repository: `haxelib git <name> <url> [branch]`.
	HBoxContainer *git_form = memnew(HBoxContainer);
	url_edit = memnew(LineEdit);
	url_edit->set_placeholder(TTR("Git URL (https://… or user/repo)"));
	url_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	git_form->add_child(url_edit);
	branch_edit = memnew(LineEdit);
	branch_edit->set_placeholder(TTR("Branch (optional)"));
	branch_edit->set_custom_minimum_size(Size2(140, 0));
	git_form->add_child(branch_edit);
	git_button = memnew(Button);
	git_button->set_text(TTR("Install from Git"));
	git_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_install_git));
	git_form->add_child(git_button);
	root->add_child(git_form);

	HBoxContainer *actions = memnew(HBoxContainer);
	Label *version_label = memnew(Label);
	version_label->set_text(TTR("Version:"));
	actions->add_child(version_label);
	// The versions the selected library can switch to. They are read from the
	// project repository (what is already unpacked) and from `haxelib info`
	// (what is released and can be downloaded), so no version has to be typed.
	version_option = memnew(OptionButton);
	version_option->set_tooltip_text(TTR("Versions of the selected library. Choosing one downloads it if needed and makes it the active version."));
	version_option->set_custom_minimum_size(Size2(140, 0));
	actions->add_child(version_option);
	switch_version_button = memnew(Button);
	switch_version_button->set_text(TTR("Switch Version"));
	switch_version_button->set_tooltip_text(TTR("Switch the selected library to the version chosen in the list."));
	switch_version_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_switch_version));
	actions->add_child(switch_version_button);
	update_button = memnew(Button);
	update_button->set_text(TTR("Update Selected"));
	update_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_update));
	actions->add_child(update_button);
	remove_button = memnew(Button);
	remove_button->set_text(TTR("Remove Selected"));
	remove_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_remove));
	actions->add_child(remove_button);
	refresh_button = memnew(Button);
	refresh_button->set_text(TTR("Refresh"));
	refresh_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_refresh));
	actions->add_child(refresh_button);
	// Opens a library source in the script editor like any other script: the
	// file is shown, edited and saved in place. This is how a `@:globalClass`
	// class the editor attaches by name (`hxvlc.godot.Video`) can be inspected
	// and adjusted from inside the project.
	open_source_button = memnew(Button);
	open_source_button->set_text(TTR("Browse Source..."));
	open_source_button->set_tooltip_text(TTR("Open a Haxe source file of the selected library in the script editor. Saving writes it back to the library."));
	open_source_button->connect("pressed", callable_mp(this, &HaxeLibrariesPanel::_open_source));
	actions->add_child(open_source_button);
	root->add_child(actions);

	progress = memnew(ProgressBar);
	progress->set_visible(false);
	progress->set_max(100);
	progress->set_indeterminate(true);
	root->add_child(progress);

	status = memnew(Label);
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	root->add_child(status);

	// haxelib reports progress on a single rewritten line, so it has to be
	// read often enough for the bar to follow the download.
	poll_timer = memnew(Timer);
	poll_timer->set_wait_time(0.15);
	poll_timer->set_autostart(false);
	poll_timer->connect("timeout", callable_mp(this, &HaxeLibrariesPanel::_poll_operation));
	add_child(poll_timer);

	// The picker for "Browse Source...". It starts in the selected library's
	// class path and only lists Haxe sources.
	source_dialog = memnew(FileDialog);
	source_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
	source_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	source_dialog->add_filter("*.hx", TTR("Haxe Source"));
	source_dialog->connect("file_selected", callable_mp(this, &HaxeLibrariesPanel::_on_source_selected));
	add_child(source_dialog);

	_on_selection_changed();
}

void HaxeLibrariesPanel::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && is_visible_in_tree()) {
		_refresh();
	}
}

HaxeLibrariesPanel::~HaxeLibrariesPanel() {
	if (process != nullptr) {
		process->kill();
		memdelete(process);
		process = nullptr;
	}
	if (info_process != nullptr) {
		info_process->kill();
		memdelete(info_process);
		info_process = nullptr;
	}
}

void HaxeLibrariesPanel::refresh() {
	_refresh();
}

void HaxeLibrariesPanel::_set_status(const String &p_text) {
	status->set_text(p_text);
}

void HaxeLibrariesPanel::_refresh() {
	// Completion reads the installed libraries from its own index; the manager
	// is where libraries change, so it invalidates that index every time it
	// re-reads the repository. That also covers a refresh triggered by hand
	// after a library was unpacked into `haxelib/` outside the editor.
	HaxeLibraryIndex::get_singleton()->invalidate();

	// Keep the selected library across a refresh (an operation finishes with
	// one), so the version list does not collapse to nothing every time.
	String selected_name;
	const int previous = _selected_index();
	if (previous >= 0 && previous < libraries.size()) {
		selected_name = libraries[previous].name;
	}

	libraries.clear();
	tree->clear();
	tree->create_item();

	// Show every library actually installed in the project repository. The
	// build resolves dependencies from haxe.json, but the manager should list
	// what is on disk: a library that was installed or unpacked by hand must
	// not be invisible just because haxe.json does not mention it.
	HaxeBuild::list_installed_libraries(libraries);

	for (int i = 0; i < libraries.size(); i++) {
		const HaxeBuild::Library &library = libraries[i];
		TreeItem *item = tree->create_item();
		item->set_text(0, library.name);
		item->set_text(1, library.version.is_empty() ? TTR("unknown") : library.version);
		item->set_text(2, library.declared ? TTR("yes") : TTR("no"));
		item->set_text(3, library.path);
		item->set_metadata(0, i);
		if (!selected_name.is_empty() && library.name == selected_name) {
			item->select(0);
		}
	}
	_on_selection_changed();
	if (process == nullptr) {
		// Do not overwrite the progress message of a running operation.
		_set_status(vformat(TTR("%d librar%s installed."), libraries.size(), libraries.size() == 1 ? "y" : "ies"));
	}

	// Installing, removing or switching a library changes which
	// `@:globalClass` library classes exist, so the registered set (and the
	// "Create Node" dialog) is refreshed here.
	HaxeProject::sync_library_global_classes();
}

int HaxeLibrariesPanel::_selected_index() const {
	TreeItem *item = tree->get_selected();
	if (item == nullptr) {
		return -1;
	}
	return item->get_metadata(0);
}

void HaxeLibrariesPanel::_on_selection_changed() {
	int index = _selected_index();
	bool project_library = index >= 0 && index < libraries.size() && !libraries[index].global;
	bool busy = process != nullptr;
	update_button->set_disabled(busy || !project_library);
	remove_button->set_disabled(busy || !project_library);
	switch_version_button->set_disabled(busy || !project_library);
	version_option->set_disabled(busy || !project_library);
	// Reading a source is safe for a global library too, so only a missing
	// selection disables it.
	open_source_button->set_disabled(index < 0);

	if (index >= 0 && index < libraries.size()) {
		// Read the released versions of the newly selected library. The query
		// runs in the background; the list shows the on-disk versions until it
		// answers.
		if (libraries[index].name != info_query_name) {
			_start_info_query(libraries[index].name);
		}
	} else if (info_process != nullptr || !info_query_name.is_empty()) {
		if (info_process != nullptr) {
			info_process->kill();
			memdelete(info_process);
			info_process = nullptr;
		}
		info_query_name = String();
		info_versions.clear();
	}
	_populate_versions();
}

void HaxeLibrariesPanel::_populate_versions() {
	version_option->clear();
	int index = _selected_index();
	if (index < 0 || index >= libraries.size()) {
		return;
	}
	const HaxeBuild::Library &library = libraries[index];
	Vector<String> versions;
	if (!library.version.is_empty()) {
		versions.push_back(library.version);
	}
	Vector<String> local;
	HaxeBuild::list_library_versions(library.name, local);
	for (const String &version : local) {
		if (!versions.has(version)) {
			versions.push_back(version);
		}
	}
	for (const String &version : info_versions) {
		if (!versions.has(version)) {
			versions.push_back(version);
		}
	}
	for (int i = 0; i < versions.size(); i++) {
		version_option->add_item(versions[i], i);
	}
	int current = versions.find(library.version);
	if (current >= 0) {
		version_option->select(current);
	}
}

void HaxeLibrariesPanel::_start_info_query(const String &p_name) {
	if (info_process != nullptr) {
		info_process->kill();
		memdelete(info_process);
		info_process = nullptr;
	}
	info_versions.clear();
	info_query_name = p_name;
	if (p_name.is_empty()) {
		return;
	}
	List<String> arguments;
	arguments.push_back("info");
	arguments.push_back(p_name);
	info_process = memnew(HaxeProcess);
	if (info_process->start(HaxeProject::get_haxelib_executable(), arguments, HaxeProject::get_haxelib_dir()) != OK) {
		memdelete(info_process);
		info_process = nullptr;
		return;
	}
	poll_timer->start();
}

void HaxeLibrariesPanel::_poll_info() {
	if (info_process == nullptr) {
		return;
	}
	if (info_process->poll()) {
		return;
	}
	String output = info_process->get_output();
	memdelete(info_process);
	info_process = nullptr;
	info_versions = HaxeBuild::parse_available_versions(output);
	_populate_versions();
}

void HaxeLibrariesPanel::_refresh_open_text_times() {
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor == nullptr) {
		return;
	}
	// An unsaved `haxe.json` tab is left alone on purpose: there the reload
	// prompt is wanted, so the user decides whether the hand edits or the
	// manifest the panel just wrote should win.
	String manifest = ProjectSettings::get_singleton()->localize_path(HaxeProject::get_manifest_path());
	if (script_editor->get_unsaved_files().has(manifest)) {
		return;
	}
	script_editor->update_script_times();
}

void HaxeLibrariesPanel::_set_busy(bool p_busy) {
	install_button->set_disabled(p_busy);
	git_button->set_disabled(p_busy);
	refresh_button->set_disabled(p_busy);
	name_edit->set_editable(!p_busy);
	version_edit->set_editable(!p_busy);
	url_edit->set_editable(!p_busy);
	branch_edit->set_editable(!p_busy);
	skip_dependencies->set_disabled(p_busy);
	progress->set_visible(p_busy);
	if (p_busy) {
		poll_timer->start();
	} else if (info_process == nullptr) {
		poll_timer->stop();
	}
	_on_selection_changed();
}

void HaxeLibrariesPanel::_start_operation(Operation p_operation, const String &p_name, const String &p_version, const String &p_url, const String &p_branch) {
	if (process != nullptr) {
		return;
	}
	operation = p_operation;
	operation_name = p_name;
	operation_version = p_version;

	List<String> arguments;
	switch (operation) {
		case OPERATION_INSTALL: {
			String output;
			if (HaxeBuild::prepare_haxelib_repository(output) != OK) {
				_set_status(vformat(TTR("Cannot install %s: %s"), p_name, output.strip_edges()));
				operation = OPERATION_NONE;
				return;
			}
			arguments = HaxeBuild::install_arguments(p_name, p_version, skip_dependencies->is_pressed());
			_set_status(vformat(TTR("Installing %s..."), p_name));
		} break;
		case OPERATION_GIT: {
			String output;
			if (HaxeBuild::prepare_haxelib_repository(output) != OK) {
				_set_status(vformat(TTR("Cannot install %s: %s"), p_name, output.strip_edges()));
				operation = OPERATION_NONE;
				return;
			}
			arguments = HaxeBuild::git_arguments(p_name, p_url, p_branch);
			_set_status(vformat(TTR("Cloning %s from Git..."), p_name));
		} break;
		case OPERATION_SET_VERSION:
			arguments = HaxeBuild::version_arguments(p_name, p_version);
			_set_status(vformat(TTR("Switching %s to version %s..."), p_name, p_version));
			break;
		case OPERATION_UPDATE:
			arguments = HaxeBuild::update_arguments(p_name);
			_set_status(vformat(TTR("Updating %s..."), p_name));
			break;
		case OPERATION_REMOVE:
			arguments = HaxeBuild::remove_arguments(p_name);
			_set_status(vformat(TTR("Removing %s..."), p_name));
			break;
		default:
			return;
	}

	process = memnew(HaxeProcess);
	Error error = process->start(HaxeProject::get_haxelib_executable(), arguments, HaxeProject::get_haxelib_dir());
	if (error != OK) {
		_set_status(vformat(TTR("Cannot run '%s'."), HaxeProject::get_haxelib_executable()));
		memdelete(process);
		process = nullptr;
		operation = OPERATION_NONE;
		return;
	}
	// A download has a percentage, everything else is a short command.
	progress->set_indeterminate(true);
	progress->set_value(0);
	_set_busy(true);
}

void HaxeLibrariesPanel::_poll_operation() {
	if (process != nullptr) {
		if (process->poll()) {
			int percent = HaxeBuild::parse_progress(process->get_output());
			if (percent >= 0) {
				progress->set_indeterminate(false);
				progress->set_value(percent);
			}
		} else {
			_finish_operation();
		}
	}
	_poll_info();
	// The timer serves both the library operation and the background version
	// query, so it only stops once neither is running. `_set_busy(false)` (from
	// `_finish_operation`) stops it, hence the restart here.
	if (process != nullptr || info_process != nullptr) {
		poll_timer->start();
	} else {
		poll_timer->stop();
	}
}

void HaxeLibrariesPanel::_finish_operation() {
	if (process == nullptr) {
		return;
	}
	String output = process->get_output();
	int exit_code = process->get_exit_code();
	memdelete(process);
	process = nullptr;

	String name = operation_name;
	Operation finished = operation;
	operation = OPERATION_NONE;
	_set_busy(false);

	if (exit_code != 0) {
		_set_status(vformat(TTR("Haxelib failed with exit code %d: %s"), exit_code, output.strip_edges()));
		return;
	}

	Error error = OK;
	switch (finished) {
		case OPERATION_INSTALL:
			error = HaxeBuild::record_installed_library(name, operation_version, output);
			break;
		case OPERATION_GIT:
			// A Git checkout is the library's active version; record it with an
			// empty version so the build resolves the checkout, not a stale pin.
			error = HaxeBuild::record_installed_library(name, String(), output, true);
			break;
		case OPERATION_SET_VERSION:
			error = HaxeBuild::record_installed_library(name, operation_version, output);
			break;
		case OPERATION_REMOVE:
			error = HaxeBuild::forget_removed_library(name, output);
			break;
		default:
			break;
	}
	if (error != OK) {
		_set_status(output.strip_edges());
		return;
	}

	if (finished == OPERATION_INSTALL || finished == OPERATION_GIT || finished == OPERATION_SET_VERSION || finished == OPERATION_REMOVE) {
		// Those operations can rewrite `haxe.json`. It is a JSON file, so the
		// script editor opens it as a plain text tab; without refreshing the
		// recorded time the editor reported it as changed on disk the next time
		// the window regained focus.
		_refresh_open_text_times();
	}

	_refresh();
	switch (finished) {
		case OPERATION_INSTALL:
			version_edit->set_text(String());
			_set_status(vformat(TTR("Installed %s."), name));
			break;
		case OPERATION_GIT:
			_set_status(vformat(TTR("Installed %s from Git."), name));
			break;
		case OPERATION_SET_VERSION:
			_set_status(vformat(TTR("Switched %s to version %s."), name, operation_version));
			break;
		case OPERATION_UPDATE:
			_set_status(vformat(TTR("Updated %s."), name));
			break;
		case OPERATION_REMOVE:
			_set_status(vformat(TTR("Removed %s."), name));
			break;
		default:
			break;
	}

	// A version switch, an update or a Git checkout replaces the library
	// sources on disk. If one of them was open, the same file is brought back
	// from the new sources, so the editor tab follows the active version.
	if (finished == OPERATION_SET_VERSION || finished == OPERATION_UPDATE || finished == OPERATION_GIT) {
		_reopen_source_after_switch(name);
	}
}

void HaxeLibrariesPanel::_install() {
	String name = name_edit->get_text().strip_edges();
	if (name.is_empty()) {
		_set_status(TTR("Enter the name of the library to install."));
		return;
	}
	_start_operation(OPERATION_INSTALL, name, version_edit->get_text().strip_edges());
}

void HaxeLibrariesPanel::_install_git() {
	String name = name_edit->get_text().strip_edges();
	if (name.is_empty()) {
		_set_status(TTR("Enter the name of the library to install."));
		return;
	}
	String url = url_edit->get_text().strip_edges();
	if (url.is_empty()) {
		_set_status(TTR("Enter the Git repository URL (or 'user/repo')."));
		return;
	}
	_start_operation(OPERATION_GIT, name, String(), url, branch_edit->get_text().strip_edges());
}

void HaxeLibrariesPanel::_switch_version() {
	int index = _selected_index();
	if (index < 0 || index >= libraries.size()) {
		return;
	}
	const String &name = libraries[index].name;
	if (version_option->get_item_count() == 0) {
		_set_status(vformat(TTR("No versions are known for %s yet."), name));
		return;
	}
	String version = version_option->get_item_text(version_option->get_selected());
	if (version.is_empty()) {
		return;
	}
	if (version == libraries[index].version) {
		_set_status(vformat(TTR("%s is already at version %s."), name, version));
		return;
	}
	_start_operation(OPERATION_SET_VERSION, name, version);
}

void HaxeLibrariesPanel::_update() {
	int index = _selected_index();
	if (index < 0 || index >= libraries.size()) {
		return;
	}
	_start_operation(OPERATION_UPDATE, libraries[index].name, String());
}

void HaxeLibrariesPanel::_remove() {
	int index = _selected_index();
	if (index < 0 || index >= libraries.size()) {
		return;
	}
	_start_operation(OPERATION_REMOVE, libraries[index].name, String());
}

/* The directory a library's sources live in: its declared class path when the
 * manifest has one, otherwise the installed package directory. */
static String _library_source_root(const HaxeBuild::Library &p_library) {
	return p_library.class_path.is_empty() ? p_library.path : p_library.class_path;
}

void HaxeLibrariesPanel::_open_source() {
	int index = _selected_index();
	if (index < 0 || index >= libraries.size()) {
		_set_status(TTR("Select a library first."));
		return;
	}
	const HaxeBuild::Library &library = libraries[index];
	const String root = _library_source_root(library);
	if (!DirAccess::dir_exists_absolute(root)) {
		_set_status(vformat(TTR("The sources of %s are not on disk (%s)."), library.name, root));
		return;
	}
	// Remember what is about to be opened so a version switch can bring the
	// same file back from the newly selected version.
	opened_source_library = library.name;
	opened_source_relative = String();
	source_dialog->set_title(vformat(TTR("Open a source of %s"), library.name));
	source_dialog->set_current_dir(root);
	source_dialog->popup_centered_ratio(0.6);
}

void HaxeLibrariesPanel::_on_source_selected(const String &p_path) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const String path = settings != nullptr ? settings->localize_path(p_path) : p_path;

	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor == nullptr) {
		return;
	}
	// The `.hx` loader builds an `HaxeScript`, so the file opens in the script
	// editor as a normal script and saving writes it back through the saver.
	script_editor->open_file(path);

	// Record the path relative to the library root, so it survives the class
	// path changing under a different version.
	int index = _selected_index();
	if (index >= 0 && index < libraries.size()) {
		const HaxeBuild::Library &library = libraries[index];
		const String root = _library_source_root(library);
		opened_source_library = library.name;
		opened_source_relative = path;
		if (!p_path.begins_with(root)) {
			opened_source_relative = p_path.get_file();
		} else {
			opened_source_relative = p_path.substr(root.length()).trim_prefix("/");
		}
		_set_status(vformat(TTR("Opened %s of %s."), opened_source_relative, library.name));
	}
}

void HaxeLibrariesPanel::_reopen_source_after_switch(const String &p_library) {
	if (p_library.is_empty() || opened_source_library != p_library || opened_source_relative.is_empty()) {
		return;
	}
	for (const HaxeBuild::Library &library : libraries) {
		if (library.name != p_library) {
			continue;
		}
		const String root = _library_source_root(library);
		const String path = root.path_join(opened_source_relative);
		if (!FileAccess::exists(path)) {
			// The new version does not ship that file; leave the editor alone
			// instead of opening a stale or missing tab.
			_set_status(vformat(TTR("%s has no %s after the switch."), p_library, opened_source_relative));
			return;
		}
		ProjectSettings *settings = ProjectSettings::get_singleton();
		const String localized = settings != nullptr ? settings->localize_path(path) : path;
		ScriptEditor *script_editor = ScriptEditor::get_singleton();
		if (script_editor != nullptr) {
			script_editor->open_file(localized);
		}
		_set_status(vformat(TTR("Reopened %s after switching %s."), opened_source_relative, p_library));
		return;
	}
}
