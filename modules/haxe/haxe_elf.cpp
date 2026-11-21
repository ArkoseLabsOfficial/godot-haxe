/**************************************************************************/
/*  haxe_elf.cpp                                                          */
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

#include "haxe_elf.h"

#include "core/io/file_access.h"

namespace {

/* Little-endian readers. `p_size` is 2, 4 or 8 bytes. */
uint16_t read_u16(const uint8_t *p_data) {
	return (uint16_t)p_data[0] | ((uint16_t)p_data[1] << 8);
}

uint32_t read_u32(const uint8_t *p_data) {
	return (uint32_t)p_data[0] | ((uint32_t)p_data[1] << 8) | ((uint32_t)p_data[2] << 16) | ((uint32_t)p_data[3] << 24);
}

uint64_t read_u64(const uint8_t *p_data) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; i--) {
		value = (value << 8) | p_data[i];
	}
	return value;
}

/* Reads `p_size` bytes of a little-endian integer, so the caller does not have
 * to branch on the field width. */
uint64_t read_word(const uint8_t *p_data, int p_size) {
	uint64_t value = 0;
	for (int i = p_size - 1; i >= 0; i--) {
		value = (value << 8) | p_data[i];
	}
	return value;
}

/* The dynamic string table entry `p_offset` names, or an empty string when the
 * offset runs past the table (a corrupt or truncated file). */
String string_at(const PackedByteArray &p_table, uint64_t p_offset) {
	if (p_offset >= (uint64_t)p_table.size()) {
		return String();
	}
	const uint8_t *bytes = p_table.ptr();
	uint64_t end = p_offset;
	while (end < (uint64_t)p_table.size() && bytes[end] != 0) {
		end++;
	}
	return String::utf8((const char *)bytes + p_offset, (int)(end - p_offset));
}

const int ELF32_HEADER_SIZE = 52;
const int ELF64_HEADER_SIZE = 64;
const uint32_t PT_LOAD = 1;
const uint32_t PT_DYNAMIC = 2;
const uint64_t DT_NULL = 0;
const uint64_t DT_NEEDED = 1;
const uint64_t DT_STRTAB = 5;
const uint64_t DT_STRSZ = 10;
const uint64_t DT_SONAME = 14;

} // namespace

