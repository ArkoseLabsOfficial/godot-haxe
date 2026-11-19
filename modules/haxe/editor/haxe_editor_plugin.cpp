/**************************************************************************/
/*  haxe_editor_plugin.cpp                                                */
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

#include "haxe_editor_plugin.h"

#include "../haxe_build.h"
#include "../haxe_language.h"
#include "../haxe_project.h"
#include "haxe_libraries_panel.h"

#include "core/object/callable_mp.h"
#include "core/templates/hash_set.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/project_settings_editor.h"
#include "main/main.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/text_edit.h"
#include "scene/main/timer.h"
#include "servers/display/display_server.h"

namespace {

String normalize_path(const String &p_path) {
	if (p_path.is_empty()) {
		return p_path;
	}
	String path = p_path;
	if (path.begins_with("file://")) {
		path = path.substr(7).uri_decode();
	}
	String root = HaxeProject::get_project_root();
	if (!root.is_empty() && path.begins_with(root)) {
		String relative = path.substr(root.length());
		if (relative.begins_with("/")) {
			relative = relative.substr(1);
		}
		return "res://" + relative;
	}
	return path;
}

} // namespace

HaxeEditorPlugin::HaxeEditorPlugin() {
	set_process(false);

	panel = memnew(VBoxContainer);
	panel->set_name(TTRC("Haxe"));

	HBoxContainer *toolbar = memnew(HBoxContainer);
	panel->add_child(toolbar);

	build_button = memnew(Button);
	build_button->set_text(TTR("Build"));
	build_button->set_tooltip_text(TTR("Compile the project for the editor."));
	build_button->connect("pressed", callable_mp(this, &HaxeEditorPlugin::_build).bind(false));
	toolbar->add_child(build_button);

	rebuild_button = memnew(Button);
	rebuild_button->set_text(TTR("Rebuild"));
	rebuild_button->set_tooltip_text(TTR("Ignore the build stamp and compile everything."));
	rebuild_button->connect("pressed", callable_mp(this, &HaxeEditorPlugin::_build).bind(true));
	toolbar->add_child(rebuild_button);

	libraries_button = memnew(Button);
	libraries_button->set_text(TTR("Libraries"));
	libraries_button->set_tooltip_text(TTR("Manage the Haxe libraries of this project."));
	libraries_button->connect("pressed", callable_mp(this, &HaxeEditorPlugin::_open_libraries));
	toolbar->add_child(libraries_button);

	// The panel body is the build log itself. A read only TextEdit is used
	// rather than a label so the output can be selected and copied.
	log_text = memnew(TextEdit);
	log_text->set_editable(false);
	log_text->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	log_text->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	log_text->set_text(TTR("No build output yet."));
	panel->add_child(log_text);

	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_text(TTR("Haxe is ready."));
	panel->add_child(status_label);

	// Hidden until a build or a library operation is running, so the panel
	// stays compact. A compile reports no percentage, so it is indeterminate;
	// haxelib downloads do report one.
	progress = memnew(ProgressBar);
	progress->set_visible(false);
	progress->set_indeterminate(true);
	progress->set_max(100);
	panel->add_child(progress);

	libraries_panel = memnew(HaxeLibrariesPanel);

	poll_timer = memnew(Timer);
	poll_timer->set_wait_time(0.4);
	poll_timer->set_autostart(true);
	poll_timer->connect("timeout", callable_mp(this, &HaxeEditorPlugin::_poll));
	panel->add_child(poll_timer);
}

HaxeEditorPlugin::~HaxeEditorPlugin() {
}

String HaxeEditorPlugin::get_plugin_name() const {
	return "Haxe";
}

void HaxeEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			add_tool_menu_item(TTR("Haxe: Build Project"), callable_mp(this, &HaxeEditorPlugin::_build).bind(false));
			add_tool_menu_item(TTR("Haxe: Rebuild Project"), callable_mp(this, &HaxeEditorPlugin::_build).bind(true));
			add_tool_menu_item(TTR("Haxe: Libraries..."), callable_mp(this, &HaxeEditorPlugin::_open_libraries));
			add_control_to_bottom_panel(panel, TTR("Haxe"));

			// The library manager is a Project Settings tab, placed right
			// after the "Plugins" tab.
			ProjectSettingsEditor *project_settings = ProjectSettingsEditor::get_singleton();
			if (project_settings != nullptr) {
				libraries_panel->set_name(TTRC("Haxe"));
				add_control_to_container(CONTAINER_PROJECT_SETTING_TAB_RIGHT, libraries_panel);
				TabContainer *tabs = project_settings->get_tabs();
				if (tabs != nullptr) {
					int plugin_index = -1;
					for (int i = 0; i < tabs->get_child_count(); i++) {
						if (tabs->get_child(i)->is_class("EditorPluginSettings")) {
							plugin_index = i;
							break;
						}
					}
					if (plugin_index >= 0) {
						tabs->move_child(libraries_panel, plugin_index + 1);
					}
				}
			}

			_update_status();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			remove_tool_menu_item(TTR("Haxe: Build Project"));
			remove_tool_menu_item(TTR("Haxe: Rebuild Project"));
			remove_tool_menu_item(TTR("Haxe: Libraries..."));
			remove_control_from_bottom_panel(panel);
			remove_control_from_container(CONTAINER_PROJECT_SETTING_TAB_RIGHT, libraries_panel);
		} break;
	}
}

/* --------------------------------------------------------------- building -- */

bool HaxeEditorPlugin::_apply_build_result(const HaxeBuild::Result &p_result, String &r_error) {
	for (const ScriptLanguage::ScriptError &error : p_result.errors) {
		Message message;
		message.path = normalize_path(error.path);
		message.line = error.line > 0 ? error.line : 1;
		message.column = error.column > 0 ? error.column : 1;
		message.text = error.message;
		message.kind = MESSAGE_ERROR;
		build_messages.push_back(message);
	}
	if (!p_result.success && build_messages.is_empty()) {
		Message message;
		message.text = p_result.message.is_empty() ? TTR("The build failed.") : p_result.message;
		message.kind = MESSAGE_ERROR;
		build_messages.push_back(message);
	}

	if (p_result.success) {
		if (p_result.up_to_date) {
			status_label->set_text(TTR("Haxe build is up to date."));
		} else {
			status_label->set_text(vformat(TTR("Haxe build finished: %s"), p_result.library_path));
		}
	} else {
		r_error = p_result.message;
		status_label->set_text(vformat(TTR("Haxe build failed: %s"), p_result.message));
	}
	_refresh_messages();
	_update_build_log();
	return p_result.success;
}

void HaxeEditorPlugin::_set_building(bool p_building) {
	build_button->set_disabled(p_building);
	rebuild_button->set_disabled(p_building);
	libraries_button->set_disabled(p_building);
	progress->set_visible(p_building);
}

void HaxeEditorPlugin::_build(bool p_force) {
	make_bottom_panel_item_visible(panel);

	HaxeLanguage *language = HaxeLanguage::get_singleton();
	if (language == nullptr) {
		return;
	}
	if (language->has_build_session()) {
		// A build is already running (or its result has not been shown yet).
		// Starting a second compiler would write the same output file.
		return;
	}

	build_messages.clear();
	String problem = HaxeBuild::check_toolchain();
	toolchain_checked = true;
	toolchain_problem = problem;
	if (!problem.is_empty()) {
		status_label->set_text(problem);
		return;
	}

	if (!language->start_build(false, true, p_force)) {
		return;
	}
	_set_building(true);
	progress->set_indeterminate(true);
	status_label->set_text(TTR("Building the Haxe project..."));
}

