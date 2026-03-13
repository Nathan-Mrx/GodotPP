#include "net_manager.h"

#include <random>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/engine.hpp>

#include "godot_cpp/classes/viewport.hpp"

godot::NetworkManager::NetworkManager() {
    socket = nullptr;
}

godot::NetworkManager::~NetworkManager() {}

void godot::NetworkManager::_ready()
{
    Node::_ready();

    if (Engine::get_singleton()->is_editor_hint()) {
        set_physics_process(false);
        set_process(false);
        return;
    }

    set_physics_process(true);

    // IMPORTANT : On réactive la boucle visuelle (Uncapped FPS) pour le lerp
    set_process(true);

    socket = net_socket_create("127.0.0.1:0");

    if (socket) {
        HelloPacket packet;
        packet.type = PacketType::HELLO;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(-512, 512);

        packet.x = distrib(gen);
        packet.y = distrib(gen) / 2;

        UtilityFunctions::print("[CLIENT] Send hello at: ", packet.x, ", ", packet.y);
        net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(HelloPacket));
    } else {
        UtilityFunctions::print("[CLIENT] Socket could not be created");
    }

    linking_context = LinkingContext();
    linking_context.register_type(1, []() -> Node*
    {
        Ref<PackedScene> player_scene = ResourceLoader::get_singleton()->load("res://player.tscn");
        return player_scene->instantiate();
    });
}

void godot::NetworkManager::_process(double delta)
{
    if (Engine::get_singleton()->is_editor_hint()) return;

    uint64_t now_ms = Time::get_singleton()->get_ticks_msec();
    // On dessine le monde tel qu'il était il y a 100ms
    uint64_t render_time = now_ms - interpolation_delay_ms;

    for (auto& pair : interpolation_buffers) {
        NetID net_id = pair.first;
        auto& buffer = pair.second;

        // S'il n'y a pas assez de données pour interpoler, on skip
        if (buffer.size() < 2) continue;

        Node* node = linking_context.get_node(net_id);
        if (!node) continue;
        Node2D* node_2d = dynamic_cast<Node2D*>(node);
        if (!node_2d) continue;

        // 1. Purge des vieux snapshots inutiles (On garde seulement ceux qui encadrent notre render_time)
        while (buffer.size() > 2 && buffer[1].timestamp < render_time) {
            buffer.erase(buffer.begin()); // Note: Sur un std::vector c'est O(N), acceptable ici vu la taille (< 10)
        }

        TransformSnapshot& from = buffer[0];
        TransformSnapshot& to = buffer[1];

        // 2. Calcul du lerp
        if (render_time >= from.timestamp && render_time <= to.timestamp) {
            float t = (float)(render_time - from.timestamp) / (float)(to.timestamp - from.timestamp);
            node_2d->set_position(from.position.lerp(to.position, t));
        }
        else if (render_time > to.timestamp) {
            // Extrapolation : Le réseau lag et on manque de snapshots futurs. On bloque sur la dernière position connue.
            // TODO: Ajouter de la Dead-Reckoning (prédiction de trajectoire avec la vélocité) au lieu de bloquer net.
            node_2d->set_position(to.position);
        }
    }
}

