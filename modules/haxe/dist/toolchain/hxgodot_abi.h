/**************************************************************************/
/*  hxgodot_abi.h                                                         */
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
/*  hxgodot_abi.h                                                          */
/*                                                                           */
/*  Plain C ABI shared between the Godot Haxe module and the Haxe/hxcpp      */
/*  shared libraries produced from a Godot project.                          */
/*                                                                           */
/*  This header is the single source of truth for the binary contract. It is */
/*  shipped with the engine (bin/haxe/toolchain) and compiled into every     */
/*  project library by the generated glue, so both sides are guaranteed to   */
/*  agree. It must stay C compatible: no engine and no hxcpp headers here.   */
/*                                                                           */
/*  Versioning: bump HXGODOT_ABI_VERSION whenever HxValue, HxHostApi or the  */
/*  exported entry points change in an incompatible way. Libraries built     */
/*  against another version are rejected instead of being loaded.            */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HXGODOT_ABI_VERSION 6

#if defined(_WIN32)
#define HXGODOT_EXPORT __declspec(dllexport)
#else
#define HXGODOT_EXPORT __attribute__((visibility("default")))
#endif

/* Value tags. The numeric order is part of the on disk/wire format used by
 * the binary blob encoding, so entries are only ever appended. */
enum HxValueType {
	HXG_NIL = 0,
	HXG_BOOL = 1,
	HXG_INT = 2,
	HXG_FLOAT = 3,
	HXG_STRING = 4,
	HXG_STRING_NAME = 5,
	HXG_NODE_PATH = 6,
	HXG_VECTOR2 = 7,
	HXG_VECTOR2I = 8,
	HXG_RECT2 = 9,
	HXG_RECT2I = 10,
	HXG_VECTOR3 = 11,
	HXG_VECTOR3I = 12,
	HXG_TRANSFORM2D = 13,
	HXG_VECTOR4 = 14,
	HXG_VECTOR4I = 15,
	HXG_PLANE = 16,
	HXG_QUATERNION = 17,
	HXG_AABB = 18,
	HXG_BASIS = 19,
	HXG_TRANSFORM3D = 20,
	HXG_PROJECTION = 21,
	HXG_COLOR = 22,
	HXG_RID = 23,
	HXG_OBJECT = 24,
	HXG_CALLABLE = 25,
	HXG_SIGNAL = 26,
	HXG_ARRAY = 27,
	HXG_DICTIONARY = 28,
	HXG_PACKED_BYTE_ARRAY = 29,
	HXG_PACKED_INT32_ARRAY = 30,
	HXG_PACKED_INT64_ARRAY = 31,
	HXG_PACKED_FLOAT32_ARRAY = 32,
	HXG_PACKED_FLOAT64_ARRAY = 33,
	HXG_PACKED_STRING_ARRAY = 34,
	HXG_PACKED_VECTOR2_ARRAY = 35,
	HXG_PACKED_VECTOR3_ARRAY = 36,
	HXG_PACKED_COLOR_ARRAY = 37,
	HXG_PACKED_VECTOR4_ARRAY = 38,
	/* A Callable that is not a plain object+method pair (a CallableCustom,
	 * a bound callable, ...). `i` holds a handle owned by the host. */
	HXG_CALLABLE_HANDLE = 39,
	/* A Callable implemented by Haxe code. `i` holds a closure id owned by
	 * the library that produced it. */
	HXG_CLOSURE = 40,
	/* Like HXG_OBJECT, but the sender also hands the receiver one owned
	 * reference to the object: the receiver keeps the object alive with it and
	 * releases it with HxHostApi::unref_object when it is done. Used inside
	 * blobs, where the per-value `owned` field is not available. */
	HXG_OBJECT_PINNED = 41,
	HXG_TYPE_MAX = 42,
};

