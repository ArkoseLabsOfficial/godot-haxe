/**************************************************************************/
/*  haxe_value.cpp                                                        */
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

#include "haxe_value.h"

#include "haxe_runtime.h"

#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/variant/callable.h"
#include "core/variant/variant.h"

namespace {

void put_u8(Vector<uint8_t> &r_blob, uint8_t p_value) {
	r_blob.push_back(p_value);
}

void put_u32(Vector<uint8_t> &r_blob, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		r_blob.push_back((uint8_t)((p_value >> (i * 8)) & 0xFF));
	}
}

void put_u64(Vector<uint8_t> &r_blob, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		r_blob.push_back((uint8_t)((p_value >> (i * 8)) & 0xFF));
	}
}

void put_f64(Vector<uint8_t> &r_blob, double p_value) {
	uint64_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	put_u64(r_blob, bits);
}

void put_text(Vector<uint8_t> &r_blob, const String &p_text) {
	CharString utf8 = p_text.utf8();
	put_u32(r_blob, (uint32_t)utf8.length());
	for (int i = 0; i < utf8.length(); i++) {
		r_blob.push_back((uint8_t)utf8[i]);
	}
}

class Reader {
public:
	Reader(const uint8_t *p_data, int64_t p_size) :
			data(p_data), size(p_size) {}

	bool read_u8(uint8_t &r_value) {
		if (pos + 1 > size) {
			return false;
		}
		r_value = data[pos++];
		return true;
	}

	bool read_u32(uint32_t &r_value) {
		if (pos + 4 > size) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 4; i++) {
			r_value |= (uint32_t)data[pos + i] << (i * 8);
		}
		pos += 4;
		return true;
	}

	bool read_u64(uint64_t &r_value) {
		if (pos + 8 > size) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 8; i++) {
			r_value |= (uint64_t)data[pos + i] << (i * 8);
		}
		pos += 8;
		return true;
	}

	bool read_f64(double &r_value) {
		uint64_t bits = 0;
		if (!read_u64(bits)) {
			return false;
		}
		memcpy(&r_value, &bits, sizeof(r_value));
		return true;
	}

	bool read_text(String &r_text) {
		uint32_t length = 0;
		if (!read_u32(length)) {
			return false;
		}
		if ((int64_t)length > size - pos) {
			return false;
		}
		r_text = String::utf8((const char *)data + pos, length);
		pos += length;
		return true;
	}

	bool at_end() const { return pos >= size; }
	int64_t remaining() const { return size - pos; }

private:
	const uint8_t *data = nullptr;
	int64_t size = 0;
	int64_t pos = 0;
};

Variant decode_value(Reader &p_reader);

Variant decode_array(Reader &p_reader) {
	uint32_t count = 0;
	if (!p_reader.read_u32(count)) {
		return Variant();
	}
	Array array;
	for (uint32_t i = 0; i < count; i++) {
		array.push_back(decode_value(p_reader));
	}
	return array;
}

Variant decode_dictionary(Reader &p_reader) {
	uint32_t count = 0;
	if (!p_reader.read_u32(count)) {
		return Variant();
	}
	Dictionary dictionary;
	for (uint32_t i = 0; i < count; i++) {
		Variant key = decode_value(p_reader);
		Variant value = decode_value(p_reader);
		dictionary[key] = value;
	}
	return dictionary;
}

