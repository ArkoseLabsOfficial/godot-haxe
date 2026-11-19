package hxgodot;

import hxgodot.Native;

/**
 * Base class of every generated engine class binding.
 *
 * An instance wraps a live engine object by its instance id. Created objects
 * are registered with the engine through `Native.newObject`, which returns the
 * id of the freshly instantiated object. Script instances are created by the
 * engine instead: there the id is empty while the constructor runs and the
 * engine assigns it afterwards.
 */
@:keep
class NativeObject {
	/** Engine object id as decimal text. Empty until the object is bound. */
	@:keep public var __hxgodotId:String = "";

	public function new(?__engineClass:String) {
		if (__engineClass == null) {
			return;
		}
		var id = Native.newObject(__engineClass);
		if (id.length > 0) {
			Native.bindWrapper(this, id);
		}
	}

	/** True while the engine object still exists. */
	public function hxValid():Bool {
		return __hxgodotId.length > 0 && Native.isValid(__hxgodotId);
	}

	/** Native class name of the wrapped object, e.g. `"Node2D"`. */
	public function hxClassName():String {
		return __hxgodotId.length > 0 ? Native.className(__hxgodotId) : "";
	}

	/** Calls an engine method by its engine name. */
	public function hxCall(method:String, args:Array<Dynamic> = null):Dynamic {
		return Native.call(__hxgodotId, method, args == null ? [] : args);
	}

	/** Reads an engine property by its engine name. */
	public function hxGet(name:String):Dynamic {
		return Native.getProperty(__hxgodotId, name);
	}

	/** Writes an engine property by its engine name. */
	public function hxSet(name:String, value:Dynamic):Void {
		Native.setProperty(__hxgodotId, name, value);
	}

	public function hxHasMethod(method:String):Bool {
		return Native.hasMethod(__hxgodotId, method);
	}

	public function hxHasProperty(name:String):Bool {
		return Native.hasProperty(__hxgodotId, name);
	}

	/** The engine `Signal` value for one of this object's signals. */
	public function hxSignal(name:String):godot.Signal {
		return godot.Signal.fromObject(this, name);
	}

	/** Variant equality: true when both wrappers refer to the same object. */
	public function hxEquals(other:Dynamic):Bool {
		if (other == null) {
			return false;
		}
		var other_id:String = untyped other.__hxgodotId;
		return other_id != null && other_id.length > 0 && other_id == __hxgodotId;
	}

	public function toString():String {
		if (__hxgodotId.length == 0) {
			return "[unbound " + Type.getClassName(Type.getClass(this)) + "]";
		}
		return Native.className(__hxgodotId);
	}
}
