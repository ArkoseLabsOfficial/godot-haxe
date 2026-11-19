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

#include "../haxe_project.h"

#include "core/object/callable_mp.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
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

	HBoxContainer *actions = memnew(HBoxContainer);
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
}

void HaxeLibrariesPanel::refresh() {
	_refresh();
}

void HaxeLibrariesPanel::_set_status(const String &p_text) {
	status->set_text(p_text);
}

void HaxeLibrariesPanel::_refresh() {
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
	}
	_on_selection_changed();
	if (process == nullptr) {
		// Do not overwrite the progress message of a running operation.
		_set_status(vformat(TTR("%d librar%s installed."), libraries.size(), libraries.size() == 1 ? "y" : "ies"));
	}
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
}

void HaxeLibrariesPanel::_set_busy(bool p_busy) {
	install_button->set_disabled(p_busy);
	refresh_button->set_disabled(p_busy);
	name_edit->set_editable(!p_busy);
	version_edit->set_editable(!p_busy);
	skip_dependencies->set_disabled(p_busy);
	progress->set_visible(p_busy);
	if (p_busy) {
		poll_timer->start();
	} else {
		poll_timer->stop();
	}
	_on_selection_changed();
}

void HaxeLibrariesPanel::_start_operation(Operation p_operation, const String &p_name, const String &p_version) {
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
	if (process == nullptr) {
		poll_timer->stop();
		return;
	}
	if (process->poll()) {
		int percent = HaxeBuild::parse_progress(process->get_output());
		if (percent >= 0) {
			progress->set_indeterminate(false);
			progress->set_value(percent);
		}
		return;
	}
	_finish_operation();
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

	_refresh();
	switch (finished) {
		case OPERATION_INSTALL:
			version_edit->set_text(String());
			_set_status(vformat(TTR("Installed %s."), name));
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
}

void HaxeLibrariesPanel::_install() {
	String name = name_edit->get_text().strip_edges();
	if (name.is_empty()) {
		_set_status(TTR("Enter the name of the library to install."));
		return;
	}
	_start_operation(OPERATION_INSTALL, name, version_edit->get_text().strip_edges());
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