Variant decode_value(Reader &p_reader) {
	uint8_t tag = 0;
	if (!p_reader.read_u8(tag)) {
		return Variant();
	}

	switch (tag) {
		case HXG_NIL:
			return Variant();
		case HXG_BOOL: {
			uint8_t value = 0;
			if (!p_reader.read_u8(value)) {
				return Variant();
			}
			return value != 0;
		}
		case HXG_INT: {
			int64_t value = 0;
			if (!p_reader.read_u64((uint64_t &)value)) {
				return Variant();
			}
			return value;
		}
		case HXG_FLOAT: {
			double value = 0;
			if (!p_reader.read_f64(value)) {
				return Variant();
			}
			return value;
		}
		case HXG_STRING:
		case HXG_STRING_NAME:
		case HXG_NODE_PATH: {
			String text;
			if (!p_reader.read_text(text)) {
				return Variant();
			}
			if (tag == HXG_STRING) {
				return text;
			}
			if (tag == HXG_STRING_NAME) {
				return StringName(text);
			}
			return NodePath(text);
		}
		case HXG_OBJECT:
		case HXG_OBJECT_PINNED: {
			uint64_t id = 0;
			if (!p_reader.read_u64(id)) {
				return Variant();
			}
			Object *object = ObjectDB::get_instance(ObjectID(id));
			if (object == nullptr) {
				return Variant();
			}
			if (Object::cast_to<RefCounted>(object)) {
				return Ref<RefCounted>(Object::cast_to<RefCounted>(object));
			}
			return object;
		}
		case HXG_ARRAY:
			return decode_array(p_reader);
		case HXG_DICTIONARY:
			return decode_dictionary(p_reader);
		case HXG_PACKED_BYTE_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedByteArray array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				uint8_t value = 0;
				if (!p_reader.read_u8(value)) {
					return Variant();
				}
				array.set(i, value);
			}
			return array;
		}
		case HXG_PACKED_INT32_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedInt32Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				int64_t value = 0;
				if (!p_reader.read_u64((uint64_t &)value)) {
					return Variant();
				}
				array.set(i, (int32_t)value);
			}
			return array;
		}
		case HXG_PACKED_INT64_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedInt64Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				int64_t value = 0;
				if (!p_reader.read_u64((uint64_t &)value)) {
					return Variant();
				}
				array.set(i, value);
			}
			return array;
		}
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			Vector<double> values;
			values.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				if (!p_reader.read_f64(values.write[i])) {
					return Variant();
				}
			}
			if (tag == HXG_PACKED_FLOAT32_ARRAY) {
				PackedFloat32Array array;
				array.resize(count);
				for (uint32_t i = 0; i < count; i++) {
					array.set(i, (float)values[i]);
				}
				return array;
			}
			PackedFloat64Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				array.set(i, values[i]);
			}
			return array;
		}
		case HXG_PACKED_STRING_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedStringArray array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				String text;
				if (!p_reader.read_text(text)) {
					return Variant();
				}
				array.set(i, text);
			}
			return array;
		}
		case HXG_PACKED_VECTOR2_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedVector2Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				double x = 0;
				double y = 0;
				if (!p_reader.read_f64(x) || !p_reader.read_f64(y)) {
					return Variant();
				}
				array.set(i, Vector2((real_t)x, (real_t)y));
			}
			return array;
		}
		case HXG_PACKED_VECTOR3_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedVector3Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				double x = 0;
				double y = 0;
				double z = 0;
				if (!p_reader.read_f64(x) || !p_reader.read_f64(y) || !p_reader.read_f64(z)) {
					return Variant();
				}
				array.set(i, Vector3((real_t)x, (real_t)y, (real_t)z));
			}
			return array;
		}
		case HXG_PACKED_VECTOR4_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedVector4Array array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				double x = 0;
				double y = 0;
				double z = 0;
				double w = 0;
				if (!p_reader.read_f64(x) || !p_reader.read_f64(y) || !p_reader.read_f64(z) || !p_reader.read_f64(w)) {
					return Variant();
				}
				array.set(i, Vector4((real_t)x, (real_t)y, (real_t)z, (real_t)w));
			}
			return array;
		}
		case HXG_PACKED_COLOR_ARRAY: {
			uint32_t count = 0;
			if (!p_reader.read_u32(count)) {
				return Variant();
			}
			PackedColorArray array;
			array.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				double r = 0;
				double g = 0;
				double b = 0;
				double a = 0;
				if (!p_reader.read_f64(r) || !p_reader.read_f64(g) || !p_reader.read_f64(b) || !p_reader.read_f64(a)) {
					return Variant();
				}
				array.set(i, Color((float)r, (float)g, (float)b, (float)a));
			}
			return array;
		}
		case HXG_VECTOR2: {
			double x = 0;
			double y = 0;
			if (!p_reader.read_f64(x) || !p_reader.read_f64(y)) {
				return Variant();
			}
			return Vector2((real_t)x, (real_t)y);
		}
		case HXG_VECTOR3: {
			double values[3] = {};
			for (int i = 0; i < 3; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Vector3((real_t)values[0], (real_t)values[1], (real_t)values[2]);
		}
		case HXG_VECTOR4: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Vector4((real_t)values[0], (real_t)values[1], (real_t)values[2], (real_t)values[3]);
		}
		case HXG_COLOR: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Color((float)values[0], (float)values[1], (float)values[2], (float)values[3]);
		}
		case HXG_RID: {
			uint64_t id = 0;
			if (!p_reader.read_u64(id)) {
				return Variant();
			}
			return RID::from_uint64(id);
		}
		case HXG_VECTOR2I: {
			double x = 0;
			double y = 0;
			if (!p_reader.read_f64(x) || !p_reader.read_f64(y)) {
				return Variant();
			}
			return Vector2i((int)x, (int)y);
		}
		case HXG_VECTOR3I: {
			double values[3] = {};
			for (int i = 0; i < 3; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Vector3i((int)values[0], (int)values[1], (int)values[2]);
		}
		case HXG_VECTOR4I: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Vector4i((int)values[0], (int)values[1], (int)values[2], (int)values[3]);
		}
		case HXG_RECT2: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Rect2(Point2((real_t)values[0], (real_t)values[1]), Size2((real_t)values[2], (real_t)values[3]));
		}
		case HXG_RECT2I: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Rect2i(Point2i((int)values[0], (int)values[1]), Size2i((int)values[2], (int)values[3]));
		}
		case HXG_PLANE: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Plane((real_t)values[0], (real_t)values[1], (real_t)values[2], (real_t)values[3]);
		}
		case HXG_QUATERNION: {
			double values[4] = {};
			for (int i = 0; i < 4; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Quaternion((real_t)values[0], (real_t)values[1], (real_t)values[2], (real_t)values[3]);
		}
		case HXG_AABB: {
			double values[6] = {};
			for (int i = 0; i < 6; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return AABB(Vector3((real_t)values[0], (real_t)values[1], (real_t)values[2]), Vector3((real_t)values[3], (real_t)values[4], (real_t)values[5]));
		}
		case HXG_BASIS: {
			double values[9] = {};
			for (int i = 0; i < 9; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			Basis basis;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					basis.rows[row][column] = (real_t)values[row * 3 + column];
				}
			}
			return basis;
		}
		case HXG_TRANSFORM2D: {
			double values[6] = {};
			for (int i = 0; i < 6; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			return Transform2D(Vector2((real_t)values[0], (real_t)values[1]), Vector2((real_t)values[2], (real_t)values[3]), Vector2((real_t)values[4], (real_t)values[5]));
		}
		case HXG_TRANSFORM3D: {
			double values[12] = {};
			for (int i = 0; i < 12; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			Basis basis;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					basis.rows[row][column] = (real_t)values[row * 3 + column];
				}
			}
			return Transform3D(basis, Vector3((real_t)values[9], (real_t)values[10], (real_t)values[11]));
		}
		case HXG_PROJECTION: {
			double values[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!p_reader.read_f64(values[i])) {
					return Variant();
				}
			}
			Projection projection;
			for (int column = 0; column < 4; column++) {
				for (int row = 0; row < 4; row++) {
					projection.columns[column][row] = (real_t)values[column * 4 + row];
				}
			}
			return projection;
		}
		case HXG_CALLABLE: {
			uint64_t object_id = 0;
			String method;
			if (!p_reader.read_u64(object_id) || !p_reader.read_text(method)) {
				return Variant();
			}
			Object *object = object_id == 0 ? nullptr : ObjectDB::get_instance(ObjectID(object_id));
			if (object == nullptr) {
				return Callable();
			}
			return Callable(object, StringName(method));
		}
		case HXG_CALLABLE_HANDLE: {
			uint64_t handle = 0;
			if (!p_reader.read_u64(handle)) {
				return Variant();
			}
			return HaxeRuntime::get_callable((int64_t)handle);
		}
		case HXG_CLOSURE: {
			uint64_t closure = 0;
			if (!p_reader.read_u64(closure)) {
				return Variant();
			}
			return haxe_make_closure_callable(HaxeRuntime::get_active_library(), (int64_t)closure);
		}
		case HXG_SIGNAL: {
			uint64_t object_id = 0;
			String name;
			if (!p_reader.read_u64(object_id) || !p_reader.read_text(name)) {
				return Variant();
			}
			Object *object = object_id == 0 ? nullptr : ObjectDB::get_instance(ObjectID(object_id));
			if (object == nullptr) {
				return Variant();
			}
			return Signal(object, StringName(name));
		}
		default:
			return Variant();
	}
}

} // namespace

