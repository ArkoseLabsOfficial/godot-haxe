/**************************************************************************/
/*  haxe_export_plugin.h                                                  */
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

#include "core/templates/hash_set.h"
#include "editor/export/editor_export_plugin.h"

/* Packages the compiled Haxe game library into exported builds.
 *
 * A Haxe project has no runtime compiler: the script classes live in a native
 * shared library that `HaxeBuild` produces. An exported game cannot build it,
 * so the library is compiled here, at export time, and stored in the pck under
 * `HaxeProject::get_packaged_library_path()` (or, on Android, in the APK's
 * native library directory). On start the runtime loads it.
 *
 * A native export is supported for the host platform and for Android. hxcpp
 * cross-compiles Android with the NDK; any other target would need a second
 * C++ toolchain, which this module does not provide, so it is reported as an
 * export error instead of silently packaging a library the target cannot load.
 *
 * The `.hx` files themselves are replaced by empty stubs (like Godot's C#
 * export does with its scripts) and the class descriptions travel in a JSON
 * manifest, because a game needs the class metadata but not the source. */
class HaxeExportPlugin : public EditorExportPlugin {
	GDCLASS(HaxeExportPlugin, EditorExportPlugin);

	/* Set once `_export_begin()` packaged the script manifest. Only then may
	 * `_export_file()` replace a source with an empty stub; without the
	 * manifest the game would lose the class description as well. */
	bool metadata_packaged = false;

public:
	virtual String get_name() const override;

	/* True when the export template at `p_path` (an executable, an APK or the
	 * engine AAR of a Gradle build template) carries the Haxe module. Exposed
	 * for the engine tests, which exercise the archive scanning without an
	 * Android template. */
	static bool template_contains_haxe(const String &p_path);

	/* Whether a web template was built with threads: 1 when it was, 0 when it
	 * was not, -1 when the archive has no `godot.js` entry to inspect.
	 *
	 * A page and its Haxe library have to agree on this, because a threaded
	 * build gives the engine a shared linear memory and a single-threaded
	 * library declares a non-shared one; the browser refuses to instantiate
	 * the module ("mismatch in shared state of memory"). The stock template
	 * filenames encode the variant, but a custom template can be either.
	 * Exposed for the engine tests. */
	static int web_template_threads(const String &p_path);

	/* Whether the template at `p_path` carries this build's host API marker
	 * (`HXGODOT_TEMPLATE_ABI_MARKER`). A game refuses a library built for a
	 * different host API version, so an export whose template disagrees is
	 * refused before it is written. Exposed for the engine tests. */
	static bool template_host_abi_matches(const String &p_path);

	/* Whether the template carries any host API marker at all. False for a Haxe
	 * template built before the marker existed, which cannot be classified and
	 * only warns. Exposed for the engine tests. */
	static bool template_declares_host_abi(const String &p_path);

	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	virtual void _export_end() override;

	/* The export dialog's "Haxe" section. The project settings are the
	 * baseline; these let one preset differ (for example a release preset with
	 * `-dce std` and an extra `-D RELEASE`) without editing the project. */
	virtual void _get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const override;
	virtual String _get_export_option_warning(const Ref<EditorExportPlatform> &p_export_platform, const String &p_option_name) const override;
};
