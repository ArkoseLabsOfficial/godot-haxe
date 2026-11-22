/**************************************************************************/
/*  haxe_abi.h                                                            */
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

/* The ABI is defined once and shipped with the engine support files, so the
 * module and every project library are compiled against the exact same
 * contract. See modules/haxe/dist/toolchain/hxgodot_abi.h. */
#include "dist/toolchain/hxgodot_abi.h"

/* A byte string an exported game's engine binary carries, naming the host API
 * version it was built for. The export plugin cannot read a template's symbols,
 * so the version is searched for as text: a template built from another engine
 * revision cannot load a library this editor compiled, and the export is
 * refused instead of shipping a game that starts with "incompatible host API
 * version". The trailing `;` keeps `...=1;` from matching `...=12;`.
 * `register_types.cpp` passes the marker to `print_verbose()`, which is what
 * keeps it in the binary. */
#define HXGODOT_STRINGIFY_DETAIL(value) #value
#define HXGODOT_STRINGIFY(value) HXGODOT_STRINGIFY_DETAIL(value)
#define HXGODOT_TEMPLATE_ABI_MARKER "hxgodot-abi=" HXGODOT_STRINGIFY(HXGODOT_ABI_VERSION) ";"