void godot::NetworkManager::_physics_process(double delta)
{
    if (Engine::get_singleton()->is_editor_hint()) return;

    uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

    int32_t bytes_read;
    while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
    {
        PacketType packet_type = (PacketType)read_buffer[0];

        if (packet_type == PacketType::SPAWN)
        {
            SpawnPacket* packet = reinterpret_cast<SpawnPacket*>(read_buffer);
            if (bytes_read >= sizeof(SpawnPacket))
            {
                Node* spawned_node = linking_context.spawn_network_object(packet->netID, packet->typeID);
                if (spawned_node)
                {
                    add_child(spawned_node);
                    Node2D* spawned_node_2d = dynamic_cast<Node2D*>(spawned_node);
                    if (spawned_node_2d != nullptr) {
                        spawned_node_2d->set_position(Vector2(packet->x, packet->y));
                        UtilityFunctions::print("[CLIENT] Spawned ID: ", packet->netID, " at: ", packet->x, ", ", packet->y);

                        // Initialisation du buffer pour éviter un "jump" au premier UPDATE
                        auto& buffer = interpolation_buffers[packet->netID];
                        buffer.push_back({now_ms, Vector2(packet->x, packet->y)});
                    }
                }
            }
        }
        else if (packet_type == PacketType::UPDATE)
        {
            UpdatePacket* packet = reinterpret_cast<UpdatePacket*>(read_buffer);
            if (bytes_read >= sizeof(UpdatePacket))
            {
                auto& buffer = interpolation_buffers[packet->netID];
                buffer.push_back({now_ms, Vector2(packet->x, packet->y)});

                // Sécurité mémoire : On empêche le buffer de grossir à l'infini en cas de gel du renderer
                if (buffer.size() > 20) {
                    buffer.erase(buffer.begin());
                }
            }
        }
        else if (packet_type == PacketType::DESPAWN)
        {
            DespawnPacket* packet = reinterpret_cast<DespawnPacket*>(read_buffer);
            if (bytes_read >= sizeof(DespawnPacket))
            {
                linking_context.despawn_network_object(packet->netID);

                // On n'oublie pas de nettoyer la RAM
                interpolation_buffers.erase(packet->netID);
            }
        }
        else if (packet_type == PacketType::PING_RESPONSE)
        {
            PingResponsePacket* packet = reinterpret_cast<PingResponsePacket*>(read_buffer);
            if (bytes_read >= sizeof(PingResponsePacket))
            {
                uint64_t t_receive = Time::get_singleton()->get_ticks_msec();
                current_rtt = t_receive - packet->t0;
            }
        }
    }

    // --- ENVOI DES INPUTS ---
    Input* input = Input::get_singleton();
    if (!input) return;

    static const StringName ui_up("ui_up");
    static const StringName ui_down("ui_down");
    static const StringName ui_left("ui_left");
    static const StringName ui_right("ui_right");
    static const StringName ui_accept("ui_accept");

    InputState current_state = {};
    current_state.keys = InputFlags::NONE;

    if (input->is_action_pressed(ui_up))     current_state.keys |= InputFlags::UP;
    if (input->is_action_pressed(ui_down))   current_state.keys |= InputFlags::DOWN;
    if (input->is_action_pressed(ui_left))   current_state.keys |= InputFlags::LEFT;
    if (input->is_action_pressed(ui_right))  current_state.keys |= InputFlags::RIGHT;
    if (input->is_action_pressed(ui_accept)) current_state.keys |= InputFlags::ACTION;

    Viewport* viewport = get_viewport();
    if (viewport) {
        Vector2 mouse_pos = viewport->get_mouse_position();
        current_state.aim_x = mouse_pos.x;
        current_state.aim_y = mouse_pos.y;
    } else {
        current_state.aim_x = 0.0f;
        current_state.aim_y = 0.0f;
    }

    input_history[input_sequence % 20] = current_state;

    InputPacket packet;
    packet.type = PacketType::INPUT;
    packet.sequence = input_sequence;

    for (int i = 0; i < 20; ++i) {
        if (input_sequence >= static_cast<uint32_t>(i)) {
            packet.history[i] = input_history[(input_sequence - i) % 20];
        } else {
            packet.history[i] = {};
        }
    }

    if (socket) {
        bool drop_input = (simulated_packet_drop_chance > 0.0) && (UtilityFunctions::randf() < simulated_packet_drop_chance);

        if (!drop_input) {
            net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(InputPacket));
        }
    }

    input_sequence++;

    // --- PING SYNC ---
    if (now_ms - last_ping_send_time >= 1000)
    {
        PingRequestPacket ping_pkt;
        ping_pkt.type = PacketType::PING_REQUEST;
        ping_pkt.id = ping_id++;
        ping_pkt.t0 = now_ms;

        if (socket) {
            bool drop_ping = (simulated_packet_drop_chance > 0.0) && (UtilityFunctions::randf() < simulated_packet_drop_chance);

            if (!drop_ping) {
                net_socket_send(socket, server_address, (uint8_t*)&ping_pkt, sizeof(PingRequestPacket));
            }
        }

        last_ping_send_time = now_ms;
    }
}

void godot::NetworkManager::_exit_tree()
{
    if (Engine::get_singleton()->is_editor_hint()) {
        return;
    }

    if (socket) {
        DisconnectPacket packet;
        packet.type = PacketType::DISCONNECT;
        net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(DisconnectPacket));
        UtilityFunctions::print("[CLIENT] Sent disconnect packet");
    }
}

void godot::NetworkManager::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("set_simulated_packet_drop_chance", "chance"), &NetworkManager::set_simulated_packet_drop_chance);
    ClassDB::bind_method(D_METHOD("get_simulated_packet_drop_chance"), &NetworkManager::get_simulated_packet_drop_chance);

    ClassDB::bind_method(D_METHOD("get_current_rtt"), &NetworkManager::get_current_rtt);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_packet_drop_chance", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_simulated_packet_drop_chance", "get_simulated_packet_drop_chance");
}

uint32_t godot::NetworkManager::get_current_rtt() const
{
    return current_rtt;
}

void godot::NetworkManager::set_simulated_packet_drop_chance(double p_chance)
{
    simulated_packet_drop_chance = p_chance;
}

double godot::NetworkManager::get_simulated_packet_drop_chance() const
{
    return simulated_packet_drop_chance;
}
