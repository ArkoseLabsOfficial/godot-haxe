#!/usr/bin/env python3
"""Generates the Haxe bindings for Godot from the engine's extension_api.json.

The API dump is the authoritative description of every class, method, property,
signal, enum and constant the engine exposes. Generating the Haxe API from it
keeps the bindings synchronized with the engine version: when the engine is
updated, the dump is regenerated with --dump-extension-api and this script runs
again.

Usage:
    python3 generate_bindings.py --api extension_api.json --out dist/std/godot

Nothing in the output directory should be edited by hand.
"""

import argparse
import json
import os
import re
import sys

# Identifiers that cannot be used as Haxe names. Godot only uses a handful of
# them (GDScript.new, some shader node properties). The engine name is always
# emitted as a string literal, so renaming the Haxe identifier is safe.
KEYWORDS = {
    "abstract",
    "break",
    "case",
    "cast",
    "catch",
    "class",
    "continue",
    "default",
    "do",
    "dynamic",
    "else",
    "enum",
    "extends",
    "extern",
    "final",
    "for",
    "function",
    "if",
    "implements",
    "import",
    "in",
    "inline",
    "interface",
    "is",
    "macro",
    "new",
    "null",
    "operator",
    "overload",
    "override",
    "package",
    "private",
    "public",
    "return",
    "static",
    "super",
    "switch",
    "this",
    "throw",
    "true",
    "false",
    "try",
    "typedef",
    "untyped",
    "using",
    "var",
    "while",
}

# Built-in value types that are handled by the Haxe language or mapped to Haxe
# types instead of getting a generated class.
MAPPED_BUILTINS = {
    "Nil",
    "bool",
    "int",
    "float",
    "String",
    "StringName",
    "NodePath",
    "Array",
}

# Members stored directly on the Haxe wrapper. Everything else that a built-in
# type exposes is derived and resolved through the engine.
PRIMARY_MEMBERS = {
    "Vector2": ["x", "y"],
    "Vector2i": ["x", "y"],
    "Vector3": ["x", "y", "z"],
    "Vector3i": ["x", "y", "z"],
    "Vector4": ["x", "y", "z", "w"],
    "Vector4i": ["x", "y", "z", "w"],
    "Rect2": ["position", "size"],
    "Rect2i": ["position", "size"],
    "Transform2D": ["x", "y", "origin"],
    "Plane": ["x", "y", "z", "d"],
    "Quaternion": ["x", "y", "z", "w"],
    "AABB": ["position", "size"],
    "Basis": ["x", "y", "z"],
    "Transform3D": ["basis", "origin"],
    "Projection": ["x", "y", "z", "w"],
    "Color": ["r", "g", "b", "a"],
}

MEMBER_TYPES = {
    "Vector2": {"x": "Float", "y": "Float"},
    "Vector2i": {"x": "Int", "y": "Int"},
    "Vector3": {"x": "Float", "y": "Float", "z": "Float"},
    "Vector3i": {"x": "Int", "y": "Int", "z": "Int"},
    "Vector4": {"x": "Float", "y": "Float", "z": "Float", "w": "Float"},
    "Vector4i": {"x": "Int", "y": "Int", "z": "Int", "w": "Int"},
    "Rect2": {"position": "godot.Vector2", "size": "godot.Vector2"},
    "Rect2i": {"position": "godot.Vector2i", "size": "godot.Vector2i"},
    "Transform2D": {"x": "godot.Vector2", "y": "godot.Vector2", "origin": "godot.Vector2"},
    "Plane": {"x": "Float", "y": "Float", "z": "Float", "d": "Float"},
    "Quaternion": {"x": "Float", "y": "Float", "z": "Float", "w": "Float"},
    "AABB": {"position": "godot.Vector3", "size": "godot.Vector3"},
    "Basis": {"x": "godot.Vector3", "y": "godot.Vector3", "z": "godot.Vector3"},
    "Transform3D": {"basis": "godot.Basis", "origin": "godot.Vector3"},
    "Projection": {"x": "godot.Vector4", "y": "godot.Vector4", "z": "godot.Vector4", "w": "godot.Vector4"},
    "Color": {"r": "Float", "g": "Float", "b": "Float", "a": "Float"},
}

PACKED_ARRAYS = (
    "PackedByteArray",
    "PackedInt32Array",
    "PackedInt64Array",
    "PackedFloat32Array",
    "PackedFloat64Array",
    "PackedStringArray",
    "PackedVector2Array",
    "PackedVector3Array",
    "PackedColorArray",
    "PackedVector4Array",
)

# Constructors. extension_api.json does not describe them, so the argument
# lists follow the documented GDScript constructors.
CONSTRUCTORS = {
    "Vector2": ("x:Float = 0.0, y:Float = 0.0", ["this.x = x;", "this.y = y;"]),
    "Vector2i": ("x:Int = 0, y:Int = 0", ["this.x = x;", "this.y = y;"]),
    "Vector3": ("x:Float = 0.0, y:Float = 0.0, z:Float = 0.0", ["this.x = x;", "this.y = y;", "this.z = z;"]),
    "Vector3i": ("x:Int = 0, y:Int = 0, z:Int = 0", ["this.x = x;", "this.y = y;", "this.z = z;"]),
    "Vector4": (
        "x:Float = 0.0, y:Float = 0.0, z:Float = 0.0, w:Float = 0.0",
        ["this.x = x;", "this.y = y;", "this.z = z;", "this.w = w;"],
    ),
    "Vector4i": (
        "x:Int = 0, y:Int = 0, z:Int = 0, w:Int = 0",
        ["this.x = x;", "this.y = y;", "this.z = z;", "this.w = w;"],
    ),
    "Rect2": (
        "x:Float = 0.0, y:Float = 0.0, width:Float = 0.0, height:Float = 0.0",
        ["this.position = new godot.Vector2(x, y);", "this.size = new godot.Vector2(width, height);"],
    ),
    "Rect2i": (
        "x:Int = 0, y:Int = 0, width:Int = 0, height:Int = 0",
        ["this.position = new godot.Vector2i(x, y);", "this.size = new godot.Vector2i(width, height);"],
    ),
    "Plane": (
        "a:Float = 0.0, b:Float = 0.0, c:Float = 0.0, d:Float = 0.0",
        ["this.x = a;", "this.y = b;", "this.z = c;", "this.d = d;"],
    ),
    "Quaternion": (
        "x:Float = 0.0, y:Float = 0.0, z:Float = 0.0, w:Float = 1.0",
        ["this.x = x;", "this.y = y;", "this.z = z;", "this.w = w;"],
    ),
    "AABB": (
        "x:Float = 0.0, y:Float = 0.0, z:Float = 0.0, width:Float = 0.0, height:Float = 0.0, depth:Float = 0.0",
        ["this.position = new godot.Vector3(x, y, z);", "this.size = new godot.Vector3(width, height, depth);"],
    ),
    "Basis": (
        ", ".join("m%d:Float = %s" % (i, "1.0" if i in (0, 4, 8) else "0.0") for i in range(9)),
        [
            "this.x = new godot.Vector3(m0, m1, m2);",
            "this.y = new godot.Vector3(m3, m4, m5);",
            "this.z = new godot.Vector3(m6, m7, m8);",
        ],
    ),
    "Transform2D": (
        ", ".join("m%d:Float = %s" % (i, "1.0" if i in (0, 3) else "0.0") for i in range(6)),
        [
            "this.x = new godot.Vector2(m0, m1);",
            "this.y = new godot.Vector2(m2, m3);",
            "this.origin = new godot.Vector2(m4, m5);",
        ],
    ),
    "Transform3D": (
        "basis:godot.Basis = null, origin:godot.Vector3 = null",
        [
            "this.basis = basis == null ? new godot.Basis() : basis;",
            "this.origin = origin == null ? new godot.Vector3() : origin;",
        ],
    ),
    "Projection": (
        "x:godot.Vector4 = null, y:godot.Vector4 = null, z:godot.Vector4 = null, w:godot.Vector4 = null",
        [
            "this.x = x == null ? new godot.Vector4(1, 0, 0, 0) : x;",
            "this.y = y == null ? new godot.Vector4(0, 1, 0, 0) : y;",
            "this.z = z == null ? new godot.Vector4(0, 0, 1, 0) : z;",
            "this.w = w == null ? new godot.Vector4(0, 0, 0, 1) : w;",
        ],
    ),
    "Color": (
        "r:Float = 0.0, g:Float = 0.0, b:Float = 0.0, a:Float = 1.0",
        ["this.r = r;", "this.g = g;", "this.b = b;", "this.a = a;"],
    ),
    "Callable": ("", ["__initFields();"]),
    "Signal": ("", ["__initFields();"]),
    "Dictionary": ("", ["__initFields();"]),
    "RID": ("id:Int = 0", ["this.id = id;"]),
}
for _packed in PACKED_ARRAYS:
    CONSTRUCTORS[_packed] = ("", ["__initFields();"])