Error HaxeElf::parse(const String &p_path, Object &r_object) {
	r_object = Object();

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return ERR_CANT_OPEN;
	}
	if (file->get_length() < ELF32_HEADER_SIZE) {
		return ERR_FILE_CORRUPT;
	}

	PackedByteArray ident = file->get_buffer(16);
	if (ident.size() != 16 || ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') {
		return ERR_FILE_UNRECOGNIZED;
	}
	const int elf_class = ident[4];
	const int elf_data = ident[5];
	if (elf_data != 1) {
		// Big-endian shared objects exist only on targets the module does build
		// for; refusing them is clearer than parsing them wrong.
		return ERR_FILE_UNRECOGNIZED;
	}
	if (elf_class != 1 && elf_class != 2) {
		return ERR_FILE_UNRECOGNIZED;
	}
	const bool is_64 = elf_class == 2;

	// The header is read from the start of the file, so the position has to be
	// reset after reading the identifier.
	file->seek(0);
	PackedByteArray header = file->get_buffer(is_64 ? ELF64_HEADER_SIZE : ELF32_HEADER_SIZE);
	if (header.size() != (is_64 ? ELF64_HEADER_SIZE : ELF32_HEADER_SIZE)) {
		return ERR_FILE_CORRUPT;
	}
	const uint8_t *h = header.ptr();

	r_object.bits = is_64 ? 64 : 32;
	r_object.machine = read_u16(h + 0x12);

	const uint64_t program_offset = is_64 ? read_u64(h + 0x20) : read_u32(h + 0x1c);
	const uint32_t program_entry_size = is_64 ? read_u16(h + 0x36) : read_u16(h + 0x2a);
	const uint32_t program_count = is_64 ? read_u16(h + 0x38) : read_u16(h + 0x2c);
	if (program_entry_size == 0 || program_count == 0) {
		return ERR_FILE_CORRUPT;
	}
	if (program_offset > (uint64_t)file->get_length() ||
			(uint64_t)program_entry_size * program_count > (uint64_t)file->get_length() - program_offset) {
		return ERR_FILE_CORRUPT;
	}

	file->seek((int64_t)program_offset);
	PackedByteArray programs = file->get_buffer(program_entry_size * program_count);
	if (programs.size() != (int)(program_entry_size * program_count)) {
		return ERR_FILE_CORRUPT;
	}

	/* The dynamic section is read through a program header, and `DT_STRTAB`
	 * holds a virtual address, so the load segments are kept to turn it into a
	 * file offset. Reading program headers instead of section headers keeps
	 * this working on a stripped shared object, which is what an Android
	 * library usually is. */
	struct LoadSegment {
		uint64_t vaddr = 0;
		uint64_t offset = 0;
		uint64_t filesz = 0;
	};
	Vector<LoadSegment> loads;
	uint64_t dynamic_offset = 0;
	uint64_t dynamic_size = 0;
	bool has_dynamic = false;

	for (uint32_t i = 0; i < program_count; i++) {
		const uint8_t *entry = programs.ptr() + (uint64_t)i * program_entry_size;
		const uint32_t type = read_u32(entry);
		if (is_64) {
			const uint64_t offset = read_u64(entry + 0x08);
			const uint64_t vaddr = read_u64(entry + 0x10);
			const uint64_t filesz = read_u64(entry + 0x20);
			if (type == PT_LOAD) {
				loads.push_back({ vaddr, offset, filesz });
			} else if (type == PT_DYNAMIC) {
				dynamic_offset = offset;
				dynamic_size = filesz;
				has_dynamic = true;
			}
		} else {
			const uint32_t offset = read_u32(entry + 0x04);
			const uint32_t vaddr = read_u32(entry + 0x08);
			const uint32_t filesz = read_u32(entry + 0x10);
			if (type == PT_LOAD) {
				loads.push_back({ vaddr, offset, filesz });
			} else if (type == PT_DYNAMIC) {
				dynamic_offset = offset;
				dynamic_size = filesz;
				has_dynamic = true;
			}
		}
	}
	if (!has_dynamic || dynamic_size == 0) {
		return ERR_FILE_CORRUPT;
	}
	if (dynamic_offset > (uint64_t)file->get_length() ||
			(dynamic_size > (uint64_t)file->get_length() - dynamic_offset)) {
		return ERR_FILE_CORRUPT;
	}

	file->seek((int64_t)dynamic_offset);
	PackedByteArray dynamic = file->get_buffer((int)dynamic_size);
	if (dynamic.size() != (int)dynamic_size) {
		return ERR_FILE_CORRUPT;
	}

	const int entry_size = is_64 ? 16 : 8;
	const int tag_size = is_64 ? 8 : 4;
	uint64_t string_table_vaddr = 0;
	bool has_string_table = false;
	uint64_t string_table_size = 0;
	Vector<uint64_t> needed_offsets;
	uint64_t soname_offset = 0;
	bool has_soname = false;

	for (int offset = 0; offset + entry_size <= dynamic.size(); offset += entry_size) {
		const uint8_t *entry = dynamic.ptr() + offset;
		const uint64_t tag = read_word(entry, tag_size);
		const uint64_t value = read_word(entry + tag_size, tag_size);
		if (tag == DT_NULL) {
			break;
		}
		switch (tag) {
			case DT_NEEDED:
				needed_offsets.push_back(value);
				break;
			case DT_STRTAB:
				string_table_vaddr = value;
				has_string_table = true;
				break;
			case DT_STRSZ:
				string_table_size = value;
				break;
			case DT_SONAME:
				soname_offset = value;
				has_soname = true;
				break;
			default:
				break;
		}
	}
	if (!has_string_table) {
		return ERR_FILE_CORRUPT;
	}

	/* `DT_STRTAB` is an address, not an offset: find the load segment that
	 * contains it and translate it. A zero address means the table address is
	 * already a file offset (position independent files that were not mapped
	 * at a base address), which some linkers emit. */
	uint64_t string_table_offset = string_table_vaddr;
	for (const LoadSegment &load : loads) {
		if (string_table_vaddr >= load.vaddr && string_table_vaddr < load.vaddr + load.filesz) {
			string_table_offset = load.offset + (string_table_vaddr - load.vaddr);
			break;
		}
	}
	if (string_table_offset > (uint64_t)file->get_length()) {
		return ERR_FILE_CORRUPT;
	}
	uint64_t available = (uint64_t)file->get_length() - string_table_offset;
	uint64_t table_size = string_table_size != 0 && string_table_size < available ? string_table_size : available;
	if (table_size == 0) {
		return ERR_FILE_CORRUPT;
	}

	file->seek((int64_t)string_table_offset);
	PackedByteArray string_table = file->get_buffer((int)table_size);
	if (string_table.size() != (int)table_size) {
		return ERR_FILE_CORRUPT;
	}

	for (const uint64_t needed : needed_offsets) {
		const String name = string_at(string_table, needed);
		if (!name.is_empty()) {
			r_object.needed.push_back(name);
		}
	}
	if (has_soname) {
		r_object.soname = string_at(string_table, soname_offset);
	}
	return OK;
}

uint16_t HaxeElf::abi_to_machine(const String &p_abi) {
	if (p_abi == "arm64-v8a") {
		return MACHINE_AARCH64;
	}
	if (p_abi == "armeabi-v7a") {
		return MACHINE_ARM;
	}
	if (p_abi == "x86") {
		return MACHINE_X86;
	}
	if (p_abi == "x86_64") {
		return MACHINE_X86_64;
	}
	return MACHINE_NONE;
}

String HaxeElf::machine_to_abi(uint16_t p_machine) {
	switch (p_machine) {
		case MACHINE_AARCH64:
			return "arm64-v8a";
		case MACHINE_ARM:
			return "armeabi-v7a";
		case MACHINE_X86:
			return "x86";
		case MACHINE_X86_64:
			return "x86_64";
		default:
			return String();
	}
}
