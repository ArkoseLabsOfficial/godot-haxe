/**************************************************************************/
/*  haxe_completion.h                                                     */
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

#include "core/object/script_language.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

/* Code completion and symbol lookup for Haxe scripts.
 *
 * Everything is resolved inside the engine: engine binding metadata
 * (`api.json`) and the project source index provide completion, member lists
 * and "go to definition", so no external Haxe installation is needed.
 */
Error haxe_complete_code(const String &p_code, const String &p_path, Object *p_owner, List<ScriptLanguage::CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint);
Error haxe_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, ScriptLanguage::LookupResult &r_result);

/* Resolves the Haxe type of a dotted expression (`self.position`) inside a
 * script. Used by completion and by the editor plugin to find the documentation
 * page of a member. Returns an empty string when the type cannot be derived.
 */
String haxe_resolve_member_type(const Vector<String> &p_path, const String &p_script_path);

/* Splits `a.b.c` into segments. */
Vector<String> haxe_split_expression(const String &p_expression);