# Extra fields that carry the state of non-math built-in values.
SPECIAL_FIELDS = {
    "Callable": [
        "public var kind:Int = 0;",
        'public var objectId:String = "";',
        'public var method:String = "";',
        "public var handle:Int = 0;",
        "public var closureId:Int = 0;",
    ],
    "Signal": [
        'public var objectId:String = "";',
        'public var name:String = "";',
    ],
    "Dictionary": [
        "public var __keys:Array<Dynamic> = [];",
        "public var __values:Array<Dynamic> = [];",
        "public var __readOnly:Bool = false;",
    ],
    "RID": ["public var id:Int = 0;"],
}
for _packed in PACKED_ARRAYS:
    SPECIAL_FIELDS[_packed] = ["public var __data:Array<Dynamic> = [];"]

SPECIAL_BODY = {
    "Object": """
	/**
	 * Stores a value type in one of this object's properties.
	 *
	 * Generated property getters return value types (`Vector2`, `Transform2D`,
	 * ...) bound to this object; writing a component of such a value comes
	 * back here, so `node.position.x = 5` behaves like GDScript and updates
	 * the node instead of a temporary copy. See `hxgodot.ValueOwner`.
	 */
	@:noCompletion public function __hxwrite(property:String, value:Dynamic):Void {
		set(property, value);
	}
""",
    "Dictionary": """
	public static function fromPairs(keys:Array<Dynamic>, values:Array<Dynamic>):godot.Dictionary {
		var dictionary = new godot.Dictionary();
		dictionary.__keys = keys == null ? [] : keys.copy();
		dictionary.__values = values == null ? [] : values.copy();
		return dictionary;
	}

	/** Wraps a Haxe map: `Dictionary.fromMap(["a" => 1])`. */
	public static function fromMap(map:Map<Dynamic, Dynamic>):godot.Dictionary {
		var dictionary = new godot.Dictionary();
		if (map == null) return dictionary;
		for (key in map.keys()) {
			dictionary.__keys.push(key);
			dictionary.__values.push(map.get(key));
		}
		return dictionary;
	}

	@:noCompletion inline function __ensure():Void {
		if (__keys == null) __keys = [];
		if (__values == null) __values = [];
	}

	@:noCompletion inline function __initFields():Void {
		__keys = [];
		__values = [];
		__readOnly = false;
	}
""",
    "Callable": """
	public static function fromObject(object:hxgodot.NativeObject, method:String):godot.Callable {
		var callable = new godot.Callable();
		callable.kind = 0;
		callable.objectId = object == null ? "" : object.__hxgodotId;
		callable.method = method;
		return callable;
	}

	/**
	 * Creates a Callable backed by a Haxe function. The function must accept
	 * as many arguments as the engine passes (signal handlers usually none or
	 * one) and may return a value when the engine expects one.
	 */
	public static function fromFunction(fn:Dynamic):godot.Callable {
		var callable = new godot.Callable();
		callable.kind = 2;
		callable.closureId = hxgodot.Native.storeClosure(fn);
		hxgodot.Native.attachClosureFinalizer(callable, callable.closureId);
		return callable;
	}

	@:noCompletion inline function __initFields():Void {
		kind = 0;
		objectId = "";
		method = "";
		handle = 0;
		closureId = 0;
	}
""",
    "Signal": """
	public static function fromObject(object:hxgodot.NativeObject, signal:String):godot.Signal {
		var result = new godot.Signal();
		result.objectId = object == null ? "" : object.__hxgodotId;
		result.name = signal;
		return result;
	}

	@:noCompletion inline function __initFields():Void {
		objectId = "";
		name = "";
	}
""",
    "RID": """
	public static function fromId(id:Int):godot.RID {
		return new godot.RID(id);
	}
""",
}
for _packed in PACKED_ARRAYS:
    SPECIAL_BODY[_packed] = """
	public static function fromArray(values:Array<Dynamic>):godot.%s {
		var packed = new godot.%s();
		packed.__data = values == null ? [] : values.copy();
		return packed;
	}

	/** Plain copy of the elements, for use with Haxe loops and functions. */
	public function toArray():Array<Dynamic> {
		__ensure();
		return __data.copy();
	}

	@:noCompletion inline function __ensure():Void {
		if (__data == null) __data = [];
	}

	@:noCompletion inline function __initFields():Void {
		__data = [];
	}
""" % (_packed, _packed)


def _packed_mutators(name):
    def body(text):
        return text % {"name": name}

    return {
        "clear": body("""public function clear():Void {
		__ensure();
		__data = [];
	}"""),
        "size": body("""public function size():Int {
		__ensure();
		return __data.length;
	}"""),
        "is_empty": body("""public function is_empty():Bool {
		__ensure();
		return __data.length == 0;
	}"""),
        "get": body("""public function get(index:Int):Dynamic {
		__ensure();
		return __data[index];
	}"""),
        "set": body("""public function set(index:Int, value:Dynamic):Void {
		__ensure();
		__data[index] = value;
	}"""),
        "push_back": body("""public function push_back(value:Dynamic):Bool {
		__ensure();
		__data.push(value);
		return true;
	}"""),
        "append": body("""public function append(value:Dynamic):Bool {
		__ensure();
		__data.push(value);
		return true;
	}"""),
        "append_array": body("""public function append_array(array:Dynamic):Void {
		__ensure();
		var other:Array<Dynamic> = hxgodot.VariantOps.toArray(array);
		if (other == null) return;
		for (value in other) __data.push(value);
	}"""),
        "remove_at": body("""public function remove_at(index:Int):Void {
		__ensure();
		__data.splice(index, 1);
	}"""),
        "insert": body("""public function insert(index:Int, value:Dynamic):Bool {
		__ensure();
		__data.insert(index, value);
		return true;
	}"""),
        "fill": body("""public function fill(value:Dynamic):Void {
		__ensure();
		for (i in 0...__data.length) __data[i] = value;
	}"""),
        "resize": body("""public function resize(size:Int):Int {
		__ensure();
		while (__data.length > size) __data.pop();
		while (__data.length < size) __data.push(null);
		return 0;
	}"""),
        "reverse": body("""public function reverse():Void {
		__ensure();
		__data.reverse();
	}"""),
        "sort": body("""public function sort():Void {
		__ensure();
		__data.sort(function(a, b) return hxgodot.VariantOps.compare(a, b));
	}"""),
        "erase": body("""public function erase(value:Dynamic):Bool {
		__ensure();
		for (i in 0...__data.length) {
			if (hxgodot.VariantOps.sameKey(__data[i], value)) {
				__data.splice(i, 1);
				return true;
			}
		}
		return false;
	}"""),
        "has": body("""public function has(value:Dynamic):Bool {
		__ensure();
		for (item in __data) if (hxgodot.VariantOps.sameKey(item, value)) return true;
		return false;
	}"""),
        "count": body("""public function count(value:Dynamic):Int {
		__ensure();
		var total = 0;
		for (item in __data) if (hxgodot.VariantOps.sameKey(item, value)) total++;
		return total;
	}"""),
        "find": body("""public function find(value:Dynamic, from:Int = 0):Int {
		__ensure();
		var start = from < 0 ? 0 : from;
		for (i in start...__data.length) if (hxgodot.VariantOps.sameKey(__data[i], value)) return i;
		return -1;
	}"""),
        "rfind": body("""public function rfind(value:Dynamic, from:Int = -1):Int {
		__ensure();
		var i = from < 0 || from >= __data.length ? __data.length - 1 : from;
		while (i >= 0) {
			if (hxgodot.VariantOps.sameKey(__data[i], value)) return i;
			i--;
		}
		return -1;
	}"""),
        "slice": body("""public function slice(begin:Int, end_:Int = 2147483647):godot.%(name)s {
		__ensure();
		var result = new godot.%(name)s();
		var last = end_ < 0 ? __data.length + end_ : end_;
		if (last > __data.length) last = __data.length;
		var first = begin < 0 ? __data.length + begin : begin;
		if (first < 0) first = 0;
		if (first > last) first = last;
		result.__data = __data.slice(first, last);
		return result;
	}"""),
        "duplicate": body("""public function duplicate():godot.%(name)s {
		__ensure();
		var copy = new godot.%(name)s();
		copy.__data = __data.copy();
		return copy;
	}"""),
    }


