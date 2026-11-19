/**************************************************************************/
/*  hxgodot_glue.h                                                        */
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
/*  hxgodot_glue.h                                                        */
/*                                                                        */
/*  Declarations of the functions that generated Haxe code calls. The     */
/*  bodies live in glue.cpp, which is compiled into every project         */
/*  library, so these functions share the library's hxcpp runtime.        */
/*                                                                        */
/*  Generated code reaches them through `@:native(...)` externs declared   */
/*  in hxgodot/Native.hx; the class carries `@:headerCode` pointing here.  */

/* `hxcpp.h` is deliberately not included here. hxcpp compiles with a
 * precompiled header located through the include path, and a second include of
 * `hxcpp.h` fails because only the precompiled form is reachable. Every
 * translation unit that includes this header already includes `<hxcpp.h>`
 * first, so the guard below only documents that requirement. */
#ifndef HXCPP_H
#error "include <hxcpp.h> before hxgodot_glue.h"
#endif

#include "hxgodot_abi.h"

/* Property and method access on engine objects. */
Dynamic hxgodot_haxe_call(String p_object_id, String p_method, Array<Dynamic> p_args);
Dynamic hxgodot_haxe_call_static(String p_class, String p_method, Array<Dynamic> p_args);
Dynamic hxgodot_haxe_get_property(String p_object_id, String p_name);
void hxgodot_haxe_set_property(String p_object_id, String p_name, Dynamic p_value);
bool hxgodot_haxe_has_method(String p_object_id, String p_method);
bool hxgodot_haxe_has_property(String p_object_id, String p_name);

/* Calls a built-in value method through Variant. */
Dynamic hxgodot_haxe_variant_call(Dynamic p_target, String p_method, Array<Dynamic> p_args);
Dynamic hxgodot_haxe_variant_call_static(String p_type, String p_method, Array<Dynamic> p_args);

/* Calls a non-const built-in method and copies the updated primary fields
 * back into the wrapper. */
Dynamic hxgodot_haxe_variant_call_mutating(Dynamic p_target, String p_method, Array<Dynamic> p_args);

/* Derived members of built-in values (Rect2.end, Color.h, ...). Writing one
 * asks the engine for the updated value and copies its primary fields back
 * into the wrapper. */
Dynamic hxgodot_haxe_get_member(Dynamic p_target, String p_name);
void hxgodot_haxe_set_member(Dynamic p_target, String p_name, Dynamic p_value);

/* Variant comparison with engine semantics (Variant::evaluate). Used by
 * hxgodot.VariantOps so Haxe sees the same equality and ordering rules as
 * GDScript for numbers, strings, math values, arrays and dictionaries. */
bool hxgodot_haxe_value_equal(Dynamic p_a, Dynamic p_b);
int hxgodot_haxe_value_compare(Dynamic p_a, Dynamic p_b);

/* Object creation and inspection. */
Dynamic hxgodot_haxe_create_object(String p_class);
Dynamic hxgodot_haxe_singleton(String p_name);

/* Generated `new()` support. new_object returns the engine instance id, or an
 * empty string when the object is created by the engine (script instances).
 * bind_wrapper stores the id on the wrapper and keeps RefCounted values
 * pinned for as long as it lives. */
String hxgodot_haxe_new_object(String p_class);
void hxgodot_haxe_bind_wrapper(Dynamic p_wrapper, String p_object_id);
bool hxgodot_haxe_in_script_creation();

/* Calls a Haxe closure stored by this library. */
Dynamic hxgodot_haxe_call_closure(int p_closure, Array<Dynamic> p_args);
bool hxgodot_haxe_is_valid(String p_object_id);
String hxgodot_haxe_class_name(String p_object_id);
bool hxgodot_haxe_is_ref_counted(String p_object_id);

/* Haxe closures handed to the engine as Callable values. `attach_*` installs
 * the GC finalizer that releases the native resource when the Haxe wrapper
 * object dies. */
int hxgodot_haxe_store_closure(Dynamic p_function);
void hxgodot_haxe_attach_closure_finalizer(Dynamic p_wrapper, int p_closure);
void hxgodot_haxe_attach_handle_finalizer(Dynamic p_wrapper, int p_handle);

/* Diagnostics. */
void hxgodot_haxe_log(int p_level, String p_message);
String hxgodot_haxe_engine_version();
String hxgodot_haxe_type_name(Dynamic p_value);
