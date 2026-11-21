/**************************************************************************/
/*  haxe_elf.h                                                            */
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

#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <cstdint>

/* Reads the parts of an ELF shared object the Haxe module needs to package a
 * compiled library's native dependencies.
 *
 * hxcpp links a project library against whatever shared objects the Haxe
 * libraries it uses need (`libvlc.so` for `hxvlc-godot`, for example). On a
 * desktop target those come from the system, but an Android export has to ship
 * them inside the APK next to the Haxe library. The dependency names live in
 * the library's `DT_NEEDED` entries, so they are read from the linked file
 * instead of being configured per Haxe library: a Haxe library that bundles
 * native code works without the project having to describe it.
 *
 * Only little-endian shared objects are parsed, which covers every target the
 * module builds for (the host, Android and the web). */
class HaxeElf {
public:
	/* `e_machine` values from the ELF specification, named for the Android
	 * ABIs that use them (`abi_to_machine()` maps the ABI string to one of
	 * these). */
	enum Machine {
		MACHINE_NONE = 0,
		MACHINE_X86 = 3,
		MACHINE_ARM = 40,
		MACHINE_X86_64 = 62,
		MACHINE_AARCH64 = 183,
	};

	struct Object {
		int bits = 0; // 32 or 64.
		uint16_t machine = MACHINE_NONE;
		String soname;
		Vector<String> needed;
	};

	/* Parses the program headers and the dynamic section of `p_path`. Returns
	 * OK only when it is an ELF file whose `DT_NEEDED` list could be read. */
	static Error parse(const String &p_path, Object &r_object);

	/* The ELF machine an Android ABI name (`arm64-v8a`, `armeabi-v7a`, `x86`,
	 * `x86_64`) runs on, or `MACHINE_NONE` for an unknown name. */
	static uint16_t abi_to_machine(const String &p_abi);

	/* The ABI name that runs `p_machine`, or an empty string. */
	static String machine_to_abi(uint16_t p_machine);
};