# Methods that mutate a value in place. They cannot be forwarded to the engine
# because the engine would mutate a temporary copy; the Haxe representation is
# the source of truth for these.
MUTATORS = {
    "Dictionary": {
        "clear": """public function clear():Void {
		__ensure();
		if (__readOnly) return;
		__keys = [];
		__values = [];
	}""",
        "merge": """public function merge(dictionary:godot.Dictionary, overwrite:Bool = false):Void {
		__ensure();
		if (__readOnly || dictionary == null) return;
		var otherKeys:Array<Dynamic> = dictionary.keys();
		var otherValues:Array<Dynamic> = dictionary.values();
		for (i in 0...otherKeys.length) set(otherKeys[i], otherValues[i], overwrite);
	}""",
        "set": """public function set(key:Dynamic, value:Dynamic, overwrite:Bool = true):Bool {
		__ensure();
		if (__readOnly) return false;
		for (i in 0...__keys.length) {
			if (hxgodot.VariantOps.sameKey(__keys[i], key)) {
				if (!overwrite) return false;
				__values[i] = value;
				return true;
			}
		}
		__keys.push(key);
		__values.push(value);
		return true;
	}""",
        "erase": """public function erase(key:Dynamic):Bool {
		__ensure();
		if (__readOnly) return false;
		for (i in 0...__keys.length) {
			if (hxgodot.VariantOps.sameKey(__keys[i], key)) {
				__keys.splice(i, 1);
				__values.splice(i, 1);
				return true;
			}
		}
		return false;
	}""",
        "get_or_add": """public function get_or_add(key:Dynamic, defaultValue:Dynamic = null):Dynamic {
		__ensure();
		for (i in 0...__keys.length) {
			if (hxgodot.VariantOps.sameKey(__keys[i], key)) return __values[i];
		}
		__keys.push(key);
		__values.push(defaultValue);
		return defaultValue;
	}""",
        "assign": """public function assign(dictionary:godot.Dictionary):Void {
		__ensure();
		if (__readOnly || dictionary == null) return;
		__keys = dictionary.keys();
		__values = dictionary.values();
	}""",
        "make_read_only": """public function make_read_only():Void {
		__readOnly = true;
	}""",
        "size": """public function size():Int {
		__ensure();
		return __keys.length;
	}""",
        "is_empty": """public function is_empty():Bool {
		__ensure();
		return __keys.length == 0;
	}""",
        "is_read_only": """public function is_read_only():Bool {
		return __readOnly == true;
	}""",
        "keys": """public function keys():Array<Dynamic> {
		__ensure();
		return __keys.copy();
	}""",
        "values": """public function values():Array<Dynamic> {
		__ensure();
		return __values.copy();
	}""",
        "has": """public function has(key:Dynamic):Bool {
		__ensure();
		for (item in __keys) if (hxgodot.VariantOps.sameKey(item, key)) return true;
		return false;
	}""",
        "has_all": """public function has_all(keys:Array<Dynamic>):Bool {
		if (keys == null) return true;
		for (key in keys) if (!has(key)) return false;
		return true;
	}""",
        "find_key": """public function find_key(value:Dynamic):Dynamic {
		__ensure();
		for (i in 0...__values.length) {
			if (hxgodot.VariantOps.sameKey(__values[i], value)) return __keys[i];
		}
		return null;
	}""",
        "get": """public function get(key:Dynamic, defaultValue:Dynamic = null):Dynamic {
		__ensure();
		for (i in 0...__keys.length) {
			if (hxgodot.VariantOps.sameKey(__keys[i], key)) return __values[i];
		}
		return defaultValue;
	}""",
        "merged": """public function merged(dictionary:godot.Dictionary, overwrite:Bool = false):godot.Dictionary {
		var copy = duplicate();
		if (dictionary != null) copy.merge(dictionary, overwrite);
		return copy;
	}""",
        "sort": """public function sort():Void {
		__ensure();
		if (__readOnly) return;
		var count = __keys.length;
		if (count < 2) return;
		var indices:Array<Int> = [for (i in 0...count) i];
		indices.sort(function(a, b) return hxgodot.VariantOps.compare(__keys[a], __keys[b]));
		var sortedKeys:Array<Dynamic> = [];
		var sortedValues:Array<Dynamic> = [];
		for (index in indices) {
			sortedKeys.push(__keys[index]);
			sortedValues.push(__values[index]);
		}
		__keys = sortedKeys;
		__values = sortedValues;
	}""",
        "duplicate": """public function duplicate(deep:Bool = false):godot.Dictionary {
		__ensure();
		var copy = new godot.Dictionary();
		copy.__keys = __keys.copy();
		copy.__values = __values.copy();
		return copy;
	}""",
    },
}
for _packed in PACKED_ARRAYS:
    MUTATORS[_packed] = _packed_mutators(_packed)

# Static helpers for values whose constructor is not the natural way to build
# them.
EXTRA_STATICS = {
    "Transform3D": """
	public static function fromFloats(xx:Float, xy:Float, xz:Float, yx:Float, yy:Float, yz:Float,
			zx:Float, zy:Float, zz:Float, ox:Float, oy:Float, oz:Float):godot.Transform3D {
		return new godot.Transform3D(new godot.Basis(xx, xy, xz, yx, yy, yz, zx, zy, zz),
				new godot.Vector3(ox, oy, oz));
	}
""",
    "Projection": """
	public static function fromFloats(xx:Float, xy:Float, xz:Float, xw:Float, yx:Float, yy:Float,
			yz:Float, yw:Float, zx:Float, zy:Float, zz:Float, zw:Float, wx:Float, wy:Float,
			wz:Float, ww:Float):godot.Projection {
		return new godot.Projection(new godot.Vector4(xx, xy, xz, xw), new godot.Vector4(yx, yy, yz, yw),
				new godot.Vector4(zx, zy, zz, zw), new godot.Vector4(wx, wy, wz, ww));
	}
""",
}

HEADER = """// Generated by modules/haxe/dist/tools/generate_bindings.py from the engine's
// extension_api.json for %s.
// Do not edit: run the generator instead.
"""


def ident(name):
    return name + "_" if name in KEYWORDS else name


def split_args(text):
    """Splits "1, 0, 0" respecting nesting and string literals."""
    result = []
    depth = 0
    quoted = False
    current = ""
    for ch in text:
        if quoted:
            current += ch
            if ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
            current += ch
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            result.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        result.append(current.strip())
    return result


def int_value(text):
    """Signed 32-bit value of an engine integer.

    Haxe's `Int` is 32 bits wide on every target, while the API dump describes
    some values as 64 bits (`RenderingServer.ARRAY_FLAG_FORMAT_VERSION_2` is
    `8 << 35`). Such values are truncated to their low 32 bits and read back as
    signed, which is the value the runtime would end up with anyway.
    """
    number = int(text, 0) & 0xFFFFFFFF
    if number > 0x7FFFFFFF:
        number -= 0x100000000
    return number


def int_literal(text):
    return str(int_value(text))


