/**************************************************************************/
/*  haxe_highlighter.h                                                    */
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

#include "editor/script/syntax_highlighters.h"

/* Syntax highlighting for `.hx` files in the script editor.
 *
 * The grammar is Haxe's, not GDScript's: `//` and block comments, both quote
 * styles, `@:metadata`, `#if` conditional compilation and `$` interpolation.
 * Engine and project types come from the same sources the language itself
 * uses (the class database and the project source index), so a regenerated
 * binding set is highlighted without any extra registration.
 */
class HaxeSyntaxHighlighter : public EditorSyntaxHighlighter {
	GDCLASS(HaxeSyntaxHighlighter, EditorSyntaxHighlighter)

private:
	struct ColorRegion {
		enum Type {
			TYPE_NONE,
			TYPE_STRING,
			TYPE_COMMENT,
			TYPE_DOC_COMMENT,
		};

		Type type = TYPE_NONE;
		Color color;
		String start_key;
		String end_key;
		bool line_only = false;
		bool is_string = false;
		bool is_line_comment = false;
	};

	Vector<ColorRegion> color_regions;
	HashMap<int, int> color_region_cache;

	HashMap<StringName, Color> class_names;
	HashMap<StringName, Color> reserved_keywords;
	HashMap<StringName, Color> member_keywords;

	enum Type {
		NONE,
		REGION,
		ANNOTATION,
		SYMBOL,
		NUMBER,
		FUNCTION,
		KEYWORD,
		MEMBER,
		IDENTIFIER,
		TYPE,
	};

	Color font_color;
	Color symbol_color;
	Color function_color;
	Color function_definition_color;
	Color number_color;
	Color member_variable_color;
	Color string_color;
	Color placeholder_color;
	Color annotation_color;
	Color type_color;
	Color comment_color;

	void add_color_region(ColorRegion::Type p_type, const String &p_start_key, const String &p_end_key, const Color &p_color, bool p_line_only = false);
	/* Dedicated coloring for `import`/`using`/`package` lines, whose dotted
	 * path is a type reference rather than member access. */
	Dictionary _highlight_import_line(const String &p_line);
	Dictionary _highlight_directive_line(int p_start);

public:
	virtual void _update_cache() override;
	virtual Dictionary _get_line_syntax_highlighting_impl(int p_line) override;

	virtual String _get_name() const override;
	virtual PackedStringArray _get_supported_languages() const override;

	virtual Ref<EditorSyntaxHighlighter> _create() const override;
};
