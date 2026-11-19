/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "haxe_build.h"
#include "haxe_language.h"
#include "haxe_project.h"
#include "haxe_script.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#include "editor/haxe_editor_plugin.h"
#include "editor/haxe_export_plugin.h"
#include "editor/haxe_highlighter.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/script/script_editor_plugin.h"
#endif

static HaxeLanguage *haxe_language = nullptr;
static Ref<ResourceFormatLoaderHaxe> haxe_resource_loader;
static Ref<ResourceFormatSaverHaxe> haxe_resource_saver;

#ifdef TOOLS_ENABLED
static void _haxe_editor_init() {
	// Web exports need Emscripten, which is normally installed outside the
	// editor's PATH: a desktop launch does not source `emsdk_env.sh`, so
	// `emcc --version` working in a terminal says nothing about the editor's
	// environment.
	HaxeBuild::register_web_editor_settings();

	// The export plugin compiles and packages the game library every time the
	// project is exported.
	Ref<HaxeExportPlugin> haxe_export;
	haxe_export.instantiate();
	EditorExport::get_singleton()->add_export_plugin(haxe_export);

	// The highlighter is instantiated once and cloned per editor instance by
	// the script editor.
	Ref<HaxeSyntaxHighlighter> haxe_syntax_highlighter;
	haxe_syntax_highlighter.instantiate();
	ScriptEditor::get_singleton()->register_syntax_highlighter(haxe_syntax_highlighter);
}
#endif

void initialize_haxe_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		GDREGISTER_CLASS(HaxeScript);
		GDREGISTER_CLASS(HaxeLanguage);

		HaxeProject::register_project_settings();

		haxe_language = memnew(HaxeLanguage);
		ScriptServer::register_language(haxe_language);

		haxe_resource_loader.instantiate();
		ResourceLoader::add_resource_format_loader(haxe_resource_loader);

		haxe_resource_saver.instantiate();
		ResourceSaver::add_resource_format_saver(haxe_resource_saver);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		EditorNode::add_init_callback(_haxe_editor_init);

		// `--build-solutions` compiles the project's editor library, so a
		// broken Haxe script fails the editor launch instead of appearing
		// later as a runtime error.
		EditorNode::add_build_callback(HaxeEditorPlugin::build_solutions);
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(HaxeSyntaxHighlighter);
		EditorPlugins::add_by_type<HaxeEditorPlugin>();
	}
#endif
}

void uninitialize_haxe_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		if (haxe_language != nullptr) {
			ScriptServer::unregister_language(haxe_language);
			memdelete(haxe_language);
			haxe_language = nullptr;
		}

		ResourceLoader::remove_resource_format_loader(haxe_resource_loader);
		haxe_resource_loader.unref();

		ResourceSaver::remove_resource_format_saver(haxe_resource_saver);
		haxe_resource_saver.unref();
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Editor plugins are owned by the editor and torn down with it.
	}
#endif
}
