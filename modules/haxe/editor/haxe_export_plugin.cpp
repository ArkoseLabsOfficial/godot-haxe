/**************************************************************************/
/*  haxe_export_plugin.cpp                                                */
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

#include "haxe_export_plugin.h"

#include "../haxe_build.h"
#include "../haxe_project.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/zip_io.h"
#include "core/os/os.h"
#include "core/string/translation.h"
#include "core/version.h"
#include "editor/export/export_template_manager.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"

#include <cstring>

namespace {

/* Any binary built with this module registers the `HaxeScript` class, so the
 * class name is present in its read-only data. Official export templates ship
 * no Haxe module, so searching an export template for this string tells a
 * Haxe-enabled template apart from one that would fail at runtime with a
 * confusing "no loader found for resource" error. */
const char *const HAXE_TEMPLATE_MARKER = "HaxeScript";

/* The ABIs Godot's Android exporter can package, in the order it reports
 * them. The names are also the hxcpp target that produces each library. */
const char *const ANDROID_ABIS[] = { "armeabi-v7a", "arm64-v8a", "x86", "x86_64" };

bool has_android_ndk_layout(const String &p_path) {
	return FileAccess::exists(p_path.path_join("source.properties")) &&
			DirAccess::dir_exists_absolute(p_path.path_join("toolchains/llvm/prebuilt"));
}

/* `ANDROID_NDK_DIR` usually points at the directory that holds several
 * versioned NDKs; pick the newest one that has a usable toolchain. */
String find_versioned_ndk(const String &p_directory) {
	Ref<DirAccess> dir = DirAccess::open(p_directory);
	if (dir.is_null()) {
		return String();
	}
	Vector<String> candidates;
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (!dir->current_is_dir() || name.begins_with(".")) {
			continue;
		}
		String candidate = p_directory.path_join(name);
		if (has_android_ndk_layout(candidate)) {
			candidates.push_back(candidate);
		}
	}
	dir->list_dir_end();
	if (candidates.is_empty()) {
		return String();
	}
	candidates.sort();
	return candidates[candidates.size() - 1];
}

String find_android_ndk_root() {
	for (const char *variable : { "ANDROID_NDK_ROOT", "ANDROID_NDK_DIR" }) {
		String value = OS::get_singleton()->get_environment(variable).strip_edges();
		if (value.is_empty()) {
			continue;
		}
		if (has_android_ndk_layout(value)) {
			return value;
		}
		String versioned = find_versioned_ndk(value);
		if (!versioned.is_empty()) {
			return versioned;
		}
	}
	String sdk = OS::get_singleton()->get_environment("ANDROID_SDK").strip_edges();
	if (sdk.is_empty()) {
		sdk = String(EDITOR_GET("export/android/android_sdk_path")).strip_edges();
	}
	if (!sdk.is_empty()) {
		String versioned = find_versioned_ndk(sdk.path_join("ndk"));
		if (!versioned.is_empty()) {
			return versioned;
		}
		if (has_android_ndk_layout(sdk.path_join("ndk-bundle"))) {
			return sdk.path_join("ndk-bundle");
		}
	}
	return String();
}

/* The installed export template with this name, or an empty string.
 * `EditorExportPlatform::find_export_template` is protected, and it only joins
 * the templates directory, the engine version and the file name. */
String find_installed_template(const String &p_name) {
	EditorPaths *paths = EditorPaths::get_singleton();
	if (paths == nullptr) {
		return String();
	}
	const String path = paths->get_export_templates_dir().path_join(GODOT_VERSION_FULL_CONFIG).path_join(p_name);
	return FileAccess::exists(path) ? path : String();
}

/* Whether `p_needle` occurs in a file or, when `p_path` is a directory, in any
 * file below it. Android puts its engine library inside an APK or an AAR from
 * the Gradle build template, so the marker can be one archive level down. */
bool path_contains_marker(const String &p_path, const char *p_needle, int p_depth = 0);

