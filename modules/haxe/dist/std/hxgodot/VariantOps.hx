package hxgodot;

import hxgodot.Native;

/**
 * Value semantics shared by the generated bindings.
 *
 * Godot compares values with `Variant::evaluate`, which is deep for arrays and
 * dictionaries and structural for math types. The engine callbacks used here
 * run exactly that code, so a value stored from Haxe and a value coming back
 * from the engine always compare the same way.
 */
class VariantOps {
	/** Deep equality with engine semantics (`==`). */
	public static inline function equal(a:Dynamic, b:Dynamic):Bool {
		return Native.valueEqual(a, b);
	}

	/**
	 * Key identity for `godot.Dictionary`.
	 *
	 * Godot hashes keys, so dictionary lookups use the same equality as `==`.
	 * Haxe `Map` cannot be used for this because its keys are compared with
	 * Haxe identity rules.
	 */
	public static inline function sameKey(a:Dynamic, b:Dynamic):Bool {
		return Native.valueEqual(a, b);
	}

	/** Ordering: -1 when `a < b`, 1 when `a > b`, 0 otherwise. */
	public static function compare(a:Dynamic, b:Dynamic):Int {
		return Native.valueCompare(a, b);
	}

	/**
	 * Copy of a packed array or dictionary payload as a plain Haxe array.
	 * Returns `null` for values that are not containers.
	 */
	public static function toArray(value:Dynamic):Array<Dynamic> {
		if (value == null) {
			return null;
		}
		if (Std.isOfType(value, Array)) {
			return value;
		}
		var data:Array<Dynamic> = untyped value.__data;
		if (data != null) {
			return data.copy();
		}
		var keys:Array<Dynamic> = untyped value.__keys;
		if (keys != null) {
			return keys.copy();
		}
		return null;
	}

	/**
	 * Type name as the engine sees it (`"Vector2"`, `"Object"`, ...). Useful in
	 * diagnostics and for typed `Array` checks.
	 */
	public static function typeName(value:Dynamic):String {
		return Native.typeName(value);
	}
}