bool HaxeEditorPlugin::build() {
	// Runs before the project starts. Building here means a compile error is
	// reported instead of the game silently running the previous library.
	if (!HaxeProject::build_on_run()) {
		return true;
	}
	if (run_build_active) {
		// Reached from an event pumped while the Run build was running: the
		// build that started first owns the library.
		return false;
	}

	HaxeLanguage *language = HaxeLanguage::get_singleton();
	if (language == nullptr) {
		return true;
	}

	String problem = HaxeBuild::check_toolchain();
	toolchain_checked = true;
	toolchain_problem = problem;
	if (!problem.is_empty()) {
		status_label->set_text(problem);
		make_bottom_panel_item_visible(panel);
		return false;
	}

	build_messages.clear();
	make_bottom_panel_item_visible(panel);
	status_label->set_text(TTR("Building the Haxe project before running..."));
	_update_build_log();

	// Compile through the same background session the toolbar uses, and drive
	// it here because `build()` has to be finished before the game starts.
	// A session the panel already started is drained first: it may be for the
	// editor library, and running the game needs the game library.
	run_build_active = true;
	while (language->is_building()) {
		language->poll_build();
		OS::get_singleton()->delay_usec(2000);
	}
	if (language->has_build_session()) {
		(void)language->take_build_result();
	}
	if (!language->start_build(false, false, false)) {
		run_build_active = false;
		String error;
		_apply_build_result(HaxeBuild::Result::failed(TTR("The Haxe build could not be started.")), error);
		return false;
	}

	// The editor blocks on this call, so the main loop is pumped while the
	// compiler runs. That is what Godot's own export code does while it waits
	// on an external build tool, and it is the difference between the Haxe
	// panel filling with compiler output and the window looking frozen.
	bool pump = DisplayServer::get_singleton()->can_any_window_draw();
	while (language->is_building()) {
		language->poll_build();
		if (!language->is_building()) {
			break;
		}
		_update_build_log();
		if (pump) {
			DisplayServer::get_singleton()->process_events();
			Main::force_redraw();
			if (Main::iteration()) {
				// The editor is quitting: stop drawing, but still wait for
				// the compiler so the result is reported.
				pump = false;
			}
		} else {
			OS::get_singleton()->delay_usec(2000);
		}
	}
	run_build_active = false;

	HaxeBuild::Result result = language->take_build_result();
	String error;
	bool success = _apply_build_result(result, error);
	if (!success) {
		ERR_PRINT(vformat("Haxe build failed, not running the project: %s", error));
		make_bottom_panel_item_visible(panel);
	}
	return success;
}

bool HaxeEditorPlugin::build_solutions() {
	if (!Main::is_cmdline_tool()) {
		// A Run: `build()` compiles the game library, which is the one the
		// game loads. Compiling the editor library as well would run two
		// compilers over the same sources before the game could start.
		return true;
	}
	String error;
	String problem = HaxeBuild::check_toolchain();
	if (!problem.is_empty()) {
		ERR_PRINT(vformat("Haxe toolchain is not usable: %s", problem));
		return false;
	}
	HaxeBuild::Result result = HaxeLanguage::get_singleton()->build_project(false, true, false, error);
	if (!result.success) {
		ERR_PRINT(vformat("Haxe build failed: %s", error));
		return false;
	}
	return true;
}

/* ------------------------------------------------------------- libraries -- */

void HaxeEditorPlugin::_open_libraries() {
	ProjectSettingsEditor *project_settings = ProjectSettingsEditor::get_singleton();
	if (project_settings == nullptr) {
		return;
	}
	project_settings->popup_project_settings();
	libraries_panel->refresh();
	TabContainer *tabs = project_settings->get_tabs();
	if (tabs != nullptr) {
		tabs->set_current_tab(tabs->get_tab_idx_from_control(libraries_panel));
	}
}

/* ------------------------------------------------------------ diagnostics -- */

Vector<HaxeEditorPlugin::Message> HaxeEditorPlugin::collect_diagnostics() const {
	Vector<Message> result;
	// Parse every project source in the engine. This is the same pass that
	// powers class discovery, completion and lookup, so it stays in sync with
	// the editor without an external process.
	for (const String &path : HaxeProject::scan_sources()) {
		HaxeClassInfo info;
		if (HaxeProject::get_class_info(path, info) && info.valid) {
			continue;
		}
		Message message;
		message.path = normalize_path(path);
		message.line = info.class_line > 0 ? info.class_line : 1;
		message.text = info.error.is_empty() ? String("Invalid Haxe source.") : info.error;
		message.kind = MESSAGE_ERROR;
		result.push_back(message);
	}
	return result;
}