/* Whether an APK/AAR/web template contains a native library or WebAssembly
 * module carrying `p_needle`. Godot's Android templates store their libraries
 * compressed (`useLegacyPackaging` is on for the minimum SDK Godot targets) and
 * a web template is a zip, so the marker is never visible in the archive bytes:
 * every binary entry has to be inflated and searched. */
bool archive_contains_marker(const String &p_path, const char *p_needle) {
	const int needle_length = (int)strlen(p_needle);
	if (needle_length == 0) {
		return true;
	}
	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile package = unzOpen2(p_path.utf8().get_data(), &io);
	if (package == nullptr) {
		return false;
	}

	const int chunk_size = 1 << 20;
	Vector<uint8_t> buffer;
	buffer.resize(chunk_size + needle_length);
	bool found = false;
	for (int ret = unzGoToFirstFile(package); ret == UNZ_OK && !found; ret = unzGoToNextFile(package)) {
		unz_file_info64 info;
		String entry;
		if (godot_unzip_get_current_file_info(package, info, entry) != UNZ_OK) {
			continue;
		}
		// Android libraries and the web engine side module.
		if (!entry.ends_with(".so") && !entry.ends_with(".wasm")) {
			continue;
		}
		if (unzOpenCurrentFile(package) != UNZ_OK) {
			continue;
		}
		int carry = 0;
		while (true) {
			const int read = unzReadCurrentFile(package, buffer.ptrw() + carry, chunk_size);
			if (read <= 0) {
				break;
			}
			const int total = carry + read;
			for (int i = 0; i + needle_length <= total; i++) {
				if (memcmp(buffer.ptr() + i, p_needle, needle_length) == 0) {
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
			carry = MIN(needle_length - 1, total);
			memmove(buffer.ptrw(), buffer.ptr() + total - carry, carry);
		}
		unzCloseCurrentFile(package);
	}
	unzClose(package);
	return found;
}

/* Whether a zip archive has an entry with this exact name. A web template built
 * with `dlink_enabled=yes` carries the engine as a side module; without it the
 * page has no dynamic linker to load the Haxe module. */
bool archive_has_entry(const String &p_path, const String &p_entry) {
	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile package = unzOpen2(p_path.utf8().get_data(), &io);
	if (package == nullptr) {
		return false;
	}
	bool found = false;
	for (int ret = unzGoToFirstFile(package); ret == UNZ_OK && !found; ret = unzGoToNextFile(package)) {
		unz_file_info64 info;
		String entry;
		if (godot_unzip_get_current_file_info(package, info, entry) != UNZ_OK) {
			continue;
		}
		found = entry == p_entry;
	}
	unzClose(package);
	return found;
}

bool file_contains_marker(const String &p_path, const char *p_needle) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	const int needle_length = (int)strlen(p_needle);
	if (needle_length == 0) {
		return true;
	}
	// A zip-based template (APK, AAR) has to be searched entry by entry.
	if (file->get_length() >= 4) {
		uint8_t magic[4];
		if (file->get_buffer(magic, 4) == 4 && magic[0] == 'P' && magic[1] == 'K') {
			return archive_contains_marker(p_path, p_needle);
		}
		file->seek(0);
	}
	const int chunk_size = 1 << 20;
	Vector<uint8_t> buffer;
	buffer.resize(chunk_size + needle_length);
	int carry = 0;
	while (true) {
		const int read = file->get_buffer(buffer.ptrw() + carry, chunk_size);
		if (read <= 0) {
			break;
		}
		const int total = carry + read;
		for (int i = 0; i + needle_length <= total; i++) {
			if (memcmp(buffer.ptr() + i, p_needle, needle_length) == 0) {
				return true;
			}
		}
		carry = MIN(needle_length - 1, total);
		memmove(buffer.ptrw(), buffer.ptr() + total - carry, carry);
	}
	return false;
}

bool path_contains_marker(const String &p_path, const char *p_needle, int p_depth) {
	if (!DirAccess::dir_exists_absolute(p_path)) {
		return file_contains_marker(p_path, p_needle);
	}
	// The Gradle build template nests the engine archive a couple of
	// directories deep (app/libs/<target>/godot-lib.<target>.aar).
	if (p_depth > 4) {
		return false;
	}
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return false;
	}
	Vector<String> entries;
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		entries.push_back(p_path.path_join(name));
	}
	dir->list_dir_end();
	for (const String &entry : entries) {
		if (path_contains_marker(entry, p_needle, p_depth + 1)) {
			return true;
		}
	}
	return false;
}