String hx_value_type_name(int p_type) {
	static const char *names[HXG_TYPE_MAX] = {
		"nil", "bool", "int", "float", "String", "StringName", "NodePath",
		"Vector2", "Vector2i", "Rect2", "Rect2i", "Vector3", "Vector3i", "Transform2D",
		"Vector4", "Vector4i", "Plane", "Quaternion", "AABB", "Basis", "Transform3D",
		"Projection", "Color", "RID", "Object", "Callable", "Signal", "Array", "Dictionary",
		"PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
		"PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array",
		"PackedColorArray", "PackedVector4Array", "CallableHandle", "Closure", "ObjectPinned"
	};
	if (p_type < 0 || p_type >= HXG_TYPE_MAX) {
		return "unknown";
	}
	return names[p_type];
}

void hx_value_encode_variant(const Variant &p_value, Vector<uint8_t> &r_blob, bool p_own_objects) {
	switch (p_value.get_type()) {
		case Variant::NIL:
			put_u8(r_blob, HXG_NIL);
			break;
		case Variant::BOOL:
			put_u8(r_blob, HXG_BOOL);
			put_u8(r_blob, p_value.operator bool() ? 1 : 0);
			break;
		case Variant::INT: {
			put_u8(r_blob, HXG_INT);
			put_u64(r_blob, (uint64_t)p_value.operator int64_t());
		} break;
		case Variant::FLOAT: {
			put_u8(r_blob, HXG_FLOAT);
			put_f64(r_blob, p_value.operator double());
		} break;
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			put_u8(r_blob, p_value.get_type() == Variant::STRING ? HXG_STRING : (p_value.get_type() == Variant::STRING_NAME ? HXG_STRING_NAME : HXG_NODE_PATH));
			put_text(r_blob, p_value.operator String());
		} break;
		case Variant::OBJECT: {
			Object *object = p_value.operator Object *();
			if (p_own_objects && object != nullptr && Object::cast_to<RefCounted>(object) != nullptr) {
				// The receiver keeps the object alive with the handed over
				// reference until its own wrapper owns it.
				HaxeRuntime::pin_object(object);
				put_u8(r_blob, HXG_OBJECT_PINNED);
			} else {
				put_u8(r_blob, HXG_OBJECT);
			}
			put_u64(r_blob, object ? (uint64_t)object->get_instance_id() : 0);
		} break;
		case Variant::CALLABLE: {
			Callable callable = p_value;
			put_u8(r_blob, HXG_CALLABLE);
			put_u64(r_blob, callable.get_object_id());
			put_text(r_blob, callable.get_method());
		} break;
		case Variant::SIGNAL: {
			Signal signal = p_value;
			put_u8(r_blob, HXG_SIGNAL);
			Object *object = signal.get_object();
			put_u64(r_blob, object ? (uint64_t)object->get_instance_id() : 0);
			put_text(r_blob, signal.get_name());
		} break;
		case Variant::ARRAY: {
			put_u8(r_blob, HXG_ARRAY);
			Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				hx_value_encode_variant(array[i], r_blob, p_own_objects);
			}
		} break;
		case Variant::DICTIONARY: {
			put_u8(r_blob, HXG_DICTIONARY);
			Dictionary dictionary = p_value;
			put_u32(r_blob, (uint32_t)dictionary.size());
			for (const KeyValue<Variant, Variant> &E : dictionary) {
				hx_value_encode_variant(E.key, r_blob, p_own_objects);
				hx_value_encode_variant(E.value, r_blob, p_own_objects);
			}
		} break;
		case Variant::PACKED_BYTE_ARRAY: {
			put_u8(r_blob, HXG_PACKED_BYTE_ARRAY);
			PackedByteArray array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_u8(r_blob, array[i]);
			}
		} break;
		case Variant::PACKED_INT32_ARRAY: {
			put_u8(r_blob, HXG_PACKED_INT32_ARRAY);
			PackedInt32Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_u64(r_blob, (uint64_t)array[i]);
			}
		} break;
		case Variant::PACKED_INT64_ARRAY: {
			put_u8(r_blob, HXG_PACKED_INT64_ARRAY);
			PackedInt64Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_u64(r_blob, (uint64_t)array[i]);
			}
		} break;
		case Variant::PACKED_FLOAT32_ARRAY: {
			put_u8(r_blob, HXG_PACKED_FLOAT32_ARRAY);
			PackedFloat32Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i]);
			}
		} break;
		case Variant::PACKED_FLOAT64_ARRAY: {
			put_u8(r_blob, HXG_PACKED_FLOAT64_ARRAY);
			PackedFloat64Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i]);
			}
		} break;
		case Variant::PACKED_STRING_ARRAY: {
			put_u8(r_blob, HXG_PACKED_STRING_ARRAY);
			PackedStringArray array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_text(r_blob, array[i]);
			}
		} break;
		case Variant::PACKED_VECTOR2_ARRAY: {
			put_u8(r_blob, HXG_PACKED_VECTOR2_ARRAY);
			PackedVector2Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i].x);
				put_f64(r_blob, array[i].y);
			}
		} break;
		case Variant::PACKED_VECTOR3_ARRAY: {
			put_u8(r_blob, HXG_PACKED_VECTOR3_ARRAY);
			PackedVector3Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i].x);
				put_f64(r_blob, array[i].y);
				put_f64(r_blob, array[i].z);
			}
		} break;
		case Variant::PACKED_VECTOR4_ARRAY: {
			put_u8(r_blob, HXG_PACKED_VECTOR4_ARRAY);
			PackedVector4Array array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i].x);
				put_f64(r_blob, array[i].y);
				put_f64(r_blob, array[i].z);
				put_f64(r_blob, array[i].w);
			}
		} break;
		case Variant::PACKED_COLOR_ARRAY: {
			put_u8(r_blob, HXG_PACKED_COLOR_ARRAY);
			PackedColorArray array = p_value;
			put_u32(r_blob, (uint32_t)array.size());
			for (int i = 0; i < array.size(); i++) {
				put_f64(r_blob, array[i].r);
				put_f64(r_blob, array[i].g);
				put_f64(r_blob, array[i].b);
				put_f64(r_blob, array[i].a);
			}
		} break;
		case Variant::VECTOR2: {
			put_u8(r_blob, HXG_VECTOR2);
			Vector2 value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
		} break;
		case Variant::VECTOR2I: {
			put_u8(r_blob, HXG_VECTOR2I);
			Vector2i value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
		} break;
		case Variant::VECTOR3: {
			put_u8(r_blob, HXG_VECTOR3);
			Vector3 value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
			put_f64(r_blob, value.z);
		} break;
		case Variant::VECTOR3I: {
			put_u8(r_blob, HXG_VECTOR3I);
			Vector3i value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
			put_f64(r_blob, value.z);
		} break;
		case Variant::VECTOR4: {
			put_u8(r_blob, HXG_VECTOR4);
			Vector4 value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
			put_f64(r_blob, value.z);
			put_f64(r_blob, value.w);
		} break;
		case Variant::VECTOR4I: {
			put_u8(r_blob, HXG_VECTOR4I);
			Vector4i value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
			put_f64(r_blob, value.z);
			put_f64(r_blob, value.w);
		} break;
		case Variant::RECT2: {
			put_u8(r_blob, HXG_RECT2);
			Rect2 value = p_value;
			put_f64(r_blob, value.position.x);
			put_f64(r_blob, value.position.y);
			put_f64(r_blob, value.size.x);
			put_f64(r_blob, value.size.y);
		} break;
		case Variant::RECT2I: {
			put_u8(r_blob, HXG_RECT2I);
			Rect2i value = p_value;
			put_f64(r_blob, value.position.x);
			put_f64(r_blob, value.position.y);
			put_f64(r_blob, value.size.x);
			put_f64(r_blob, value.size.y);
		} break;
		case Variant::PLANE: {
			put_u8(r_blob, HXG_PLANE);
			Plane value = p_value;
			put_f64(r_blob, value.normal.x);
			put_f64(r_blob, value.normal.y);
			put_f64(r_blob, value.normal.z);
			put_f64(r_blob, value.d);
		} break;
		case Variant::QUATERNION: {
			put_u8(r_blob, HXG_QUATERNION);
			Quaternion value = p_value;
			put_f64(r_blob, value.x);
			put_f64(r_blob, value.y);
			put_f64(r_blob, value.z);
			put_f64(r_blob, value.w);
		} break;
		case Variant::AABB: {
			put_u8(r_blob, HXG_AABB);
			AABB value = p_value;
			put_f64(r_blob, value.position.x);
			put_f64(r_blob, value.position.y);
			put_f64(r_blob, value.position.z);
			put_f64(r_blob, value.size.x);
			put_f64(r_blob, value.size.y);
			put_f64(r_blob, value.size.z);
		} break;
		case Variant::BASIS: {
			put_u8(r_blob, HXG_BASIS);
			Basis value = p_value;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					put_f64(r_blob, value.rows[row][column]);
				}
			}
		} break;
		case Variant::TRANSFORM2D: {
			put_u8(r_blob, HXG_TRANSFORM2D);
			Transform2D value = p_value;
			put_f64(r_blob, value.columns[0].x);
			put_f64(r_blob, value.columns[0].y);
			put_f64(r_blob, value.columns[1].x);
			put_f64(r_blob, value.columns[1].y);
			put_f64(r_blob, value.columns[2].x);
			put_f64(r_blob, value.columns[2].y);
		} break;
		case Variant::TRANSFORM3D: {
			put_u8(r_blob, HXG_TRANSFORM3D);
			Transform3D value = p_value;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					put_f64(r_blob, value.basis.rows[row][column]);
				}
			}
			put_f64(r_blob, value.origin.x);
			put_f64(r_blob, value.origin.y);
			put_f64(r_blob, value.origin.z);
		} break;
		case Variant::PROJECTION: {
			put_u8(r_blob, HXG_PROJECTION);
			Projection value = p_value;
			for (int column = 0; column < 4; column++) {
				for (int row = 0; row < 4; row++) {
					put_f64(r_blob, value.columns[column][row]);
				}
			}
		} break;
		case Variant::COLOR: {
			put_u8(r_blob, HXG_COLOR);
			Color value = p_value;
			put_f64(r_blob, value.r);
			put_f64(r_blob, value.g);
			put_f64(r_blob, value.b);
			put_f64(r_blob, value.a);
		} break;
		case Variant::RID: {
			put_u8(r_blob, HXG_RID);
			RID value = p_value;
			put_u64(r_blob, value.get_id());
		} break;
		default:
			put_u8(r_blob, HXG_NIL);
			break;
	}
}

