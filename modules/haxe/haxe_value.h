/**************************************************************************/
/*  haxe_value.h                                                          */
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

#include "haxe_abi.h"

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

/* Owns the memory referenced by an HxValue. The value stays valid while the
 * holder is alive, which is exactly the lifetime required by the ABI. */
struct HxValueHolder {
	HxValue value = {};
	CharString text;
	Vector<uint8_t> blob;
};

/* Converts engine values to the ABI representation. `p_own_objects` hands one
 * owned reference for RefCounted objects to the receiver; it is set for values
 * the library keeps (return values), and left off when the sender's own
 * Variant keeps the object alive for the whole call (arguments). */
void hx_value_from_variant(const Variant &p_value, HxValueHolder &r_holder, bool p_own_objects = false);
void hx_value_encode_variant(const Variant &p_value, Vector<uint8_t> &r_blob, bool p_own_objects = false);

/* Converts the ABI representation to engine values. Returns false when the
 * value is malformed or references a dead engine object. */
bool hx_value_to_variant(const HxValue &p_value, Variant &r_value);
bool hx_value_decode_variant(const uint8_t *p_data, int64_t p_size, int64_t &r_offset, Variant &r_value);

/* Engine type names used by labels and error messages. */
String hx_value_type_name(int p_type);