/* The compiler diagnostics of a failed build, one per line. `Result::message`
 * only carries a count for a compile failure, so the errors it parsed are what
 * tells the user which file and line to fix. */
String format_build_errors(const HaxeBuild::Result &p_result) {
	return HaxeBuild::format_errors(p_result);
}

} // namespace

String HaxeExportPlugin::get_name() const {
	return "Haxe";
}

bool HaxeExportPlugin::template_contains_haxe(const String &p_path) {
	return path_contains_marker(p_path, HAXE_TEMPLATE_MARKER);
}

int HaxeExportPlugin::web_template_threads(const String &p_path) {
	// Emscripten only emits the dlsync thread entry point in a `-pthread`
	// build, and the name is looked up as a string by the glue, so it survives
	// closure or minification. A single-threaded template has no reason to
	// mention it at all.
	const char *const marker = "_emscripten_dlsync_threads";
	const int marker_length = (int)strlen(marker);

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile package = unzOpen2(p_path.utf8().get_data(), &io);
	if (package == nullptr) {
		return -1;
	}

	int result = -1;
	for (int ret = unzGoToFirstFile(package); ret == UNZ_OK; ret = unzGoToNextFile(package)) {
		unz_file_info64 info;
		String entry;
		if (godot_unzip_get_current_file_info(package, info, entry) != UNZ_OK || entry != "godot.js") {
			continue;
		}
		if (unzOpenCurrentFile(package) != UNZ_OK) {
			break;
		}
		const int chunk_size = 1 << 20;
		Vector<uint8_t> buffer;
		buffer.resize(chunk_size + marker_length);
		int carry = 0;
		bool found = false;
		while (true) {
			const int read = unzReadCurrentFile(package, buffer.ptrw() + carry, chunk_size);
			if (read <= 0) {
				break;
			}
			const int total = carry + read;
			for (int i = 0; i + marker_length <= total; i++) {
				if (memcmp(buffer.ptr() + i, marker, marker_length) == 0) {
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
			carry = MIN(marker_length - 1, total);
			memmove(buffer.ptrw(), buffer.ptr() + total - carry, carry);
		}
		unzCloseCurrentFile(package);
		result = found ? 1 : 0;
		break;
	}
	unzClose(package);
	return result;
}

void HaxeExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	(void)p_features;
	(void)p_path;
	(void)p_flags;

	metadata_packaged = false;

	Ref<EditorExportPlatform> platform = get_export_platform();
	if (platform.is_null()) {
		return;
	}

	// A project without Haxe sources is not this module's business; leaving it
	// alone keeps an ordinary Godot project exporting exactly as before.
	if (HaxeProject::scan_sources().is_empty()) {
		return;
	}

	const String host = OS::get_singleton()->get_name();
	const String target = platform->get_os_name();
	const bool android = target == "Android";
	const bool web = target == "Web";
	// hxcpp produces a native shared library: for the host, for Android (which
	// hxcpp cross-compiles with the NDK), or for the web (which hxcpp compiles
	// with Emscripten into a WebAssembly side module). Any other target would
	// need another C++ toolchain, which this module does not provide, so it is
	// refused instead of packaging a library the target cannot load.
	if (!android && !web && target != host) {
		platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
				vformat("The Haxe library is compiled for the host platform (%s), for Android and for the web, so it cannot be packaged for %s.", host, target));
		return;
	}

	Ref<EditorExportPreset> preset = get_export_preset();

	Vector<String> abis;
	String ndk_root;
	// Which web variant the preset uses; only meaningful for a web export.
	String web_target = HaxeProject::WEB_TARGET;
	if (android) {
		for (const char *abi : ANDROID_ABIS) {
			if (preset.is_valid() && (bool)preset->get(vformat("architectures/%s", abi))) {
				abis.push_back(abi);
			}
		}
		if (abis.is_empty()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					TTR("No Android architecture is enabled, so no Haxe library can be packaged. Enable at least one architecture in the preset."));
			return;
		}

		// The game has to be able to load the Haxe library. A template built
		// from this engine carries the Haxe module; an official one does not,
		// and using it produces a game that fails later with "no loader found
		// for resource: ...Main.hx" or a missing native symbol, so the template
		// is checked here instead.
		const bool gradle_build = preset.is_valid() && (bool)preset->get("gradle_build/use_gradle_build");
		String template_path;
		if (!gradle_build && preset.is_valid()) {
			template_path = preset->get(p_debug ? "custom_template/debug" : "custom_template/release");
		}
		if (gradle_build) {
			const String build_directory = ExportTemplateManager::get_android_build_directory(preset);
			if (!DirAccess::dir_exists_absolute(build_directory)) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("This export uses a Gradle build, but the Android build template is not installed ('%s'). Install it from the Project menu and build it with a Godot engine that includes the Haxe module.", build_directory));
				return;
			}
			// The engine library lives in an AAR inside the installed build
			// template. If it cannot be identified, the export continues with a
			// warning rather than guessing; the user is told what to do.
			if (!template_contains_haxe(build_directory.path_join("app").path_join("libs")) &&
					!template_contains_haxe(build_directory.path_join("libs"))) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_WARNING, TTR("Haxe"),
						TTR("Could not confirm that the Android build template contains the Haxe module. If the game reports that Haxe scripts cannot be loaded, rebuild the Android build template from this engine (Project > Install Android Build Template)."));
			}
		} else {
			if (template_path.strip_edges().is_empty()) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("This project uses Haxe, but the export preset has no custom %s template, so the stock template is used and it cannot load Haxe scripts. "
								"Build an Android template from this engine (for example 'scons platform=android target=template_release') and set it as the preset's custom template.",
								p_debug ? "debug" : "release"));
				return;
			}
			if (!FileAccess::exists(template_path)) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("The custom export template '%s' does not exist.", template_path));
				return;
			}
			// An Android template is an APK whose native libraries are stored
			// deflated, so the marker check inflates the archive entries.
			if (!template_contains_haxe(template_path)) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("The export template '%s' was not built with the Haxe module, so the exported game could not load Haxe scripts. "
								"Build an Android template from this engine and set it as the preset's custom template.",
								template_path));
				return;
			}
		}

		ndk_root = find_android_ndk_root();
		if (ndk_root.is_empty()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					TTR("The Android NDK was not found, so the Haxe library cannot be cross-compiled. Set ANDROID_NDK_ROOT (or ANDROID_NDK_DIR), or configure 'export/android/android_sdk_path' in the Editor Settings."));
			return;
		}
	} else if (web) {
		// The Haxe library is a WebAssembly side module, so the export needs a
		// template built with dynamic linking. In the web preset that is the
		// "Extensions Support" variant, which also selects the `web_dlink_*`
		// template name.
		if (preset.is_valid() && !(bool)preset->get("variant/extensions_support")) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					TTR("This project uses Haxe, whose game code is a WebAssembly module loaded at run time, but the preset was exported without 'Extensions Support'. Enable it in the web preset so a template with dynamic linking is used."));
			return;
		}

		// A threads-enabled template links its engine module with `-pthread`,
		// which makes its linear memory shared. A wasm module declares at link
		// time whether it shares the memory, and the browser refuses to
		// instantiate one whose declaration disagrees with the memory it is
		// given, so the Haxe library has to be linked for the same variant.
		const bool threads = preset.is_valid() && (bool)preset->get("variant/thread_support");
		web_target = threads ? HaxeProject::WEB_THREADS_TARGET : HaxeProject::WEB_TARGET;

		String template_path;
		if (preset.is_valid()) {
			template_path = String(preset->get(p_debug ? "custom_template/debug" : "custom_template/release")).strip_edges();
		}
		if (template_path.is_empty()) {
			const String name = vformat("web_dlink%s_%s.zip", threads ? "" : "_nothreads", p_debug ? "debug" : "release");
			template_path = find_installed_template(name);
			if (template_path.is_empty()) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("The web export template '%s' is not installed, so the game cannot load Haxe scripts. Build one from this engine with 'scons platform=web target=%s dlink_enabled=yes%s', install it in the export templates directory, or set it as the preset's custom template.",
								name, p_debug ? "template_debug" : "template_release", threads ? String() : String(" threads=no")));
				return;
			}
		}
		if (!FileAccess::exists(template_path)) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The custom export template '%s' does not exist.", template_path));
			return;
		}
		// `dlink_enabled` moves the engine into `godot.side.wasm` and leaves a
		// small runtime as the main module. Without that split there is no
		// dynamic linker on the page, so the Haxe module cannot be loaded.
		if (!archive_has_entry(template_path, "godot.side.wasm")) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The web export template '%s' was not built with dynamic linking, so the exported game could not load the Haxe module. Build it with 'dlink_enabled=yes' (the 'Extensions Support' variant) from this engine.",
							template_path));
			return;
		}
		// A custom template can be the other variant from the one the preset
		// selects. A threaded build gives the engine a shared linear memory,
		// and a single-threaded Haxe library declares a non-shared one (and the
		// reverse), so the browser refuses to instantiate it with "mismatch in
		// shared state of memory" — after the page has started, as a load
		// failure rather than an export error.
		const int template_threads = HaxeExportPlugin::web_template_threads(template_path);
		if (template_threads >= 0 && (template_threads == 1) != threads) {
			const String expected = vformat("web_dlink%s_%s.zip", threads ? "" : "_nothreads", p_debug ? "debug" : "release");
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The web export template '%s' is a %s build, but 'Thread Support' is %s in the preset, so the Haxe library is compiled as a %s module and the page would reject it at load time ('mismatch in shared state of memory'). Set the preset's custom %s template to '%s', or change 'Thread Support' to match the template.",
							template_path, template_threads == 1 ? "multi-threaded" : "single-threaded",
							threads ? "enabled" : "disabled",
							threads ? "multi-threaded" : "single-threaded",
							p_debug ? "debug" : "release", expected));
			return;
		}
		// The template also has to contain this module, otherwise the game
		// cannot load `.hx` resources at all.
		if (!template_contains_haxe(template_path)) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The web export template '%s' was not built with the Haxe module, so the exported game could not load Haxe scripts. Build a template from this engine and set it as the preset's custom template.",
							template_path));
			return;
		}
	} else {
		// The game has to be able to load `.hx` resources, which only a
		// template built with this module can do.
		String template_path;
		if (preset.is_valid()) {
			template_path = preset->get(p_debug ? "custom_template/debug" : "custom_template/release");
		}
		if (template_path.strip_edges().is_empty()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("This project uses Haxe, but the export preset has no custom %s template, so the stock template is used and it cannot load Haxe scripts. "
							"Build a template from this engine (for example 'scons platform=linuxbsd target=template_release') and set it as the preset's custom template.",
							p_debug ? "debug" : "release"));
			return;
		}
		if (!FileAccess::exists(template_path)) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The custom export template '%s' does not exist.", template_path));
			return;
		}
		if (!template_contains_haxe(template_path)) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The export template '%s' was not built with the Haxe module, so the exported game could not load Haxe scripts. "
							"Build a template from this engine and set it as the preset's custom template.",
							template_path));
			return;
		}
	}

	// The game library (no editor-only classes) is what an exported game
	// loads; debug exports carry the debug configuration, release exports the
	// release one.
	const bool release = !p_debug;
	if (android) {
		for (const String &abi : abis) {
			HaxeBuild::Result result = HaxeBuild::build(release, false, false, abi, ndk_root);
			if (!result.success) {
				String message = vformat("The Haxe project could not be compiled for Android %s, so the exported game cannot run its scripts.\n%s", abi, result.message);
				String errors = format_build_errors(result);
				if (!errors.is_empty() && !message.contains(errors)) {
					message += "\n" + errors;
				}
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"), message);
				return;
			}
			if (!FileAccess::exists(result.library_path)) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
						vformat("The compiled Haxe library for Android %s is missing at '%s'.", abi, result.library_path));
				return;
			}
			// Godot stores the library under lib/<abi>/ in the APK, and the
			// name must keep the `lib` prefix so the dynamic linker can resolve
			// the soname the game opens.
			add_shared_object(result.library_path, Vector<String>{ abi });
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_INFO, TTR("Haxe"),
					vformat("Packaged the compiled Haxe library for Android %s ('%s').", abi, result.library_path.get_file()));
		}
	} else if (web) {
		HaxeBuild::Result result = HaxeBuild::build(release, false, false, web_target);
		if (!result.success) {
			String message = vformat("The Haxe project could not be compiled for the web, so the exported game cannot run its scripts.\n%s", result.message);
			String errors = format_build_errors(result);
			if (!errors.is_empty() && !message.contains(errors)) {
				message += "\n" + errors;
			}
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"), message);
			return;
		}
		if (!FileAccess::exists(result.library_path)) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The compiled Haxe library for the web is missing at '%s'.", result.library_path));
			return;
		}
		// The web exporter copies every shared object next to index.html and
		// lists it in the page configuration, so `dlopen("<Name>.so")` finds the
		// module by name in the browser's virtual file system.
		add_shared_object(result.library_path, Vector<String>());
		platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_INFO, TTR("Haxe"),
				vformat("Packaged the compiled Haxe library for the web ('%s').", result.library_path.get_file()));
	} else {
		HaxeBuild::Result result = HaxeBuild::build(release, false, false);
		if (!result.success) {
			String message = vformat("The Haxe project could not be compiled, so the exported game cannot run its scripts.\n%s", result.message);
			String errors = format_build_errors(result);
			if (!errors.is_empty() && !message.contains(errors)) {
				message += "\n" + errors;
			}
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"), message);
			return;
		}

		Ref<FileAccess> file = FileAccess::open(result.library_path, FileAccess::READ);
		if (file.is_null()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The compiled Haxe library is missing at '%s'.", result.library_path));
			return;
		}
		Vector<uint8_t> contents = file->get_buffer(file->get_length());
		if (contents.is_empty()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("Haxe"),
					vformat("The compiled Haxe library at '%s' is empty.", result.library_path));
			return;
		}

		const String packaged = HaxeProject::get_packaged_library_path();
		add_file(packaged, contents, false);
		platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_INFO, TTR("Haxe"),
				vformat("Packaged the compiled Haxe library as '%s'.", packaged));
	}

	// The class descriptions the game needs to attach and resolve scripts. The
	// `.hx` resources are replaced by empty stubs in `_export_file()`, so this
	// manifest is what keeps the engine's class discovery working without
	// shipping the source.
	add_file(HaxeProject::get_packaged_metadata_path(), HaxeProject::serialize_script_metadata().to_utf8_buffer(), false);
	metadata_packaged = true;
}

void HaxeExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	(void)p_type;
	(void)p_features;

	// A game needs the compiled class and its metadata, not the Haxe source.
	// An empty line is used instead of an empty file because an empty resource
	// makes the engine emit a spurious error (the same trick Godot's C#
	// exporter uses).
	if (!metadata_packaged || p_path.get_extension().to_lower() != "hx") {
		return;
	}
	const Vector<uint8_t> stub = String("\n").to_utf8_buffer();
	add_file(p_path, stub, false);
	skip();
}

void HaxeExportPlugin::_export_end() {
	metadata_packaged = false;
}