bool hx_value_decode_variant(const uint8_t *p_data, int64_t p_size, int64_t &r_offset, Variant &r_value) {
	Reader reader(p_data + r_offset, p_size - r_offset);
	r_value = decode_value(reader);
	int64_t consumed = (p_size - r_offset) - reader.remaining();
	if (consumed <= 0) {
		return false;
	}
	r_offset += consumed;
	return true;
}

bool hx_value_to_variant(const HxValue &p_value, Variant &r_value) {
	switch (p_value.type) {
		case HXG_NIL:
			r_value = Variant();
			return true;
		case HXG_BOOL:
			r_value = p_value.i != 0;
			return true;
		case HXG_INT:
			r_value = (int64_t)p_value.i;
			return true;
		case HXG_FLOAT:
			r_value = p_value.f[0];
			return true;
		case HXG_STRING:
			r_value = String::utf8(p_value.s ? p_value.s : "");
			return true;
		case HXG_STRING_NAME:
			r_value = StringName(String::utf8(p_value.s ? p_value.s : ""));
			return true;
		case HXG_NODE_PATH:
			r_value = NodePath(String::utf8(p_value.s ? p_value.s : ""));
			return true;
		case HXG_VECTOR2:
			r_value = Vector2((real_t)p_value.f[0], (real_t)p_value.f[1]);
			return true;
		case HXG_VECTOR2I:
			r_value = Vector2i((int)p_value.f[0], (int)p_value.f[1]);
			return true;
		case HXG_VECTOR3:
			r_value = Vector3((real_t)p_value.f[0], (real_t)p_value.f[1], (real_t)p_value.f[2]);
			return true;
		case HXG_VECTOR3I:
			r_value = Vector3i((int)p_value.f[0], (int)p_value.f[1], (int)p_value.f[2]);
			return true;
		case HXG_VECTOR4:
			r_value = Vector4((real_t)p_value.f[0], (real_t)p_value.f[1], (real_t)p_value.f[2], (real_t)p_value.f[3]);
			return true;
		case HXG_VECTOR4I:
			r_value = Vector4i((int)p_value.f[0], (int)p_value.f[1], (int)p_value.f[2], (int)p_value.f[3]);
			return true;
		case HXG_RECT2:
			r_value = Rect2(Point2((real_t)p_value.f[0], (real_t)p_value.f[1]), Size2((real_t)p_value.f[2], (real_t)p_value.f[3]));
			return true;
		case HXG_RECT2I:
			r_value = Rect2i(Point2i((int)p_value.f[0], (int)p_value.f[1]), Size2i((int)p_value.f[2], (int)p_value.f[3]));
			return true;
		case HXG_PLANE:
			r_value = Plane((real_t)p_value.f[0], (real_t)p_value.f[1], (real_t)p_value.f[2], (real_t)p_value.f[3]);
			return true;
		case HXG_QUATERNION:
			r_value = Quaternion((real_t)p_value.f[0], (real_t)p_value.f[1], (real_t)p_value.f[2], (real_t)p_value.f[3]);
			return true;
		case HXG_COLOR:
			r_value = Color((float)p_value.f[0], (float)p_value.f[1], (float)p_value.f[2], (float)p_value.f[3]);
			return true;
		case HXG_RID:
			r_value = RID::from_uint64((uint64_t)p_value.i);
			return true;
		case HXG_OBJECT:
		case HXG_OBJECT_PINNED: {
			Object *object = p_value.i == 0 ? nullptr : ObjectDB::get_instance(ObjectID((uint64_t)p_value.i));
			if (object == nullptr) {
				r_value = Variant();
				return p_value.i == 0;
			}
			RefCounted *ref_counted = Object::cast_to<RefCounted>(object);
			if (ref_counted != nullptr) {
				r_value = Ref<RefCounted>(ref_counted);
			} else {
				r_value = object;
			}
			return true;
		}
		case HXG_SIGNAL: {
			Object *object = p_value.i == 0 ? nullptr : ObjectDB::get_instance(ObjectID((uint64_t)p_value.i));
			if (object == nullptr) {
				r_value = Variant();
				return false;
			}
			r_value = Signal(object, StringName(String::utf8(p_value.s ? p_value.s : "")));
			return true;
		}
		case HXG_CALLABLE_HANDLE: {
			r_value = HaxeRuntime::get_callable(p_value.i);
			return true;
		}
		case HXG_CLOSURE: {
			r_value = haxe_make_closure_callable(HaxeRuntime::get_active_library(), p_value.i);
			return true;
		}
		case HXG_CALLABLE: {
			Object *object = p_value.i == 0 ? nullptr : ObjectDB::get_instance(ObjectID((uint64_t)p_value.i));
			if (object == nullptr || p_value.s == nullptr) {
				r_value = Callable();
				return object == nullptr && p_value.i == 0;
			}
			r_value = Callable(object, StringName(String::utf8(p_value.s)));
			return true;
		}
		case HXG_ARRAY:
		case HXG_DICTIONARY:
		case HXG_PACKED_BYTE_ARRAY:
		case HXG_PACKED_INT32_ARRAY:
		case HXG_PACKED_INT64_ARRAY:
		case HXG_PACKED_FLOAT32_ARRAY:
		case HXG_PACKED_FLOAT64_ARRAY:
		case HXG_PACKED_STRING_ARRAY:
		case HXG_PACKED_VECTOR2_ARRAY:
		case HXG_PACKED_VECTOR3_ARRAY:
		case HXG_PACKED_VECTOR4_ARRAY:
		case HXG_PACKED_COLOR_ARRAY:
		case HXG_AABB:
		case HXG_BASIS:
		case HXG_TRANSFORM2D:
		case HXG_TRANSFORM3D:
		case HXG_PROJECTION: {
			if (p_value.blob == nullptr || p_value.blob_size <= 0) {
				r_value = Variant();
				return false;
			}
			int64_t offset = 0;
			return hx_value_decode_variant(p_value.blob, p_value.blob_size, offset, r_value);
		}
		default:
			r_value = Variant();
			return false;
	}
}

