@tool
extends EditorScript

## Run via: Editor → File → Run Script
## Walks the scene tree and collects collision shapes from:
##   - StaticBody2D  (CollisionShape2D and CollisionPolygon2D children)
##   - TileMapLayer  (physics layer polygons per tile)
##
## Supported export shapes:
##   RectangleShape2D  → "rect",   param_a = half_extents.x, param_b = half_extents.y
##   CircleShape2D     → "circle", param_a = radius,         param_b = 0
##   All polygon types → "rect"   (axis-aligned bounding box)

func _run() -> void:
	var scene_root: Node = get_scene()
	if scene_root == null:
		push_error("export_level.gd: No scene is open. Open a scene first.")
		return

	var objects: Array = []
	_collect_collisions(scene_root, objects)

	var json_text: String = JSON.stringify({"objects": objects}, "\t")
	var path: String = "res://level.json"
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("export_level.gd: Cannot write to %s" % path)
		return
	file.store_string(json_text)
	file.close()
	print("export_level.gd: Wrote %d objects to %s" % [objects.size(), path])


func _collect_collisions(node: Node, objects: Array) -> void:
	if node is StaticBody2D:
		_export_static_body(node, objects)
	elif node is TileMapLayer:
		_export_tile_map_layer(node, objects)
	for child in node.get_children():
		_collect_collisions(child, objects)


# ─── StaticBody2D ──────────────────────────────────────────────────────────────

func _export_static_body(body: StaticBody2D, objects: Array) -> void:
	for child in body.get_children():
		if child is CollisionShape2D:
			_export_collision_shape(child, objects)
		elif child is CollisionPolygon2D:
			_export_collision_polygon(child, objects)


func _export_collision_shape(cs: CollisionShape2D, objects: Array) -> void:
	var shape = cs.shape
	if shape == null:
		return

	# Use cs.global_position / cs.global_rotation, NOT the parent StaticBody2D,
	# so that any local offset on the CollisionShape2D is correctly reflected.
	var pos: Vector2 = cs.global_position
	var rot: float   = cs.global_rotation

	if shape is RectangleShape2D:
		var half: Vector2 = shape.size * 0.5
		objects.append({
			"shape":    "rect",
			"x":        snappedf(pos.x,  0.001),
			"y":        snappedf(pos.y,  0.001),
			"rotation": snappedf(rot,    0.001),
			"param_a":  snappedf(half.x, 0.001),
			"param_b":  snappedf(half.y, 0.001),
		})
	elif shape is CircleShape2D:
		objects.append({
			"shape":    "circle",
			"x":        snappedf(pos.x,        0.001),
			"y":        snappedf(pos.y,        0.001),
			"rotation": snappedf(rot,          0.001),
			"param_a":  snappedf(shape.radius, 0.001),
			"param_b":  0.0,
		})
	elif shape is ConvexPolygonShape2D:
		var world_pts := _transform_points(shape.points, cs.global_transform)
		_append_aabb_rect(objects, _points_aabb(world_pts))
	elif shape is ConcavePolygonShape2D:
		# segments is a flat array of segment endpoints [a0, b0, a1, b1, ...]
		var world_pts := _transform_points(shape.segments, cs.global_transform)
		_append_aabb_rect(objects, _points_aabb(world_pts))
	else:
		push_warning("export_level.gd: Unsupported shape %s on %s - skipped." % [shape.get_class(), cs.get_path()])


func _export_collision_polygon(cp: CollisionPolygon2D, objects: Array) -> void:
	if cp.polygon.is_empty():
		return
	var world_pts := _transform_points(cp.polygon, cp.global_transform)
	_append_aabb_rect(objects, _points_aabb(world_pts))


# ─── TileMapLayer ──────────────────────────────────────────────────────────────

func _export_tile_map_layer(layer: TileMapLayer, objects: Array) -> void:
	var tile_set = layer.tile_set
	if tile_set == null:
		return
	var physics_layers: int = tile_set.get_physics_layers_count()
	if physics_layers == 0:
		return

	for coords in layer.get_used_cells():
		var tile_data = layer.get_cell_tile_data(coords)
		if tile_data == null:
			continue

		# map_to_local returns the cell center in the layer's local space.
		var cell_local: Vector2 = layer.map_to_local(coords)

		for pl in range(physics_layers):
			for pi in range(tile_data.get_collision_polygons_count(pl)):
				var local_pts: PackedVector2Array = tile_data.get_collision_polygon_points(pl, pi)
				if local_pts.is_empty():
					continue
				# Polygon points are relative to the tile center in the layer's local space.
				var world_pts := PackedVector2Array()
				for p in local_pts:
					world_pts.append(layer.to_global(cell_local + p))
				_append_aabb_rect(objects, _points_aabb(world_pts))


# ─── Helpers ───────────────────────────────────────────────────────────────────

func _transform_points(pts: PackedVector2Array, xform: Transform2D) -> PackedVector2Array:
	var out := PackedVector2Array()
	for p in pts:
		out.append(xform * p)
	return out


func _points_aabb(pts: PackedVector2Array) -> Rect2:
	var lo: Vector2 = pts[0]
	var hi: Vector2 = pts[0]
	for p in pts:
		lo = lo.min(p)
		hi = hi.max(p)
	return Rect2(lo, hi - lo)


func _append_aabb_rect(objects: Array, aabb: Rect2) -> void:
	var cx: float = aabb.position.x + aabb.size.x * 0.5
	var cy: float = aabb.position.y + aabb.size.y * 0.5
	objects.append({
		"shape":    "rect",
		"x":        snappedf(cx,                0.001),
		"y":        snappedf(cy,                0.001),
		"rotation": 0.0,
		"param_a":  snappedf(aabb.size.x * 0.5, 0.001),
		"param_b":  snappedf(aabb.size.y * 0.5, 0.001),
	})
