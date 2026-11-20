/**************************************************************************/
/*  test_haxe_utils.h                                                     */
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

/* Declarations of the helpers the Haxe test cases share. They live in their
 * own header so that `test_haxe.cpp` can define them without including
 * `test_haxe.h`: the module build compiles every header under `tests/` into
 * `modules_tests.gen.h` as well, so a header that held both the helpers and the
 * test cases would register each case twice, once per translation unit. */

#include "core/templates/hash_set.h"
#include "core/variant/variant.h"

namespace TestHaxe {

/* Creates a scratch directory for source files and returns its path. */
String make_scratch_dir();

/* Writes `p_contents` to `<p_dir>/<p_name>`, returning the absolute path. */
String write_source(const String &p_dir, const String &p_name, const String &p_contents);

/* Removes a directory created by `make_scratch_dir`. */
void remove_scratch_dir(const String &p_dir);

/* Points the project's Haxe source directory at `p_dir` while this object
 * exists, so that the source index and the completion resolve classes from the
 * scratch project, then restores the previous setting. */
class SourceDirScope {
public:
	explicit SourceDirScope(const String &p_dir);
	~SourceDirScope();

private:
	bool had_setting = false;
	Variant previous;
};

/* Runs completion on `p_code` and returns the displayed option names. */
HashSet<String> completion_names(const String &p_code, const String &p_path = String());

} // namespace TestHaxe