void hx_value_from_variant(const Variant &p_value, HxValueHolder &r_holder, bool p_own_objects) {
	r_holder.blob.clear();
	r_holder.text = CharString();
	r_holder.value = HxValue();
	r_holder.value.type = HXG_NIL;

	switch (p_value.get_type()) {
		case Variant::NIL:
			r_holder.value.type = HXG_NIL;
			break;
		case Variant::BOOL:
			r_holder.value.type = HXG_BOOL;
			r_holder.value.i = p_value.operator bool() ? 1 : 0;
			break;
		case Variant::INT:
			r_holder.value.type = HXG_INT;
			r_holder.value.i = p_value.operator int64_t();
			break;
		case Variant::FLOAT:
			r_holder.value.type = HXG_FLOAT;
			r_holder.value.f[0] = p_value.operator double();
			break;
		case Variant::STRING: {
			r_holder.text = p_value.operator String().utf8();
			r_holder.value.type = HXG_STRING;
			r_holder.value.s = r_holder.text.get_data();
		} break;
		case Variant::STRING_NAME: {
			r_holder.text = p_value.operator StringName().operator String().utf8();
			r_holder.value.type = HXG_STRING_NAME;
			r_holder.value.s = r_holder.text.get_data();
		} break;
		case Variant::NODE_PATH: {
			r_holder.text = p_value.operator NodePath().operator String().utf8();
			r_holder.value.type = HXG_NODE_PATH;
			r_holder.value.s = r_holder.text.get_data();
		} break;
		case Variant::OBJECT: {
			Object *object = p_value.operator Object *();
			r_holder.value.type = HXG_OBJECT;
			r_holder.value.i = object ? (int64_t)object->get_instance_id() : 0;
			if (p_own_objects && object != nullptr && Object::cast_to<RefCounted>(object) != nullptr) {
				// A returned resource may be kept alive by the engine's
				// Variant alone, which dies as soon as this callback returns.
				// The library needs the object until its wrapper owns it, so
				// one reference travels with the value.
				HaxeRuntime::pin_object(object);
				r_holder.value.owned = 1;
			}
		} break;
		case Variant::CALLABLE: {
			Callable callable = p_value;
			if (callable.is_custom()) {
				// Custom callables (binds, Haxe closures, lambdas) cannot be
				// described as object + method, so the runtime hands out a
				// handle that the engine can invoke later.
				r_holder.value.type = HXG_CALLABLE_HANDLE;
				r_holder.value.i = HaxeRuntime::store_callable(callable);
			} else if (callable.is_valid()) {
				r_holder.value.type = HXG_CALLABLE;
				r_holder.value.i = (int64_t)callable.get_object_id();
				r_holder.text = callable.get_method().operator String().utf8();
				r_holder.value.s = r_holder.text.get_data();
			} else {
				r_holder.value.type = HXG_CALLABLE;
				r_holder.value.i = 0;
			}
		} break;
		case Variant::SIGNAL: {
			Signal signal = p_value;
			Object *object = signal.get_object();
			r_holder.value.type = HXG_SIGNAL;
			r_holder.value.i = object ? (int64_t)object->get_instance_id() : 0;
			r_holder.text = signal.get_name().operator String().utf8();
			r_holder.value.s = r_holder.text.get_data();
		} break;
		case Variant::VECTOR2: {
			Vector2 value = p_value;
			r_holder.value.type = HXG_VECTOR2;
			r_holder.value.f[0] = value.x;
			r_holder.value.f[1] = value.y;
		} break;
		case Variant::VECTOR3: {
			Vector3 value = p_value;
			r_holder.value.type = HXG_VECTOR3;
			r_holder.value.f[0] = value.x;
			r_holder.value.f[1] = value.y;
			r_holder.value.f[2] = value.z;
		} break;
		case Variant::VECTOR4: {
			Vector4 value = p_value;
			r_holder.value.type = HXG_VECTOR4;
			r_holder.value.f[0] = value.x;
			r_holder.value.f[1] = value.y;
			r_holder.value.f[2] = value.z;
			r_holder.value.f[3] = value.w;
		} break;
		case Variant::COLOR: {
			Color value = p_value;
			r_holder.value.type = HXG_COLOR;
			r_holder.value.f[0] = value.r;
			r_holder.value.f[1] = value.g;
			r_holder.value.f[2] = value.b;
			r_holder.value.f[3] = value.a;
		} break;
		case Variant::RID: {
			RID value = p_value;
			r_holder.value.type = HXG_RID;
			r_holder.value.i = (int64_t)value.get_id();
		} break;
		default:
			// Composite and remaining built-in types travel as an encoded blob.
			// The blob always contains the complete tagged encoding so both
			// sides can share one decoder, including nested containers.
			hx_value_encode_variant(p_value, r_holder.blob, p_own_objects);
			if (r_holder.blob.size() > 0) {
				r_holder.value.type = r_holder.blob[0];
			}
			r_holder.value.blob = r_holder.blob.size() ? r_holder.blob.ptr() : nullptr;
			r_holder.value.blob_size = r_holder.blob.size();
			break;
	}
}