def int_fits(text):
    """Whether the engine integer survives the conversion to a Haxe `Int`."""
    return int(text, 0) == int_value(text)


def scalar_literal(text):
    """Haxe expression for a literal value, or None when it is not literal."""
    text = text.strip()
    if text in ("true", "false"):
        return text
    if text in ("inf", "+inf"):
        return "Math.POSITIVE_INFINITY"
    if text == "-inf":
        return "Math.NEGATIVE_INFINITY"
    if text in ("nan", "-nan"):
        return "Math.NaN"
    if re.fullmatch(r"-?\d+", text):
        return int_literal(text)
    if re.fullmatch(r"0x[0-9a-fA-F]+", text):
        return int_literal(text)
    if re.fullmatch(r"-?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?", text):
        return text
    if text.startswith('&"') and text.endswith('"') and len(text) >= 3:
        return text[1:]
    if text.startswith('"') and text.endswith('"') and len(text) >= 2:
        return text
    return None


# Godot 4.7 encodes the element type of a typed container inside the type
# string: `typedarray::24/17:Font` is Array[Font] (Variant::OBJECT with
# PROPERTY_HINT_RESOURCE_TYPE) and `typedarray::27/0:` is Array[Dictionary].
VARIANT_TYPE_NAMES = {
    0: "Nil",
    1: "bool",
    2: "int",
    3: "float",
    4: "String",
    5: "Vector2",
    6: "Vector2i",
    7: "Rect2",
    8: "Rect2i",
    9: "Vector3",
    10: "Vector3i",
    11: "Transform2D",
    12: "Vector4",
    13: "Vector4i",
    14: "Plane",
    15: "Quaternion",
    16: "AABB",
    17: "Basis",
    18: "Transform3D",
    19: "Projection",
    20: "Color",
    21: "StringName",
    22: "NodePath",
    23: "RID",
    24: "Object",
    25: "Callable",
    26: "Signal",
    27: "Dictionary",
    28: "Array",
    29: "PackedByteArray",
    30: "PackedInt32Array",
    31: "PackedInt64Array",
    32: "PackedFloat32Array",
    33: "PackedFloat64Array",
    34: "PackedStringArray",
    35: "PackedVector2Array",
    36: "PackedVector3Array",
    37: "PackedColorArray",
    38: "PackedVector4Array",
}


