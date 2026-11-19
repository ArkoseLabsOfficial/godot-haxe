package hxgodot;

/**
 * Object that stores value types (`Vector2`, `Transform2D`, `Color`, ...).
 *
 * The engine passes value types by copy, so a component written through a
 * temporary would be lost: `node.position.x = 5` would change a copy of the
 * position and the node would keep its old value. Property getters in the
 * generated bindings therefore return a copy of the value bound to the object
 * it came from. Writing any component calls `__hxwrite`, which stores the
 * whole value back into the property.
 *
 * `godot.Object` implements this interface, so every engine object can own a
 * bound value. Value types implement it too, which is what makes nested
 * writes such as `node.transform.origin.x = 5` work: each level stores the
 * changed value into the level above and ends at the object.
 *
 * This is an implementation detail of the generated bindings. User code
 * should only rely on the resulting behaviour, which matches GDScript's:
 * `position.x = 5` updates the node.
 */
interface ValueOwner {
	/** Stores `value` in this object's property `property`. */
	public function __hxwrite(property:String, value:Dynamic):Void;
}