/* A single value crossing the engine/library boundary.
 *
 * Scalars, strings, object handles, callables and the fixed size math types
 * are inlined: `i` and `f` carry the payload, `s` points at UTF-8 text.
 *
 * Containers and the linear algebra types that do not fit in four doubles
 * (AABB, Basis, Transform2D, Transform3D, Projection, and any nested value
 * inside a container) travel in `blob`. A blob always starts with the
 * complete tagged encoding, including the tag byte itself, so the decoder
 * can be shared for top level and nested values.
 *
 * Blob encoding, little endian:
 *   bool            u8 tag, u8 value
 *   int             u8 tag, u64
 *   float           u8 tag, f64
 *   string-ish      u8 tag, u32 byte length, UTF-8 bytes
 *   object          u8 tag, u64 instance id
 *   pinned object   u8 tag, u64 instance id, one owned reference handed over
 *   callable        u8 tag, u64 object id, u32 method length, UTF-8 method
 *   handles         u8 tag, u64 handle
 *   array           u8 tag, u32 count, value*
 *   dictionary      u8 tag, u32 count, (key, value)*
 *   packed byte     u8 tag, u32 count, u8*
 *   packed int      u8 tag, u32 count, u64*
 *   packed float    u8 tag, u32 count, f64*
 *   packed string   u8 tag, u32 count, text*
 *   packed vector2  u8 tag, u32 count, f64 x, f64 y
 *   packed vector3  u8 tag, u32 count, f64 x, f64 y, f64 z
 *   packed color    u8 tag, u32 count, f64 r, g, b, a
 *   packed vector4  u8 tag, u32 count, f64 x, y, z, w
 *   vector2/2i      u8 tag, f64 x, y
 *   vector3/3i      u8 tag, f64 x, y, z
 *   vector4/4i      u8 tag, f64 x, y, z, w
 *   rect2/2i        u8 tag, f64 position.x, position.y, size.x, size.y
 *   plane           u8 tag, f64 normal.x, normal.y, normal.z, d
 *   quaternion      u8 tag, f64 x, y, z, w
 *   color           u8 tag, f64 r, g, b, a
 *   rid             u8 tag, u64
 *   aabb            u8 tag, f64 position.x, position.y, position.z, size.x, size.y, size.z
 *   basis           u8 tag, 9 * f64, row major
 *   transform2d     u8 tag, 6 * f64, columns
 *   transform3d     u8 tag, 9 * f64 basis row major, 3 * f64 origin
 *   projection      u8 tag, 16 * f64, columns
 *
 * Memory referenced by `s` and `blob` is owned by the sender and is only
 * guaranteed to be valid for the duration of the call it belongs to, plus the
 * explicit release call for values returned by a library. Receivers copy
 * everything they keep.
 *
 * RefCounted objects are the exception: a value returned by the host may be
 * the last owner of the object, so the engine keeps one reference alive and
 * marks the value as `owned` (or uses HXG_OBJECT_PINNED inside a blob). The
 * library takes that reference over and releases it with `unref_object` once
 * nothing references the object anymore.
 */
typedef struct HxValue {
	int32_t type;
	int32_t owned; /* 1 when the value also carries one owned reference to `i` */
	int64_t i; /* bool, int, object id, rid, callable handle, closure id */
	double f[4]; /* float, vector2, vector2i, rect2, rect2i, vector3, vector3i, vector4, plane, quaternion, color */
	const char *s; /* UTF-8 text for strings, string names, node paths and callable methods */
	const uint8_t *blob;
	int64_t blob_size;
} HxValue;

/* Host services used by the library. Appended fields keep old layouts valid,
 * but the version check makes mixing versions impossible on purpose. */