class Generator:
    def __init__(self, api, out_dir):
        self.api = api
        self.out_dir = out_dir
        self.class_names = {c["name"] for c in api["classes"]}
        # Built-in value classes (Vector3, ...) are not in `classes`, but engine
        # methods still use their enums (Vector3.Axis). Those enums are not
        # emitted as Haxe types, so they are represented as Int like the enums
        # of regular classes.
        self.builtin_class_names = {c["name"] for c in api.get("builtin_classes") or []}
        self.version = api["header"]["version_full_name"].replace(".custom_build", "").replace("custom_build", "stable")
        # Built-in values whose representation lives in the Haxe wrapper
        # (`__keys`/`__values` or `__data`). Their non-const methods must copy
        # the engine's updated value back into the wrapper.
        self.stateful = set(["Dictionary"]) | set(PACKED_ARRAYS)
        self.class_enums = {}
        self.classes = {c["name"]: c for c in api["classes"]}
        for cls in api["classes"]:
            for enum in cls.get("enums") or []:
                self.class_enums.setdefault(enum["name"], []).append(cls["name"])
        self._inherited = {}

    # ---- inheritance ---------------------------------------------------

    def ancestors(self, name):
        result = []
        parent = self.classes.get(name, {}).get("inherits")
        while parent:
            result.append(parent)
            parent = self.classes.get(parent, {}).get("inherits")
        return result

    def inherited_members(self, name):
        """Members visible from the ancestors of a class, nearest first.

        Maps a member name to `(return_type, argument_count)` for engine
        methods and to `("accessor", "get"|"set", property_type)` for the
        accessors of ancestor properties, which are methods in Haxe.
        """
        if name in self._inherited:
            return self._inherited[name]
        result = {}
        for ancestor in self.ancestors(name):
            cls = self.classes.get(ancestor) or {}
            for prop in cls.get("properties") or []:
                for direction in ("get", "set"):
                    if not prop.get("getter" if direction == "get" else "setter"):
                        continue
                    member = direction + "_" + ident(prop["name"])
                    result.setdefault(member, ("accessor", direction, prop["type"]))
            for method in cls.get("methods") or []:
                return_type = method.get("return_type")
                if return_type is None:
                    return_type = (method.get("return_value") or {}).get("type", "void")
                result.setdefault(method["name"], (return_type, len(method.get("arguments") or [])))
        self._inherited[name] = result
        return result

    def inherited_properties(self, name):
        result = set()
        for ancestor in self.ancestors(name):
            cls = self.classes.get(ancestor) or {}
            for prop in cls.get("properties") or []:
                result.add(prop["name"])
        return result

    def accessor_overrides(self, name, prop):
        """Plans how a property relates to same-named inherited members.

        Returns `(override_get, override_set, conflict)`. A getter overrides an
        inherited method when that method takes no arguments and returns the
        property type; a setter needs one argument and must return the property
        type (Haxe requires setters to return the value). `conflict` is set when
        an inherited member cannot be overridden, in which case the property
        cannot be declared at all: Haxe has no way to name an accessor
        differently since 4.3 removed custom accessor names.
        """
        inherited = self.inherited_members(name)
        prop_type = self.type_name(prop["type"])
        overrides = {"get": False, "set": False}
        for direction in ("get", "set"):
            if not prop.get("getter" if direction == "get" else "setter"):
                continue
            member = direction + "_" + ident(prop["name"])
            if member not in inherited:
                continue
            signature = inherited[member]
            if signature[0] == "accessor":
                # Same accessor in an ancestor: the property itself would be a
                # redefinition, which Haxe rejects inside a subclass.
                return overrides["get"], overrides["set"], (member, "property redefinition")
            return_type, argument_count = signature
            expected = 0 if direction == "get" else 1
            if argument_count != expected:
                return overrides["get"], overrides["set"], (member, "different arity")
            if return_type == "void":
                return overrides["get"], overrides["set"], (member, "returns void")
            if not self.same_haxe_type(return_type, prop_type):
                return overrides["get"], overrides["set"], (member, "different type " + return_type)
            overrides[direction] = True
        return overrides["get"], overrides["set"], None

    def same_haxe_type(self, engine_type, haxe_type):
        """True when an engine type maps to the given Haxe type.

        Enum abstracts have `Int` as their underlying type (`from Int to Int`),
        so overriding an `Int` property whose getter returns a bitfield enum
        stays type-compatible in Haxe.
        """
        mapped = self.type_name(engine_type)
        if mapped == haxe_type:
            return True
        if mapped.startswith("godot.GD.") and haxe_type == "Int":
            return True
        return False

    # ---- types ---------------------------------------------------------

    def typed_element(self, name, param):
        name = name.strip()
        if name.startswith("typeddictionary::"):
            return "godot.Dictionary"
        if ":" in name:
            head, _, class_name = name.partition(":")
            number = head.split("/")[0]
            if number.isdigit():
                kind = VARIANT_TYPE_NAMES.get(int(number))
                if kind == "Object":
                    return "godot." + class_name if class_name else "godot.Object"
                if kind is not None:
                    return self.type_name(kind, param)
        return self.type_name(name, param)

    def enum_path(self, name):
        """Haxe type of an engine enum.

        Class enums are represented as Int plus constants on the class, like
        GDScript's `Node.PROCESS_MODE_INHERIT`: unique Haxe types per enum are
        impossible because Haxe forbids two same-named types in one package
        (Vector2.Axis, Vector3.Axis, ...). Global enums do become enum
        abstracts, because their names are unique.
        """
        if "." in name:
            owner, enum = name.split(".", 1)
            if owner in self.class_names or owner in self.builtin_class_names:
                return "Int"
            return "godot.GD.%s_%s" % (owner, enum)
        return "godot.GD.%s" % name

    def type_name(self, name, param=False):
        name = name.strip()
        if not name:
            return "Dynamic"
        if name.startswith("const ") or name.startswith("volatile "):
            return self.type_name(name.split(" ", 1)[1], param)
        if name.startswith("typeddictionary::"):
            return "godot.Dictionary"
        if name.startswith("typedarray::"):
            return "Array<%s>" % self.typed_element(name[len("typedarray::") :], param)
        if name.startswith("enum::"):
            return self.enum_path(name[len("enum::") :])
        if name.startswith("bitfield::"):
            return self.enum_path(name[len("bitfield::") :])
        if "," in name:
            # "BaseMaterial3D,ShaderMaterial" or "Texture2D,-AnimatedTexture".
            # Negative entries are exclusions; the first positive one is the
            # type the engine actually documents.
            for part in name.split(","):
                part = part.strip()
                if part and not part.startswith("-"):
                    return self.type_name(part, param)
            return "Dynamic"
        simple = {
            "void": "Void",
            "bool": "Bool",
            "int": "Int",
            "float": "Float",
            "String": "String",
            "StringName": "String",
            "NodePath": "String",
            "Variant": "Dynamic",
            "Nil": "Dynamic",
            "Array": "Array<Dynamic>",
            "Callable": "Dynamic" if param else "godot.Callable",
        }
        if name in simple:
            return simple[name]
        if name.endswith("*"):
            return "Dynamic"
        return "godot." + name

    def default_expr(self, text, type_name=None):
        """Haxe expression for a default value, or None if not expressible."""
        text = text.strip()
        literal = scalar_literal(text)
        if literal is not None:
            return literal
        if text in ("null", "Nil", "Variant()"):
            return "null"
        if text in ("[]", "{}"):
            return "[]" if text == "[]" else "new godot.Dictionary()"
        if text.startswith("Array["):
            return "[]"
        if text.startswith("&"):
            return self.default_expr(text[1:], type_name)
        match = re.fullmatch(r"([A-Za-z0-9_]+)\((.*)\)", text, re.S)
        if not match:
            return None
        name = match.group(1)
        raw_args = split_args(match.group(2))
        args = [self.default_expr(arg, None) or arg for arg in raw_args]
        if name == "NodePath":
            inner = raw_args[0] if raw_args else '""'
            return inner if inner.startswith('"') else '"%s"' % inner
        if name in ("Transform3D",) and len(args) == 12:
            return "godot.Transform3D.fromFloats(%s)" % ", ".join(args)
        if name in ("Projection",) and len(args) == 16:
            return "godot.Projection.fromFloats(%s)" % ", ".join(args)
        if name in ("Rect2", "Rect2i", "AABB") and len(args) in (4, 6):
            return "new godot.%s(%s)" % (name, ", ".join(args))
        if name in (
            "Vector2",
            "Vector2i",
            "Vector3",
            "Vector3i",
            "Vector4",
            "Vector4i",
            "Quaternion",
            "Color",
            "Basis",
            "Plane",
            "Transform2D",
        ):
            return "new godot.%s(%s)" % (name, ", ".join(args))
        if name in ("RID", "Callable", "Signal", "Dictionary") or name.startswith("Packed"):
            return "new godot.%s(%s)" % (name, ", ".join(args))
        return None

    def param(self, arg, used_names):
        """Returns (declaration, expression, prelude lines)."""
        name = ident(arg["name"])
        while name in used_names:
            name += "_"
        used_names.add(name)
        type_name = self.type_name(arg["type"], param=True)
        if "default_value" not in arg:
            return "%s:%s" % (name, type_name), name, []
        raw = arg["default_value"]
        literal = scalar_literal(raw)
        if literal is not None:
            return "%s:%s = %s" % (name, type_name, literal), name, []
        expr = self.default_expr(raw, type_name)
        if expr is None:
            warn("no Haxe expression for default %r (%s)" % (raw, type_name))
            return "?%s:%s" % (name, type_name), name, []
        if expr == "null":
            return "?%s:%s" % (name, type_name), name, []
        local = "__" + name
        array_type = "Array<Dynamic>" if type_name == "Array<Dynamic>" else type_name
        return (
            "?%s:%s" % (name, type_name),
            local,
            ["var %s:%s = %s == null ? %s : %s;" % (local, array_type, name, expr, name)],
        )

    # ---- members -------------------------------------------------------

    def method(self, owner, method, is_builtin, override=False):
        name = ident(method["name"])
        return_type = method.get("return_type")
        if return_type is None:
            return_type = (method.get("return_value") or {}).get("type", "void")
        return_type = self.type_name(return_type)
        is_static = method.get("is_static", False)
        is_virtual = method.get("is_virtual", False) and not is_builtin
        used = set()
        declarations = []
        expressions = []
        prelude = []
        for arg in method.get("arguments") or []:
            declaration, expression, lines = self.param(arg, used)
            declarations.append(declaration)
            expressions.append(expression)
            prelude.extend(lines)
        if method.get("is_vararg"):
            declarations.append("...__varArgs:Dynamic")
            prelude.append("var __args:Array<Dynamic> = [%s];" % ", ".join(expressions))
            prelude.append("for (__item in __varArgs) __args.push(__item);")
            call_args = "__args"
        else:
            call_args = "[%s]" % ", ".join(expressions)

        body = []
        if is_virtual:
            if return_type != "Void":
                body.append("return %s;" % self.zero_value(return_type))
        elif is_builtin:
            if is_static:
                call = 'Native.variantCallStatic("%s", "%s", %s)' % (owner, method["name"], call_args)
            elif method.get("is_const", True) or owner not in self.stateful:
                call = 'Native.variantCall(this, "%s", %s)' % (method["name"], call_args)
            else:
                # Non-const built-in methods mutate the value they are called
                # on; the updated primary fields are copied back into `this`.
                call = 'Native.variantCallMutating(this, "%s", %s)' % (method["name"], call_args)
            body.append("%s;" % call if return_type == "Void" else "return cast %s;" % call)
        else:
            if is_static:
                call = 'Native.callStatic("%s", "%s", %s)' % (owner, method["name"], call_args)
            else:
                call = 'Native.call(__hxgodotId, "%s", %s)' % (method["name"], call_args)
            body.append("%s;" % call if return_type == "Void" else "return cast %s;" % call)

        modifier = "static " if is_static else "override " if override else ""
        lines = ["\tpublic %sfunction %s(%s):%s {" % (modifier, name, ", ".join(declarations), return_type)]
        for line in prelude:
            lines.append("\t\t" + line)
        for line in body:
            lines.append("\t\t" + line)
        lines.append("\t}")
        return "\n".join(lines)

    def zero_value(self, type_name):
        if type_name in ("Int", "Float"):
            return "0"
        if type_name == "Bool":
            return "false"
        if type_name == "String":
            return '""'
        if type_name.startswith("godot.GD."):
            # Global enums are `enum abstract X(Int)`, i.e. value types on
            # static targets. `null` is not a valid value for them, so the
            # generated virtual stub has to return 0 instead.
            return "0"
        return "null"

    def constant(self, constant):
        name = constant["name"]
        type_name = self.type_name(constant.get("type", "int"))
        if type_name == "Void":
            type_name = "Dynamic"
        value = constant.get("value", 0)
        if isinstance(value, str):
            expression = self.default_expr(value, type_name)
            if expression is None:
                expression = "null"
        elif isinstance(value, int) and type_name == "Int":
            expression = int_literal(str(value))
            if not int_fits(str(value)):
                warn("%s = %s does not fit a 32-bit Haxe Int; using %s" % (name, value, expression))
        else:
            expression = str(value)
        if type_name == "Dynamic":
            expression = "cast %s" % expression
        return "\t@:keep public static var %s:%s = %s;" % (name, type_name, expression)

    def enum_block(self, enum):
        lines = ["@:keep", "enum abstract %s(Int) from Int to Int {" % enum["name"]]
        for value in enum["values"]:
            if not int_fits(str(value["value"])):
                warn(
                    "%s.%s = %s does not fit a 32-bit Haxe Int; using %s"
                    % (enum["name"], value["name"], value["value"], int_literal(str(value["value"])))
                )
                lines.append("\t/* Engine value %s; truncated to 32 bits. */" % value["value"])
            lines.append("\tpublic var %s = %s;" % (ident(value["name"]), int_literal(str(value["value"]))))
        lines.append("}")
        return "\n".join(lines)

    # ---- files ---------------------------------------------------------

    def class_file(self, cls):
        name = cls["name"]
        is_builtin = name in PRIMARY_MEMBERS or name in SPECIAL_FIELDS
        base = cls.get("inherits")
        out = [HEADER % self.version, "package godot;", "", "import hxgodot.Native;", ""]

        # Value types carry the object property they were read from, so that a
        # component write reaches the object (see `hxgodot.ValueOwner`).
        owns_value = name in PRIMARY_MEMBERS
        if is_builtin:
            out.append("@:keep")
            out.append(
                "class %s%s {" % (name, " implements hxgodot.ValueOwner" if owns_value else "")
            )
        elif base:
            out.append("@:keep")
            out.append("class %s extends godot.%s {" % (name, base))
        else:
            out.append("@:keep")
            out.append("class %s extends hxgodot.NativeObject implements hxgodot.ValueOwner {" % name)

        signature, body = (
            CONSTRUCTORS.get(name, ("", []))
            if is_builtin
            else ("?__engineClass:String", ['super(__engineClass == null ? "%s" : __engineClass);' % name])
        )
        out.append("\tpublic function new(%s) {" % signature)
        for statement in body:
            out.append("\t\t" + statement)
        out.append("\t}")

        if cls.get("is_singleton"):
            singleton = cls.get("singleton_name") or name
            out.append("")
            out.append("\t/** The engine singleton instance. */")
            out.append("\tpublic static function instance():godot.%s {" % name)
            out.append('\t\treturn cast Native.singleton("%s");' % singleton)
            out.append("\t}")

        for member in PRIMARY_MEMBERS.get(name, []):
            member_type = MEMBER_TYPES.get(name, {}).get(member, "Float")
            out.append("")
            out.extend(self.value_member(member, member_type))
        for field in SPECIAL_FIELDS.get(name, []):
            out.append("\t@:keep " + field)

        if owns_value:
            out.append("")
            out.append("\t/* Set when this value was read from an object property")
            out.append("\t * (`node.position`). Component writes then store the whole value back. */")
            out.append("\tvar __hxowner:hxgodot.ValueOwner = null;")
            out.append('\tvar __hxproperty:String = null;')
            out.append("")
            out.append(
                "\t@:noCompletion public function __hxbindValue(owner:hxgodot.ValueOwner, property:String):godot.%s {"
                % name
            )
            out.append("\t\t__hxowner = owner;")
            out.append("\t\t__hxproperty = property;")
            out.append("\t\treturn this;")
            out.append("\t}")
            out.append("")
            out.append("\t/** Stores this value in the property it was bound to, if any. */")
            out.append("\t@:noCompletion public function __hxcommit():Void {")
            out.append("\t\tif (__hxowner != null) {")
            out.append("\t\t\t__hxowner.__hxwrite(__hxproperty, this);")
            out.append("\t\t}")
            out.append("\t}")
            out.append("")
            out.append("\t/** Detached copy, used by property getters before they bind it. */")
            out.append("\t@:noCompletion public function __hxcopy():godot.%s {" % name)
            out.append("\t\tvar copy = new godot.%s();" % name)
            for member in PRIMARY_MEMBERS.get(name, []):
                member_type = MEMBER_TYPES.get(name, {}).get(member, "Float")
                if member_type.startswith("godot."):
                    out.append(
                        "\t\tcopy.__%s = __%s == null ? null : __%s.__hxcopy();" % (member, member, member)
                    )
                else:
                    out.append("\t\tcopy.__%s = __%s;" % (member, member))
            out.append("\t\treturn copy;")
            out.append("\t}")
            out.append("")
            out.append("\t@:noCompletion public function __hxwrite(property:String, value:Dynamic):Void {")
            out.append("\t\tswitch (property) {")
            for member in PRIMARY_MEMBERS.get(name, []):
                out.append('\t\t\tcase "%s":' % member)
                out.append("\t\t\t\t%s = cast value;" % ident(member))
            out.append("\t\t}")
            out.append("\t}")

        if name in SPECIAL_BODY:
            out.append(SPECIAL_BODY[name])
        if name in EXTRA_STATICS:
            out.append(EXTRA_STATICS[name])
        if is_builtin:
            # Engine formatting, so `str(vector)` prints `(1, 2)` exactly like
            # GDScript instead of a Haxe class name.
            out.append("")
            out.append("\t/** Text form using the engine's own formatting. */")
            out.append("\tpublic function toString():String {")
            out.append('\t\treturn cast Native.variantCallStatic("Nil", "str", [this]);')
            out.append("\t}")

        if cls.get("constants"):
            out.append("")
            for constant in cls["constants"]:
                out.append(self.constant(constant))

        mutators = MUTATORS.get(name, {})
        members = cls.get("members") or []
        if is_builtin:
            primary = set(PRIMARY_MEMBERS.get(name, []))
            for member in members:
                if member["name"] in primary:
                    continue
                out.append("")
                out.extend(self.member_accessor(member))

        inherited_properties = self.inherited_properties(name)
        inherited_members = self.inherited_members(name)

        for prop in cls.get("properties") or []:
            if prop["name"] in inherited_properties:
                dropped_properties.append((name, prop["name"], "redeclares an inherited property"))
                continue
            override_get, override_set, conflict = self.accessor_overrides(name, prop)
            if conflict is not None:
                member, reason = conflict
                dropped_properties.append((name, prop["name"], "%s (%s)" % (member, reason)))
                continue
            out.append("")
            out.extend(self.property_accessor(prop, override_get, override_set))

        # Haxe requires property accessors to be named get_<property> and
        # set_<property>. When the engine getter/setter has that exact name the
        # accessor *is* that method, so nothing is lost. When it has another
        # name (is_enabled, has_autowrap, ...) the accessor is synthesized and a
        # same-named method with different semantics would collide; the
        # property wins (11 cases in 4.7.2) and the method stays reachable
        # through hxCall().
        method_names = {m["name"] for m in cls.get("methods") or []}
        reserved = set()
        for prop in cls.get("properties") or []:
            for direction, engine_name in (("get_", prop.get("getter")), ("set_", prop.get("setter"))):
                if not engine_name:
                    continue
                accessor_name = direction + ident(prop["name"])
                reserved.add(accessor_name)
                if accessor_name != engine_name and accessor_name in method_names:
                    dropped_methods.append((name, accessor_name, engine_name, prop["name"]))
        # A value type declares its components as properties too, so a method
        # that happens to be named `get_<component>` (`Transform2D.get_origin`)
        # would be the same Haxe function. The property is the component, so
        # the method is dropped; it stays reachable through hxCall().
        for member in PRIMARY_MEMBERS.get(name, []):
            for direction in ("get_", "set_"):
                accessor_name = direction + ident(member)
                reserved.add(accessor_name)
                if accessor_name in method_names:
                    dropped_methods.append((name, accessor_name, member, member))

        for method in cls.get("methods") or []:
            if method["name"] in reserved:
                continue
            if method["name"] in mutators:
                if method["name"] in ("keys", "values") and False:
                    continue
                out.append("")
                out.append("\t" + mutators[method["name"]])
                continue
            out.append("")
            out.append(
                self.method(
                    name,
                    method,
                    is_builtin,
                    override=not method.get("is_static") and method["name"] in inherited_members,
                )
            )

        for signal in cls.get("signals") or []:
            out.append(
                '\t@:keep public static inline var SIGNAL_%s:String = "%s";' % (signal["name"].upper(), signal["name"])
            )

        # Enum values become Int constants on the class, mirroring GDScript.
        for enum in cls.get("enums") or []:
            for value in enum["values"]:
                if not int_fits(str(value["value"])):
                    warn(
                        "%s.%s = %s does not fit a 32-bit Haxe Int; using %s"
                        % (enum["name"], value["name"], value["value"], int_literal(str(value["value"])))
                    )
                    out.append("\t/* Engine value %s; truncated to 32 bits. */" % value["value"])
                out.append(
                    "\t@:keep public static var %s:Int = %s;" % (ident(value["name"]), int_literal(str(value["value"])))
                )

        out.append("}")
        return "\n".join(out) + "\n"

    def value_member(self, member, member_type):
        """Component of a value type (`Vector2.x`, `Rect2.position`, ...).

        The component lives in a backing field and is exposed as a property:
        writing it commits the whole value back to the object it was read from,
        which is what makes `node.position.x = 5` reach the node.
        """
        name = ident(member)
        lines = ["\tvar __%s:%s;" % (name, member_type), ""]
        lines.append("\tpublic var %s(get, set):%s;" % (name, member_type))
        lines.append("")
        lines.append("\t@:noCompletion function get_%s():%s {" % (name, member_type))
        if member_type.startswith("godot."):
            # A nested value is copied as well, so a caller that keeps it does
            # not mutate this value behind its back.
            lines.append(
                '\t\treturn __%s == null ? null : __%s.__hxcopy().__hxbindValue(this, "%s");'
                % (name, name, member)
            )
        else:
            lines.append("\t\treturn __%s;" % name)
        lines.append("\t}")
        lines.append("")
        lines.append("\t@:noCompletion function set_%s(value:%s):%s {" % (name, member_type, member_type))
        lines.append("\t\t__%s = value;" % name)
        lines.append("\t\t__hxcommit();")
        lines.append("\t\treturn value;")
        lines.append("\t}")
        return lines

    def member_accessor(self, member):
        """Derived member of a built-in value (Rect2.end, Color.h, ...)."""
        name = ident(member["name"])
        type_name = self.type_name(member["type"])
        return [
            "\tpublic var %s(get, set):%s;" % (name, type_name),
            "",
            "\t@:noCompletion function get_%s():%s {" % (name, type_name),
            '\t\treturn cast Native.getMember(this, "%s");' % member["name"],
            "\t}",
            "",
            "\t@:noCompletion function set_%s(value:%s):%s {" % (name, type_name, type_name),
            '\t\tNative.setMember(this, "%s", value);' % member["name"],
            "\t\treturn value;",
            "\t}",
        ]

    def property_accessor(self, prop, override_get=False, override_set=False):
        name = ident(prop["name"])
        type_name = self.type_name(prop["type"])
        getter = prop.get("getter")
        setter = prop.get("setter")
        index = prop.get("index")
        # A value type is passed by copy, so a getter that returned the plain
        # value would make `node.position.x = 5` a write to a temporary. The
        # copy is bound to this object and writes the whole value back through
        # the property's setter (see `hxgodot.ValueOwner`).
        bound = getter and setter and prop["type"] in PRIMARY_MEMBERS
        accessors = ["get" if getter else "never", "set" if setter else "never"]
        lines = ["\tpublic var %s(%s):%s;" % (name, ", ".join(accessors), type_name)]
        if getter:
            arguments = "[%d]" % index if index is not None else "[]"
            lines.append(
                "\t@:noCompletion %sfunction get_%s():%s {" % ("override " if override_get else "", name, type_name)
            )
            if bound:
                lines.append(
                    '\t\tvar __value:%s = cast Native.call(__hxgodotId, "%s", %s);'
                    % (type_name, getter, arguments)
                )
                lines.append(
                    '\t\treturn __value == null ? null : __value.__hxbindValue(this, "%s");' % prop["name"]
                )
            else:
                lines.append('\t\treturn cast Native.call(__hxgodotId, "%s", %s);' % (getter, arguments))
            lines.append("\t}")
        if setter:
            arguments = "[%d, value]" % index if index is not None else "[value]"
            lines.append(
                "\t@:noCompletion %sfunction set_%s(value:%s):%s {"
                % ("override " if override_set else "", name, type_name, type_name)
            )
            lines.append('\t\tNative.call(__hxgodotId, "%s", %s);' % (setter, arguments))
            lines.append("\t\treturn value;")
            lines.append("\t}")
        return lines

    def gd_file(self):
        out = [HEADER % self.version, "package godot;", "", "import hxgodot.Native;", ""]
        out.append("/**")
        out.append(" * The engine's global scope: the @GlobalScope utility functions")
        out.append(" * (`print`, `str`, `load`, `range`, ...) and the global enums.")
        out.append(" */")
        out.append("@:keep")
        out.append("class GD {")
        out.append('\tpublic static inline var ENGINE_VERSION:String = "%s";' % self.version)
        for function in self.api.get("utility_functions") or []:
            name = ident(function["name"])
            return_type = self.type_name(function.get("return_type", "void"))
            used = set()
            declarations = []
            expressions = []
            prelude = []
            for arg in function.get("arguments") or []:
                declaration, expression, lines = self.param(arg, used)
                declarations.append(declaration)
                expressions.append(expression)
                prelude.extend(lines)
            if function.get("is_vararg"):
                declarations.append("...__varArgs:Dynamic")
                prelude.append("var __args:Array<Dynamic> = [%s];" % ", ".join(expressions))
                prelude.append("for (__item in __varArgs) __args.push(__item);")
                call_args = "__args"
            else:
                call_args = "[%s]" % ", ".join(expressions)
            call = 'Native.variantCallStatic("Nil", "%s", %s)' % (function["name"], call_args)
            out.append("")
            out.append("\tpublic static function %s(%s):%s {" % (name, ", ".join(declarations), return_type))
            for line in prelude:
                out.append("\t\t" + line)
            out.append("\t\t%s;" % call if return_type == "Void" else "\t\treturn cast %s;" % call)
            out.append("\t}")
        # GDScript/C# global helpers that the engine API dump does not list:
        # `load`, `preload`, `convert`, `is_instance_of`, `type_exists`, `char`,
        # `ord` and the call-stack helpers. They are written in Haxe and call
        # the same engine functions the GDScript built-ins use.
        out.append("")
        out.append("\t/** GDScript's `load()` and C#'s `GD.Load()`: the resource at `path`.")
        out.append("\t * Assign it to a typed variable, or call `load_as`, for a typed value. */")
        out.append("\tpublic static function load(path:String):Dynamic {")
        out.append("\t\treturn cast ResourceLoader.instance().load(path);")
        out.append("\t}")
        out.append("")
        out.append("\t/** `load()` with a typed result. Haxe infers the type from the")
        out.append("\t * assignment: `var tex:godot.Texture2D = GD.load_as(path);`. */")
        out.append("\tpublic static inline function load_as<T>(path:String):T {")
        out.append("\t\treturn cast load(path);")
        out.append("\t}")
        out.append("")
        out.append("\t/** Alias for `load()`. Haxe resolves resources at run time; there is")
        out.append("\t * no compile-time `preload`. */")
        out.append("\tpublic static inline function preload(path:String):Dynamic {")
        out.append("\t\treturn load(path);")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `convert()`: converts `value` to a Variant `type`. */")
        out.append("\tpublic static inline function convert(value:Dynamic, type:Int):Dynamic {")
        out.append("\t\treturn type_convert(value, type);")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `is_instance_of()`: whether `value` has Variant `type`. */")
        out.append("\tpublic static function is_instance_of(value:Dynamic, type:Int):Bool {")
        out.append("\t\treturn typeof(value) == type;")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `type_exists()`: whether `type` names an engine class. */")
        out.append("\tpublic static function type_exists(type:String):Bool {")
        out.append("\t\treturn ClassDB.instance().class_exists(type);")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `char()`: the character for a Unicode code point. */")
        out.append("\tpublic static inline function char(code:Int):String {")
        out.append("\t\treturn String.fromCharCode(code);")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `ord()`: the Unicode code point of the first character. */")
        out.append("\tpublic static inline function ord(character:String):Int {")
        out.append("\t\treturn character.length == 0 ? 0 : StringTools.fastCodeAt(character, 0);")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `get_stack()`: the Haxe call stack as text. */")
        out.append("\tpublic static function get_stack():String {")
        out.append("\t\treturn haxe.CallStack.toString(haxe.CallStack.callStack());")
        out.append("\t}")
        out.append("")
        out.append("\t/** GDScript's `print_stack()`: prints the Haxe call stack. */")
        out.append("\tpublic static function print_stack():Void {")
        out.append("\t\tprint(get_stack());")
        out.append("\t}")
        out.append("}")
        for enum in self.api.get("global_enums") or []:
            block = self.enum_block(enum)
            out.append("")
            if "." in enum["name"]:
                owner, short = enum["name"].split(".", 1)
                block = block.replace("enum abstract %s(" % enum["name"], "enum abstract %s_%s(" % (owner, short))
            out.append(block)
        return "\n".join(out) + "\n"

    def registry_file(self, generated):
        out = [HEADER % self.version, "package godot;", ""]
        out.append("/**")
        out.append(" * Keeps every generated binding class reachable so the engine can wrap")
        out.append(" * objects of any class at runtime, even when the project never names it.")
        out.append(" * The Haxe build compiles with `-dce full`, so this list is what stops")
        out.append(" * the compiler from stripping bindings for classes that only appear")
        out.append(" * inside the engine.")
        out.append(" */")
        out.append("@:keep")
        out.append("class Registry {")
        out.append("\t@:keep public static function types():Array<Class<Dynamic>> {")
        out.append("\t\treturn [")
        for index, name in enumerate(sorted(generated)):
            separator = "," if index != len(generated) - 1 else ""
            out.append("\t\t\tgodot.%s%s" % (name, separator))
        out.append("\t\t];")
        out.append("\t}")
        out.append("}")
        return "\n".join(out) + "\n"

    # ---- editor metadata ------------------------------------------------

    def api_index(self):
        """Compact metadata used by the editor for completion and lookup.

        The generated Haxe sources are the compiler facing API; this index is
        the same information in a machine readable form, so the engine can
        offer completions, member lists and documentation links without
        parsing 1000+ Haxe files or running the language server.
        """
        classes = {}

        def add(cls, is_builtin):
            name = cls["name"]
            if is_builtin and name not in PRIMARY_MEMBERS and name not in SPECIAL_FIELDS:
                return
            entry = {
                "base": cls.get("inherits"),
                "builtin": is_builtin,
                "singleton": cls.get("singleton_name") if cls.get("is_singleton") else None,
                "properties": [],
                "methods": [],
                "static_methods": [],
                "signals": [],
                "enums": {},
                "constants": [],
            }
            # Accessor named methods are shadowed by the property of the same
            # name, exactly like in the generated sources.
            reserved = set()
            for prop in cls.get("properties") or []:
                for direction, engine_name in (("get_", prop.get("getter")), ("set_", prop.get("setter"))):
                    if engine_name:
                        reserved.add(direction + ident(prop["name"]))
            inherited_properties = self.inherited_properties(name)
            for prop in cls.get("properties") or []:
                if prop["name"] in inherited_properties:
                    continue
                _, _, conflict = self.accessor_overrides(name, prop)
                if conflict is not None:
                    continue
                entry["properties"].append([prop["name"], self.type_name(prop["type"]), bool(prop.get("setter"))])
            for method in cls.get("methods") or []:
                if method["name"] in reserved:
                    continue
                return_type = method.get("return_type")
                if return_type is None:
                    return_type = (method.get("return_value") or {}).get("type", "void")
                item = [
                    ident(method["name"]),
                    [self.type_name(a.get("type", "Variant"), True) for a in method.get("arguments") or []],
                    self.type_name(return_type),
                    bool(method.get("is_static")),
                    bool(method.get("is_virtual")) and not is_builtin,
                    bool(method.get("is_vararg")),
                ]
                if item[3]:
                    entry["static_methods"].append(item)
                else:
                    entry["methods"].append(item)
            for constant in cls.get("constants") or []:
                entry["constants"].append([ident(constant["name"]), self.type_name(constant.get("type", "int"))])
            for signal in cls.get("signals") or []:
                entry["signals"].append([
                    signal["name"],
                    [self.type_name(a.get("type", "Variant"), True) for a in signal.get("arguments") or []],
                ])
            for enum in cls.get("enums") or []:
                entry["enums"][enum["name"]] = [ident(v["name"]) for v in enum["values"]]
            if is_builtin:
                for member in cls.get("members") or []:
                    entry["properties"].append([ident(member["name"]), self.type_name(member["type"]), True])
            if name in SPECIAL_FIELDS:
                for field in SPECIAL_FIELDS[name]:
                    pass
            classes[name] = entry

        for cls in self.api["classes"]:
            add(cls, False)
        for cls in self.api["builtin_classes"]:
            add(cls, True)

        global_enums = {}
        for enum in self.api.get("global_enums") or []:
            name = enum["name"]
            if "." in name:
                owner, short = name.split(".", 1)
                name = "%s_%s" % (owner, short)
            global_enums[name] = [ident(v["name"]) for v in enum["values"]]

        return {
            "version": self.version,
            "classes": classes,
            "global_enums": global_enums,
        }

    def run(self):
        os.makedirs(self.out_dir, exist_ok=True)
        generated = []
        for cls in self.api["classes"]:
            source = self.class_file(cls)
            write(os.path.join(self.out_dir, cls["name"] + ".hx"), source)
            generated.append(cls["name"])
        for cls in self.api["builtin_classes"]:
            name = cls["name"]
            if name in MAPPED_BUILTINS:
                continue
            if name not in PRIMARY_MEMBERS and name not in SPECIAL_FIELDS:
                warn("builtin class %s has no representation" % name)
                continue
            write(os.path.join(self.out_dir, name + ".hx"), self.class_file(cls))
            generated.append(name)
        write(os.path.join(self.out_dir, "GD.hx"), self.gd_file())
        write(os.path.join(self.out_dir, "Registry.hx"), self.registry_file(generated))
        return generated


