/**************************************************************************/
/*  haxe_editor_plugin.h                                                  */
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

#include "core/templates/vector.h"
#include "editor/plugins/editor_plugin.h"

class Button;
class HaxeLibrariesPanel;
class Label;
class ProgressBar;
class TextEdit;
class Timer;
class VBoxContainer;

/* Editor integration for Haxe.
 *
 * The plugin owns the "Haxe" bottom panel, whose content is the build log of
 * the last compilation (read only, so it can be selected and copied). Compiler
 * and parser errors are reported to the Output panel, where the `res://`
 * locations become clickable links. All language tooling (parsing, completion
 * and lookup) runs inside the engine, so no external process is spawned. The
 * per-project haxelib manager lives in the Project Settings dialog, and the
 * plugin registers as an engine build callback so `--build-solutions` compiles
 * the project before the editor starts.
 */
class HaxeEditorPlugin : public EditorPlugin {
	GDCLASS(HaxeEditorPlugin, EditorPlugin);

	enum MessageKind {
		MESSAGE_ERROR,
		MESSAGE_WARNING,
		MESSAGE_INFO,
	};

	struct Message {
		String path;
		int line = 1;
		int column = 1;
		String text;
		MessageKind kind = MESSAGE_ERROR;
	};

	Button *build_button = nullptr;
	Button *rebuild_button = nullptr;
	Button *libraries_button = nullptr;
	VBoxContainer *panel = nullptr;
	TextEdit *log_text = nullptr;
	Label *status_label = nullptr;
	ProgressBar *progress = nullptr;
	HaxeLibrariesPanel *libraries_panel = nullptr;
	Timer *poll_timer = nullptr;

	Vector<Message> build_messages;
	Vector<Message> messages;
	bool toolchain_checked = false;
	String toolchain_problem;
	bool log_initialized = false;
	uint64_t log_hash = 0;
	/* True from the moment a Run build starts until its result has been
	 * applied. The Run build pumps the editor's main loop so the window keeps
	 * drawing, which can deliver a second Run: that one is refused instead of
	 * racing the compiler. */
	bool run_build_active = false;

	/* Turns a finished build into editor messages, a status line and the
	 * build log. Shared by the blocking and the background path. */
	bool _apply_build_result(const HaxeBuild::Result &p_result, String &r_error);
	/* Locks or unlocks the toolbar while a background build runs. */
	void _set_building(bool p_building);
	void _build(bool p_force);
	void _open_libraries();
	void _poll();
	void _refresh_messages();
	void _report_to_output(const Message &p_message);
	void _update_build_log();
	void _update_status();
	Vector<Message> collect_diagnostics() const;
	Vector<Message> collect_build_messages() const;

protected:
	static void _bind_methods() {}

public:
	HaxeEditorPlugin();
	~HaxeEditorPlugin();

	void _notification(int p_what);
	String get_plugin_name() const override;

	/* Runs before the project is played. Compiles the game library so a failed
	 * build never launches a stale binary; returns false to abort the run.
	 * While the compiler runs the editor's main loop is pumped, so the Haxe
	 * panel shows the live output instead of the window appearing frozen. */
	bool build() override;

	/* Registered with EditorNode::add_build_callback. `--build-solutions` uses
	 * the callback to compile the editor library, which is what the editor
	 * loads scripts from; the engine also runs build callbacks from its Run
	 * buttons, and there the game library produced by `build()` is what is
	 * needed, so this does nothing outside a command line build. */
	static bool build_solutions();
};