typedef struct HxHostApi {
	int32_t version;

	/* Diagnostics. `level` is 0 for normal output, 1 for warnings and 2 for
	 * errors. The host copies the message before returning. */
	void (*log)(int p_level, const char *p_message);

	/* Object method call. Returns 0 on success, 1 when the object has no such
	 * method, 2 when the call failed (invalid arguments or a runtime error)
	 * and 3 when the object no longer exists. */
	int (*call)(int64_t p_object, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);

	/* Property access. Same return codes as call(). */
	int (*get_property)(int64_t p_object, const char *p_name, HxValue *r_ret);
	int (*set_property)(int64_t p_object, const char *p_name, const HxValue *p_value);

	int (*has_method)(int64_t p_object, const char *p_method);
	int (*is_instance_valid)(int64_t p_object);

	/* Logical native class name of an object, e.g. "Node2D". Writes a
	 * null-terminated UTF-8 string. Returns 0 on success. */
	int (*get_class_name)(int64_t p_object, char *r_out, int p_out_size);

	/* RefCounted lifetime. `ref_object` pins the object for as long as a Haxe
	 * wrapper references it; `unref_object` releases one pin. Objects created
	 * by `create_object` start unpinned and are pinned by the wrapper. The
	 * reference handed over with an `owned` return value is released the same
	 * way. */
	void (*ref_object)(int64_t p_object);
	void (*unref_object)(int64_t p_object);

	/* Calls a Callable that was handed to Haxe through HXG_CALLABLE_HANDLE. */
	int (*call_callable)(int64_t p_handle, const HxValue *p_args, int p_argc, HxValue *r_ret);
	void (*free_callable)(int64_t p_handle);

	/* Calls a static method through ClassDB. Used by generated bindings for
	 * static methods. */
	int (*call_static)(const char *p_class, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);

	/* Retrieves an engine singleton object, e.g. "Engine" or "OS". */
	int (*get_singleton)(const char *p_name, int64_t *r_object);

	/* Small engine utility helpers. `p_op` is documented in
	 * HaxeRuntime::host_utility(). */
	int (*utility)(const char *p_op, int64_t p_object, const char *p_text, HxValue *r_ret);

	/* Calls a method on a built-in value (Vector2, Transform3D, ...) through
	 * Variant, so generated bindings for math types never reimplement engine
	 * semantics. `p_target` must carry a built-in value tag. */
	int (*variant_call)(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);
	/* Calls a static built-in method, e.g. Vector2.from_angle. `p_type` is the
	 * engine type name ("Vector2"). */
	int (*variant_call_static)(const char *p_type, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);

	/* Reads or writes a member of a built-in value through Variant::get_named
	 * and Variant::set_named. Derived members (Rect2.end, Color.h, ...) are
	 * not stored in the Haxe wrapper, so the engine computes them from the
	 * value itself. `variant_set_member` writes the updated value to
	 * `r_updated`; the library copies the primary fields back. */
	int (*variant_get_member)(const HxValue *p_target, const char *p_name, HxValue *r_ret);
	int (*variant_set_member)(const HxValue *p_target, const char *p_name, const HxValue *p_value, HxValue *r_updated);

	/* Instantiates an engine class (ClassDB::instantiate). Returns 0 and the
	 * object id on success. RefCounted results are not pinned: the caller's
	 * wrapper pins them like any other object it receives. */
	int (*create_object)(const char *p_class, int64_t *r_object);

	int (*is_ref_counted)(int64_t p_object);
	int (*has_property)(int64_t p_object, const char *p_name);

	/* Variant comparison, so values compared in Haxe behave exactly like the
	 * engine: `value_equal` mirrors `Variant::evaluate(OP_EQUAL)` (deep for
	 * arrays and dictionaries, structural for math values) and
	 * `value_compare` mirrors OP_LESS/OP_GREATER. Both return 0 when the
	 * comparison is defined and 1 when the operands cannot be ordered. */
	int (*value_equal)(const HxValue *p_a, const HxValue *p_b, int *r_equal);
	int (*value_compare)(const HxValue *p_a, const HxValue *p_b, int *r_compare);

	/* Calls a non-const built-in method. The engine calls the method on its
	 * own copy, so the library needs the updated value to copy the primary
	 * fields back into the Haxe wrapper. Used by Dictionary and the packed
	 * arrays for the operations that are not implemented in Haxe. */
	int (*variant_call_mutating)(const HxValue *p_target, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret, HxValue *r_updated);

	/* Uploads raw pixel data into an ImageTexture. The library passes a
	 * pointer to its own buffer instead of a Haxe `PackedByteArray`, so a
	 * multi megabyte frame does not have to be marshalled element by element
	 * (plus a Haxe `Array<Dynamic>` allocation) before every display. The host
	 * copies `p_size` bytes and validates them against the format. `p_format`
	 * is a Godot Image::Format. Returns 0 on success, 1 when the object is not
	 * an ImageTexture and 2 on a bad argument or size. */
	int (*update_texture)(int64_t p_texture, const void *p_pixels, int64_t p_size, int p_width, int p_height, int p_format);

	/* Pushes interleaved signed 16 bit PCM samples into an
	 * AudioStreamGeneratorPlayback. The host converts them to the engine's
	 * float frames, so a frame of audio does not allocate one `Vector2` plus
	 * one boxed `Dynamic` per sample. Returns the number of frames pushed, or
	 * a negative value on failure (-1 no playback, -2 no data). */
	int (*push_audio_frames)(int64_t p_playback, const void *p_samples, int p_frames, int p_channels);

	/* Virtual file system. These let a library stream a file through the
	 * engine's own FileAccess, so entries that only exist inside an exported
	 * pack (res://) or an encrypted archive can be read by a decoder that
	 * brings its own I/O (LibVLC, for example) without the whole file ever
	 * being resident in memory. They are generic: any path FileAccess accepts
	 * works, and the functions only claim the file exists if FileAccess can
	 * open it.
	 *
	 * `file_open` opens a file for reading, returns a non-zero handle and
	 * writes the file size in bytes to `r_size`, or -1 when the size is not
	 * known. It returns 0 when the file cannot be opened. `file_size` returns
	 * the size of a handle, or -1. `file_read` reads at most `p_size` bytes
	 * from the handle's current position into `p_out` and returns the number
	 * of bytes read (0 at end of file), or a negative value on error. The
	 * position advances by the number of bytes read. `file_seek` moves the
	 * position and returns 0 on success. `file_close` releases the handle.
	 *
	 * Handles are refcounted and safe to use from a thread other than the one
	 * that opened them; a single handle is not meant to be read from two
	 * threads at once. */
	int64_t (*file_open)(const char *p_path, int64_t *r_size);
	int64_t (*file_size)(int64_t p_handle);
	int64_t (*file_read)(int64_t p_handle, void *p_out, int64_t p_size);
	int64_t (*file_seek)(int64_t p_handle, int64_t p_position);
	void (*file_close)(int64_t p_handle);

	/* Streaming audio for a decoder that hands over samples from its own
	 * thread, instead of the main loop filling an `AudioStreamGenerator`.
	 *
	 * `audio_stream_open` creates the stream and returns a handle (0 on
	 * failure). `p_mix_rate` is the sample rate of the samples that will be
	 * written, `p_buffer_len` the seconds the ring can hold and `p_target_len`
	 * the seconds of backlog the consumer aims for (the latency the sound
	 * accepts so it does not underrun). `audio_stream_attach` points an
	 * `AudioStreamPlayer` at the stream and must be called on the main thread,
	 * before the player is played.
	 *
	 * `audio_stream_write` writes interleaved signed 16 bit frames. It is the
	 * only entry point meant to be called from a non-main thread (the decoder's
	 * audio callback) and returns the number of frames taken, or a negative
	 * value when the handle is unknown. It never blocks. `audio_stream_buffered`
	 * reports the frames queued right now. `audio_stream_reset` discards the
	 * queue and rebuilds the backlog (a seek, or the start of a loop).
	 * `audio_stream_stat` reports a running counter: 0 dropped frames, 1 mixes
	 * that ran short, 2 frames trimmed to bound a runaway backlog.
	 * `audio_stream_close` releases the handle; a player still holding the
	 * stream keeps it alive until it is freed. */
	int64_t (*audio_stream_open)(double p_mix_rate, double p_buffer_len, double p_target_len);
	int (*audio_stream_attach)(int64_t p_handle, int64_t p_player);
	int (*audio_stream_write)(int64_t p_handle, const void *p_samples, int p_frames, int p_channels);
	int64_t (*audio_stream_buffered)(int64_t p_handle);
	void (*audio_stream_reset)(int64_t p_handle);
	int64_t (*audio_stream_stat)(int64_t p_handle, int p_which);
	void (*audio_stream_close)(int64_t p_handle);

} HxHostApi;