WARNINGS = []
dropped_methods: list[tuple[str, str, str, str]] = []
dropped_properties: list[tuple[str, str, str]] = []


def warn(message):
    WARNINGS.append(message)


def write(path, content):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)


def mark_singletons(api):
    """extension_api.json lists singletons separately; flag their classes."""
    classes = {c["name"]: c for c in api["classes"]}
    for entry in api.get("singletons") or []:
        cls = classes.get(entry["type"])
        if cls is None:
            continue
        cls.setdefault("is_singleton", True)
        cls.setdefault("singleton_name", entry["name"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", default="extension_api.json")
    parser.add_argument(
        "--out", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "std", "godot")
    )
    options = parser.parse_args()

    with open(options.api, "r", encoding="utf-8") as handle:
        api = json.load(handle)
    mark_singletons(api)

    generator = Generator(api, options.out)
    generated = generator.run()
    bindings_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(options.out))), "bindings")
    os.makedirs(bindings_dir, exist_ok=True)
    with open(os.path.join(bindings_dir, "api.json"), "w", encoding="utf-8") as handle:
        json.dump(generator.api_index(), handle, separators=(",", ":"), sort_keys=True)
    print("Generated %d classes in %s" % (len(generated), options.out))
    if dropped_methods:
        print(
            "%d engine method(s) were replaced by a property accessor of the "
            "same name (still reachable through hxCall()):" % len(dropped_methods),
            file=sys.stderr,
        )
        for owner, accessor, engine_name, prop_name in dropped_methods:
            print(
                "  %s::%s() -> property %s (getter/setter %s)" % (owner, accessor, prop_name, engine_name),
                file=sys.stderr,
            )
    if dropped_properties:
        print(
            "%d property(ies) are only reachable through their engine methods, "
            "because Haxe cannot override the inherited accessor:" % len(dropped_properties),
            file=sys.stderr,
        )
        for owner, prop_name, reason in dropped_properties:
            print("  %s.%s -> %s" % (owner, prop_name, reason), file=sys.stderr)
    if WARNINGS:
        print("%d warning(s):" % len(WARNINGS), file=sys.stderr)
        for message in WARNINGS[:40]:
            print("  " + message, file=sys.stderr)
        if len(WARNINGS) > 40:
            print("  ... %d more" % (len(WARNINGS) - 40), file=sys.stderr)


if __name__ == "__main__":
    main()
