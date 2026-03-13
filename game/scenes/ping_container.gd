extends VFlowContainer

@export var network_manager: NetworkManager

@onready var ping_label: Label = $PingLabel

func _ready() -> void:
	if not network_manager:
		printerr("ERREUR (PingContainer) : Le noeud NetworkManager n'est pas assigné dans l'inspecteur.")
		set_process(false)
		return
		
	if not ping_label:
		printerr("ERREUR (PingContainer) : PingLabel introuvable.")
		set_process(false)
		return


func _process(_delta: float) -> void:
	# L'appel à get_current_rtt() est extrêmement peu coûteux (lecture d'un entier en mémoire C++)
	# On peut le faire tourner à chaque frame visuelle sans impacter les performances.
	var rtt: int = network_manager.get_current_rtt()
	ping_label.text = "Ping: %d ms" % rtt
