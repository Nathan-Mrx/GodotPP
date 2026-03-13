extends VFlowContainer

@export var network_manager: NetworkManager

@onready var p_drop_slider: Range = $PDropSlider
@onready var p_drop_label: Label = $PDropLabel

func _ready() -> void:
	if not network_manager:
		printerr("ERREUR (PacketDropContainer) : Le noeud NetworkManager n'est pas assigné dans l'inspecteur.")
		return
		
	if not p_drop_slider or not p_drop_label:
		printerr("ERREUR (PacketDropContainer) : PDropSlider ou PDropLabel introuvable.")
		return

	# Empêche le slider de capturer les touches directionnelles du clavier
	p_drop_slider.focus_mode = Control.FOCUS_NONE

	p_drop_slider.min_value = 0.0
	p_drop_slider.max_value = 1.0
	p_drop_slider.step = 0.01
	
	var current_drop: float = network_manager.get_simulated_packet_drop_chance()
	p_drop_slider.value = current_drop
	_update_label(current_drop)
	
	if not p_drop_slider.value_changed.is_connected(_on_slider_value_changed):
		p_drop_slider.value_changed.connect(_on_slider_value_changed)


func _on_slider_value_changed(new_value: float) -> void:
	network_manager.set_simulated_packet_drop_chance(new_value)
	_update_label(new_value)


func _update_label(val: float) -> void:
	p_drop_label.text = "Packet Drop: %d%%" % int(val * 100)
