/**************************************************************************/
/*  haxe_libraries_panel.h                                                */
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

#include "../haxe_build.h"

#include "scene/gui/control.h"

class Button;
class CheckBox;
class FileDialog;
class Label;
class LineEdit;
class OptionButton;
class ProgressBar;
class Timer;
class Tree;

/* Per-project Haxe library manager, embedded as the "Haxelib" Project Settings
 * tab.
 *
 * Install (from haxelib, from a Git repository) and version changes, update
 * and remove all run the real haxelib executable with HAXELIB_PATH pointing at
 * the project repository, so the machine wide installation is never modified.
 * The dependency list itself lives in `haxe.json`, the file the build pipeline
 * resolves, and libraries that were copied into the project `haxelib/`
 * directory by hand are listed as well.
 *
 * The commands are driven from a timer instead of waiting for haxelib: a
 * download can take minutes, and the editor must stay responsive while it
 * runs. The progress bar follows the percentage haxelib prints.
 */
class HaxeLibrariesPanel : public Control {
	GDCLASS(HaxeLibrariesPanel, Control)

	enum Operation {
		OPERATION_NONE,
		OPERATION_INSTALL,
		OPERATION_UPDATE,
		OPERATION_REMOVE,
		OPERATION_GIT,
		OPERATION_SET_VERSION,
	};

	Tree *tree = nullptr;
	LineEdit *name_edit = nullptr;
	LineEdit *version_edit = nullptr;
	LineEdit *url_edit = nullptr;
	LineEdit *branch_edit = nullptr;
	CheckBox *skip_dependencies = nullptr;
	Button *install_button = nullptr;
	Button *git_button = nullptr;
	OptionButton *version_option = nullptr;
	Button *switch_version_button = nullptr;
	Button *update_button = nullptr;
	Button *remove_button = nullptr;
	Button *refresh_button = nullptr;
	Button *open_source_button = nullptr;
	FileDialog *source_dialog = nullptr;
	Label *status = nullptr;
	ProgressBar *progress = nullptr;
	Timer *poll_timer = nullptr;

	/* The library source last opened in the script editor, so a version switch
	 * can reopen the same file from the newly selected version. */
	String opened_source_library;
	String opened_source_relative;

	Vector<HaxeBuild::Library> libraries;

	HaxeProcess *process = nullptr;
	Operation operation = OPERATION_NONE;
	String operation_name;
	String operation_version;

	/* Versions released for the selected library, read in the background with
	 * `haxelib info` so the editor never blocks on the network. They are merged
	 * with the versions unpacked in the repository to fill the version list. */
	HaxeProcess *info_process = nullptr;
	String info_query_name;
	Vector<String> info_versions;

	void _refresh();
	void _install();
	void _install_git();
	void _switch_version();
	void _update();
	void _remove();
	void _open_source();
	void _on_source_selected(const String &p_path);
	void _reopen_source_after_switch(const String &p_library);
	void _on_selection_changed();
	int _selected_index() const;
	void _set_status(const String &p_text);
	/* Fills the version list of the selected library from the versions on disk
	 * and the ones `haxelib info` reported. */
	void _populate_versions();
	/* Refreshes the script editor's recorded file times after this panel
	 * rewrote `haxe.json`, so an open tab does not look modified on disk. */
	void _refresh_open_text_times();
	/* Starts reading the released versions of `p_name` in the background. */
	void _start_info_query(const String &p_name);
	/* Reads the running `haxelib info` output and finishes the query. */
	void _poll_info();
	/* Starts `p_operation` for one library without waiting for haxelib. */
	void _start_operation(Operation p_operation, const String &p_name, const String &p_version, const String &p_url = String(), const String &p_branch = String());
	/* Reads the running haxelib output and finishes the operation. */
	void _poll_operation();
	void _finish_operation();
	void _set_busy(bool p_busy);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	HaxeLibrariesPanel();
	~HaxeLibrariesPanel();

	/* Reloads the library list from disk. */
	void refresh();
};