/* ---- Engine -> library -------------------------------------------------- */

/* Installs the host API. Must be called once before hxgodot_start(). */
HXGODOT_EXPORT int hxgodot_install(const HxHostApi *p_api);

/* Boots the embedded hxcpp runtime and registers every generated class.
 * Idempotent. Returns 0 on success. */
HXGODOT_EXPORT int hxgodot_start(void);

/* Human readable description of the last failure. Never null. */
HXGODOT_EXPORT const char *hxgodot_last_error(void);

/* Stack trace captured for the last failed call, or an empty string. */
HXGODOT_EXPORT const char *hxgodot_last_stack_trace(void);

/* Newline separated "key=value" build metadata (abi, compiler, build date,
 * class count, ...). Used by the editor to detect stale libraries. */
HXGODOT_EXPORT const char *hxgodot_library_info(void);

HXGODOT_EXPORT int hxgodot_has_class(const char *p_class_name);

/* Creates a script instance for the given engine object. The class must have a
 * public constructor with no required arguments. Returns 0 on success. */
HXGODOT_EXPORT int hxgodot_create(int64_t p_object_id, const char *p_class_name);

HXGODOT_EXPORT int hxgodot_has_method(int64_t p_object_id, const char *p_method);

/* Returns 0 on success, 1 when the method is absent, 2 on failure. */
HXGODOT_EXPORT int hxgodot_invoke(int64_t p_object_id, const char *p_method, const HxValue *p_args, int p_argc, HxValue *r_ret);

HXGODOT_EXPORT int hxgodot_get_field(int64_t p_object_id, const char *p_name, HxValue *r_ret);
HXGODOT_EXPORT int hxgodot_set_field(int64_t p_object_id, const char *p_name, const HxValue *p_value);

/* Enumerates instance member names. `r_out` is a newline separated list; the
 * caller passes a buffer and its capacity. Returns the required size. */
HXGODOT_EXPORT int hxgodot_get_members(int64_t p_object_id, char *r_out, int p_out_size);

HXGODOT_EXPORT void hxgodot_destroy(int64_t p_object_id);

/* Releases the memory behind the last value returned by this library on the
 * calling thread. The host calls it after converting the return value. */
HXGODOT_EXPORT void hxgodot_release_return(void);

/* Calls a Haxe closure by id. Used by Callable values that the engine holds
 * for Haxe code (signal connections, timers, ...). */
HXGODOT_EXPORT int hxgodot_invoke_closure(int64_t p_closure, const HxValue *p_args, int p_argc, HxValue *r_ret);

/* Closure lifetime. A closure is created by the library with one reference and
 * every holder (the Haxe Callable wrapper and every engine Callable) keeps its
 * own reference. */
HXGODOT_EXPORT void hxgodot_retain_closure(int64_t p_closure);
HXGODOT_EXPORT void hxgodot_release_closure(int64_t p_closure);

#ifdef __cplusplus
}
#endif