Vector<HaxeEditorPlugin::Message> HaxeEditorPlugin::collect_build_messages() const {
	return build_messages;
}

void HaxeEditorPlugin::_report_to_output(const Message &p_message) {
	EditorLog *log = EditorNode::get_log();
	if (log == nullptr) {
		return;
	}
	String text;
	if (p_message.path.is_empty()) {
		text = p_message.text.replace("[", "[lb]");
	} else {
		String location = (vformat("%s:%d", p_message.path, p_message.line)).replace("[", "[lb]");
		text = vformat("[url]%s[/url] - %s", location, p_message.text.replace("[", "[lb]"));
	}
	log->add_message(text, p_message.kind == MESSAGE_WARNING ? EditorLog::MSG_TYPE_WARNING : EditorLog::MSG_TYPE_ERROR);
}

void HaxeEditorPlugin::_refresh_messages() {
	Vector<Message> updated;
	// Errors from the compiler come first: they always block running the game.
	for (const Message &message : collect_build_messages()) {
		updated.push_back(message);
	}
	for (const Message &message : collect_diagnostics()) {
		updated.push_back(message);
	}

	// Only report diagnostics that were not part of the previous pass, so the
	// 0.4 second poll does not repeat the same error in the Output panel.
	HashSet<String> previous;
	for (const Message &message : messages) {
		previous.insert(vformat("%s|%d|%d|%s", message.path, message.line, message.column, message.text));
	}
	for (const Message &message : updated) {
		if (previous.has(vformat("%s|%d|%d|%s", message.path, message.line, message.column, message.text))) {
			continue;
		}
		_report_to_output(message);
	}
	messages = updated;
}

void HaxeEditorPlugin::_update_build_log() {
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	// While the compiler runs, show what it has written so far.
	String text = language == nullptr ? String() : (language->is_building() ? language->get_build_output() : language->get_last_build_log());
	if (text.strip_edges().is_empty()) {
		text = TTR("No build output yet.");
	}
	uint64_t hash = text.hash();
	if (log_initialized && hash == log_hash) {
		return;
	}
	log_initialized = true;
	log_hash = hash;
	log_text->set_text(text);
	// Keep the newest output visible.
	int last_line = MAX(0, log_text->get_line_count() - 1);
	log_text->set_caret_line(last_line);
	log_text->set_caret_column(log_text->get_line(last_line).length());
}

/* ------------------------------------------------------------------- poll -- */

void HaxeEditorPlugin::_update_status() {
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	String text;
	if (toolchain_checked && !toolchain_problem.is_empty()) {
		text = vformat(TTR("Haxe toolchain problem: %s"), toolchain_problem);
	} else if (language != nullptr && !language->get_last_build_error().is_empty()) {
		text = vformat(TTR("Haxe build failed: %s"), language->get_last_build_error());
	} else {
		text = TTR("Haxe is ready.");
	}
	status_label->set_text(text);
}

void HaxeEditorPlugin::_poll() {
	if (run_build_active) {
		// The Run build drives its own session and pumps the main loop; the
		// timer must not consume the session out from under it.
		return;
	}
	HaxeLanguage *language = HaxeLanguage::get_singleton();
	if (language != nullptr && language->has_build_session()) {
		if (language->is_building()) {
			language->poll_build();
			if (language->is_building()) {
				// Still compiling: refresh the log so the output grows live,
				// and leave the other panels alone.
				_update_build_log();
				return;
			}
		}
		HaxeBuild::Result result = language->take_build_result();
		String error;
		_apply_build_result(result, error);
		_set_building(false);
	}

	// Re-runs the in-engine parse pass; unchanged diagnostics are ignored by
	// the signature check in _refresh_messages().
	_refresh_messages();
	_update_build_log();
	_update_status();
}
