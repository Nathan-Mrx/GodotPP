extends VFlowContainer

@export var network_manager: NetworkManager

@onready var p_drop_slider: Range = $PDropSlider
@onready var p_drop_label: Label  = $PDropLabel

func _ready() -> void:
	if not network_manager:
		printerr("ERREUR (PacketDropContainer) : NetworkManager non assigné.")
		return
	if not p_drop_slider or not p_drop_label:
		printerr("ERREUR (PacketDropContainer) : PDropSlider ou PDropLabel introuvable.")
		return

	_setup_slider(p_drop_slider, p_drop_label,
		0.0, 1.0, 0.01,
		network_manager.get_simulated_packet_drop_chance(),
		func(v: float) -> void: network_manager.set_simulated_packet_drop_chance(v),
		func(v: float) -> String: return "Packet Drop: %d%%" % int(v * 100)
	)

	_add_row("K (ressort)", 0.0, 2000.0, 1.0,
		network_manager.get_correction_K(),
		func(v: float) -> void: network_manager.set_correction_K(v),
		func(v: float) -> String: return "K: %.0f" % v
	)

	_add_row("W (amortissement)", 0.0, 100.0, 0.1,
		network_manager.get_correction_W(),
		func(v: float) -> void: network_manager.set_correction_W(v),
		func(v: float) -> String: return "W: %.1f" % v
	)

	_add_row("Erreur simulée (px)", 0.0, 500.0, 5.0,
		network_manager.get_simulated_error_px(),
		func(v: float) -> void: network_manager.set_simulated_error_px(v),
		func(v: float) -> String: return "Erreur: %.0f px" % v
	)


func _setup_slider(slider: Range, label: Label,
		min_val: float, max_val: float, step: float,
		initial: float, setter: Callable, formatter: Callable) -> void:
	slider.focus_mode = Control.FOCUS_NONE
	slider.min_value  = min_val
	slider.max_value  = max_val
	slider.step       = step
	slider.value      = initial
	label.text        = formatter.call(initial)
	slider.value_changed.connect(func(v: float) -> void:
		setter.call(v)
		label.text = formatter.call(v)
	)


func _add_row(name: String, min_val: float, max_val: float, step: float,
		initial: float, setter: Callable, formatter: Callable) -> void:
	var lbl    := Label.new()
	var slider := HSlider.new()

	lbl.text = formatter.call(initial)

	slider.focus_mode              = Control.FOCUS_NONE
	slider.min_value               = min_val
	slider.max_value               = max_val
	slider.step                    = step
	slider.value                   = initial
	slider.size_flags_horizontal   = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size.x   = p_drop_slider.custom_minimum_size.x

	slider.value_changed.connect(func(v: float) -> void:
		setter.call(v)
		lbl.text = formatter.call(v)
	)

	add_child(lbl)
	add_child(slider)
