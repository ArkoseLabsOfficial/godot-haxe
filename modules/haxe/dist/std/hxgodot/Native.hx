/**************************************************************************/
/*  Native.hx                                                             */
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

package hxgodot;

/**
 * Native bridge between Haxe and the Godot engine module.
 *
 * Every function declared here is implemented in `glue.cpp`, which is compiled
 * into each generated project library through `@:buildXml` below. The engine
 * itself never links against hxcpp: it only loads the resulting shared library
 * and talks to it through the plain C ABI described in `hxgodot_abi.h`.
 *
 * This class is an implementation detail of the generated bindings. User code
 * should use `godot.*` classes and `hxgodot.GD` instead.
 */
@:include("hxgodot_glue.h")
@:buildXml('
<files id="haxe">
  <compilerflag value="-I${hxgodot_native}" />
</files>
<files id="hxgodot_glue" dir="${hxgodot_native}">
  <compilerflag value="-I${hxgodot_native}" />
  <file name="glue.cpp" />
</files>
<target id="haxe">
  <files id="hxgodot_glue" />
</target>
')
@:keep
extern class Native {
	/** Calls an engine method through Object::callp. */
	@:native("hxgodot_haxe_call")
	public static function call(objectId:String, method:String, args:Array<Dynamic>):Dynamic;

	/** Calls a static method through ClassDB. */
	@:native("hxgodot_haxe_call_static")
	public static function callStatic(className:String, method:String, args:Array<Dynamic>):Dynamic;

	/** Reads or writes an engine property through Object::get/set. */
	@:native("hxgodot_haxe_get_property")
	public static function getProperty(objectId:String, name:String):Dynamic;

	@:native("hxgodot_haxe_set_property")
	public static function setProperty(objectId:String, name:String, value:Dynamic):Void;

	@:native("hxgodot_haxe_has_method")
	public static function hasMethod(objectId:String, method:String):Bool;

	@:native("hxgodot_haxe_has_property")
	public static function hasProperty(objectId:String, name:String):Bool;

	/** Calls a method of a built-in value (Vector2, Transform3D, Callable, ...). */
	@:native("hxgodot_haxe_variant_call")
	public static function variantCall(target:Dynamic, method:String, args:Array<Dynamic>):Dynamic;

	@:native("hxgodot_haxe_variant_call_static")
	public static function variantCallStatic(typeName:String, method:String, args:Array<Dynamic>):Dynamic;

	/**
	 * Calls a non-const built-in method (Dictionary and packed array
	 * operations that are not implemented in Haxe). The updated value is
	 * copied back into `target`.
	 */
	@:native("hxgodot_haxe_variant_call_mutating")
	public static function variantCallMutating(target:Dynamic, method:String, args:Array<Dynamic>):Dynamic;

	/** Reads a derived member of a built-in value through Variant::get_named. */
	@:native("hxgodot_haxe_get_member")
	public static function getMember(target:Dynamic, name:String):Dynamic;

	/**
	 * Writes a derived member of a built-in value. The engine computes the new
	 * value and the primary fields are copied back into `target`.
	 */
	@:native("hxgodot_haxe_set_member")
	public static function setMember(target:Dynamic, name:String, value:Dynamic):Void;

	/** Variant equality (deep for arrays and dictionaries). */
	@:native("hxgodot_haxe_value_equal")
	public static function valueEqual(a:Dynamic, b:Dynamic):Bool;

	/** Variant ordering: -1, 0 or 1. */
	@:native("hxgodot_haxe_value_compare")
	public static function valueCompare(a:Dynamic, b:Dynamic):Int;

	/** Instantiates an engine class (ClassDB::instantiate). */
	@:native("hxgodot_haxe_create_object")
	public static function createObject(className:String):Dynamic;

	/** Retrieves an engine singleton such as `Engine` or `Input`. */
	@:native("hxgodot_haxe_singleton")
	public static function singleton(name:String):Dynamic;

	@:native("hxgodot_haxe_is_valid")
	public static function isValid(objectId:String):Bool;

	@:native("hxgodot_haxe_class_name")
	public static function className(objectId:String):String;

	@:native("hxgodot_haxe_is_ref_counted")
	public static function isRefCounted(objectId:String):Bool;

	/**
	 * Uploads raw pixel data into an ImageTexture without marshalling a
	 * `PackedByteArray`. `pixels` points at `size` bytes laid out in `format`
	 * (a Godot Image::Format). Used by media decoders that own a native frame
	 * buffer. The pointer is the byte pointer `cpp.NativeArray.getBase()`
	 * yields, so no cast is needed on the caller side.
	 */
	@:native("hxgodot_haxe_update_texture")
	public static function updateTexture(textureId:String, pixels:cpp.RawPointer<cpp.Char>, size:Int, width:Int, height:Int, format:Int):Bool;

	/**
	 * Pushes interleaved signed 16 bit PCM frames into an
	 * AudioStreamGeneratorPlayback, converting to the engine's float frames on
	 * the host. Returns the number of frames pushed, or a negative value on
	 * failure. `samples` is the byte pointer `cpp.NativeArray.getBase()` yields.
	 */
	@:native("hxgodot_haxe_push_audio_frames")
	public static function pushAudioFrames(playbackId:String, samples:cpp.RawPointer<cpp.Char>, frames:Int, channels:Int):Int;

	/**
	 * Returns the id of the engine object the current script constructor is
	 * building when `wrapper` is that object, and an empty string otherwise.
	 * It lets the generated `super()` chain bind the existing node before the
	 * constructor body runs, so engine calls made from `new()` reach it.
	 */
	@:native("hxgodot_haxe_script_self_id")
	public static function scriptSelfId(wrapper:Dynamic):String;

	/**
	 * Creates the engine object behind a generated `new()`.
	 * Returns an empty string when the engine owns object creation (script
	 * instances, where the node already exists).
	 */
	@:native("hxgodot_haxe_new_object")
	public static function newObject(className:String):String;

	/** Stores the instance id on a wrapper and pins the object if needed. */
	@:native("hxgodot_haxe_bind_wrapper")
	public static function bindWrapper(wrapper:Dynamic, objectId:String):Void;

	/** True while the engine runs a Haxe script constructor. */
	@:native("hxgodot_haxe_in_script_creation")
	public static function inScriptCreation():Bool;

	/** Turns a Haxe function into a closure id owned by this library. */
	@:native("hxgodot_haxe_store_closure")
	public static function storeClosure(fn:Dynamic):Int;

	/** Calls a closure previously created by storeClosure. */
	@:native("hxgodot_haxe_call_closure")
	public static function callClosure(closureId:Int, args:Array<Dynamic>):Dynamic;

	@:native("hxgodot_haxe_attach_closure_finalizer")
	public static function attachClosureFinalizer(wrapper:Dynamic, closureId:Int):Void;

	@:native("hxgodot_haxe_attach_handle_finalizer")
	public static function attachHandleFinalizer(wrapper:Dynamic, handle:Int):Void;

	/** Prints through the engine's output system. */
	@:native("hxgodot_haxe_log")
	public static function log(level:Int, message:String):Void;

	@:native("hxgodot_haxe_engine_version")
	public static function engineVersion():String;

	@:native("hxgodot_haxe_type_name")
	public static function typeName(value:Dynamic):String;
}
