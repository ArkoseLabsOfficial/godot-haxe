/**************************************************************************/
/*  haxe_support.cpp                                                      */
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

#include "haxe_support.h"

#include "haxe_support.gen.h"

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#ifdef TOOLS_ENABLED
#include "haxe_project.h"

#include "core/config/engine.h"
#endif

namespace {

/* Where support files embedded in the engine are extracted. In the editor they
 * live inside the project's `haxe/build_directory`, next to every other file
 * the module generates, so nothing is written to the user's data directory.
 * An exported game has no writable project directory, so it keeps using the
 * data path there. */
String support_root() {
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		String build = HaxeProject::get_build_dir();
		if (!build.is_empty() && build.is_absolute_path()) {
			return build.path_join("support");
		}
	}
#endif
	String data = OS::get_singleton()->get_data_path();
	return data.is_empty() ? String() : data.path_join("godot").path_join("haxe_support");
}

/* FNV-1a over the file list, used to version the extraction directory. A new
 * engine build with different support files extracts into a fresh directory
 * instead of reusing stale ones. */
uint64_t support_manifest_hash() {
	uint64_t hash = 1469598103934665603ULL;
	for (int i = 0; i < _haxe_support_file_count; i++) {
		const HaxeSupportFile &file = _haxe_support_files[i];
		for (const char *c = file.path; *c != '\0'; c++) {
			hash ^= (uint8_t)*c;
			hash *= 1099511628211ULL;
		}
		hash ^= (uint64_t)file.size;
		hash *= 1099511628211ULL;
	}
	return hash;
}

} // namespace

bool HaxeSupport::is_available() {
	return _haxe_support_file_count > 0;
}

String HaxeSupport::get_dir() {
	static String cached;
	static bool resolved = false;
	if (resolved) {
		return cached;
	}
	resolved = true;

	if (!is_available()) {
		return cached;
	}

	// The support files are extracted into a directory versioned by the
	// manifest hash, so a new engine build never reuses stale files.
	String root = support_root();
	if (root.is_empty()) {
		return cached;
	}
	String target = root.path_join(String::num_uint64(support_manifest_hash(), 16));
	String stamp = target.path_join(".complete");
	if (FileAccess::exists(stamp)) {
		cached = target;
		return cached;
	}

	Error error = DirAccess::make_dir_recursive_absolute(target);
	if (error != OK) {
		ERR_PRINT(vformat("Cannot create the Haxe support directory '%s' (error %d).", target, (int)error));
		return cached;
	}

	for (int i = 0; i < _haxe_support_file_count; i++) {
		const HaxeSupportFile &file = _haxe_support_files[i];
		String path = target.path_join(file.path);
		DirAccess::make_dir_recursive_absolute(path.get_base_dir());

		Vector<uint8_t> buffer;
		buffer.resize(file.size);
		int64_t written = Compression::decompress(buffer.ptrw(), file.size, file.data, file.compressed_size, Compression::MODE_DEFLATE);
		if (written < 0) {
			ERR_PRINT(vformat("Cannot decompress the embedded Haxe file '%s'.", file.path));
			return cached;
		}
		Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE);
		if (output.is_null()) {
			ERR_PRINT(vformat("Cannot write the extracted Haxe file '%s'.", path));
			return cached;
		}
		output->store_buffer(buffer.ptr(), file.size);
	}

	Ref<FileAccess> marker = FileAccess::open(stamp, FileAccess::WRITE);
	if (marker.is_valid()) {
		marker->store_line("ok");
	}
	cached = target;
	return cached;
}
