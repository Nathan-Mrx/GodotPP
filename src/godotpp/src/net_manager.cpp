/**
 * @file net_manager.cpp
 * @brief Client-side NetworkManager implementation - UDP I/O, interpolation, input.
 *
 * Orchestrates the full client networking lifecycle:
 *   1. `_ready()`           - bind socket, send HELLO, register entity factories.
 *   2. `_physics_process()` - poll packets (SPAWN / UPDATE / DESPAWN / PING_RESPONSE),
 *                             send InputPacket + PingRequestPacket.
 *   3. `_process()`         - interpolate entity positions for smooth rendering.
 *   4. `_exit_tree()`       - send DISCONNECT before leaving the scene tree.
 */

#include "net_manager.h"

#include <random>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/engine.hpp>

#include "godot_cpp/classes/viewport.hpp"

// ─────────────────────────────────────────────────────────
//  Construction / Destruction
// ─────────────────────────────────────────────────────────

/** @brief Default constructor - socket starts as null. */
godot::NetworkManager::NetworkManager() {
    socket = nullptr;
}

/** @brief Default destructor. */
godot::NetworkManager::~NetworkManager() {}

// ─────────────────────────────────────────────────────────
//  _ready  -  Socket creation, HELLO, type registration
// ─────────────────────────────────────────────────────────

/**
 * @brief Opens a UDP socket, sends HELLO with a random spawn position,
 *        and registers the player PackedScene factory.
 *
 * Skipped entirely when running inside the Godot editor.
 */
void godot::NetworkManager::_ready()
{
    Node::_ready();

    if (Engine::get_singleton()->is_editor_hint()) {
        set_physics_process(false);
        set_process(false);
        UtilityFunctions::print("[CLIENT][NetworkManager] Running in editor - networking disabled");
        return;
    }

    set_physics_process(true);

    // Enable the visual loop (uncapped FPS) for smooth interpolation
    set_process(true);

    UtilityFunctions::print("[CLIENT][NetworkManager] Creating UDP socket on 127.0.0.1:0 (ephemeral port)...");
    socket = net_socket_create("127.0.0.1:0");

    if (socket) {
        UtilityFunctions::print("[CLIENT][NetworkManager] Socket created successfully");

        HelloPacket packet;
        packet.type = PacketType::HELLO;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(-512, 512);

        packet.x = distrib(gen);
        packet.y = distrib(gen) / 2;

        UtilityFunctions::print("[CLIENT][NetworkManager] Sending HELLO to server ", server_address,
                                " - requested spawn position (", packet.x, ", ", packet.y, ")");
        net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(HelloPacket));
    } else {
        UtilityFunctions::print("[CLIENT][NetworkManager] ERROR: Socket creation failed - networking is unavailable");
    }

    linking_context = LinkingContext();
    linking_context.register_type(1, []() -> Node*
    {
        Ref<PackedScene> player_scene = ResourceLoader::get_singleton()->load("res://player.tscn");
        return player_scene->instantiate();
    });

    UtilityFunctions::print("[CLIENT][NetworkManager] Initialization complete - waiting for server response");
}

// ─────────────────────────────────────────────────────────
//  _process  -  Client-side entity interpolation
// ─────────────────────────────────────────────────────────

/**
 * @brief Interpolates (or extrapolates) entity positions using the snapshot buffer.
 *
 * Renders the world as it was `interpolation_delay_ms` milliseconds ago.
 * This absorbs network jitter and packet loss while keeping motion smooth.
 *
 * - **Interpolation**: If `render_time` falls between two snapshots, lerp.
 * - **Extrapolation**: If the buffer is stale, hold the last known position.
 */
void godot::NetworkManager::_process(double delta)
{
    if (Engine::get_singleton()->is_editor_hint()) return;

    for (auto& pair : interpolation_states) {
        NetID net_id = pair.first;

        // The local player must reflect the exact server state.
        if (net_id == local_player_net_id) {
            continue;
        }

        auto& state = pair.second;
        auto& buffer = state.snapshots;

        Node* node = linking_context.get_node(net_id);
        if (!node) continue;
        Node2D* node_2d = dynamic_cast<Node2D*>(node);
        if (!node_2d) continue;

        // 1. Buffering Phase: Wait strictly for 4 frames to guarantee 3 frames of delay
        if (state.buffering) {
            if (buffer.size() >= 4) {
                state.buffering = false;
            } else {
                if (!buffer.empty()) {
                    node_2d->set_position(buffer.back().position);
                }
                continue;
            }
        }

        // 2. Playback Phase
        if (buffer.size() >= 2) {
            TransformSnapshot& from = buffer[0];
            TransformSnapshot& to = buffer[1];

            double duration_sec = static_cast<double>(to.timestamp - from.timestamp) / 1000.0;
            if (duration_sec <= 0.001) duration_sec = 0.016;

            double speed_multiplier = 1.0;
            if (buffer.size() > 4) speed_multiplier = 1.1;
            if (buffer.size() > 6) speed_multiplier = 1.5;

            state.playback_t += (delta / duration_sec) * speed_multiplier;

            while (state.playback_t >= 1.0 && buffer.size() >= 2) {
                state.playback_t -= 1.0;
                buffer.pop_front();

                if (buffer.size() < 2) {
                    state.buffering = true;
                    state.playback_t = 0.0;
                    break;
                }
            }

            if (!state.buffering && buffer.size() >= 2) {
                float t_float = static_cast<float>(state.playback_t);
                node_2d->set_position(buffer[0].position.lerp(buffer[1].position, t_float));
            }
        } else {
            state.buffering = true;
            state.playback_t = 0.0;
        }
    }
}

// ─────────────────────────────────────────────────────────
//  _physics_process  -  Packet polling, input sending, ping
// ─────────────────────────────────────────────────────────

/**
 * @brief Main networking tick - polls all pending packets, sends input + ping.
 *
 * ### Inbound packets handled:
 * | Type           | Action                                               |
 * |----------------|------------------------------------------------------|
 * | SPAWN          | Instantiate node via LinkingContext, seed interp buf. |
 * | UPDATE         | Append snapshot to the interpolation buffer.          |
 * | DESPAWN        | Destroy node and clean up interpolation buffer.       |
 * | PING_RESPONSE  | Compute RTT from echoed t0.                          |
 *
 * ### Outbound packets sent:
 * - **InputPacket** (every tick): current + 19 historical input frames.
 * - **PingRequestPacket** (every ~1 s): RTT measurement probe.
 */
void godot::NetworkManager::_physics_process(double delta)
{
    if (Engine::get_singleton()->is_editor_hint()) return;

    uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

    // --- RECEIVE LOOP ---
    int32_t bytes_read;
    while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
    {
        PacketType packet_type = (PacketType)read_buffer[0];

        if (packet_type == PacketType::SPAWN)
        {
            SpawnPacket* packet = reinterpret_cast<SpawnPacket*>(read_buffer);
            if (bytes_read >= sizeof(SpawnPacket))
            {
                if (local_player_net_id == 0) {
                    local_player_net_id = packet->netID;
                    UtilityFunctions::print("[CLIENT][Network] Local player assigned NetID: ", local_player_net_id);
                }

                Node* spawned_node = linking_context.spawn_network_object(packet->netID, packet->typeID);
                if (spawned_node)
                {
                    add_child(spawned_node);
                    Node2D* spawned_node_2d = dynamic_cast<Node2D*>(spawned_node);
                    if (spawned_node_2d != nullptr) {
                        spawned_node_2d->set_position(Vector2(packet->x, packet->y));
                        UtilityFunctions::print("[CLIENT][Entity] Spawned ID: ", packet->netID);

                        // Only buffer remote players
                        if (packet->netID != local_player_net_id) {
                            auto& state = interpolation_states[packet->netID];
                            state.snapshots.push_back({now_ms, Vector2(packet->x, packet->y)});
                        }
                    }
                }
            }
        }
        else if (packet_type == PacketType::UPDATE)
        {
            UpdatePacket* packet = reinterpret_cast<UpdatePacket*>(read_buffer);
            if (bytes_read >= sizeof(UpdatePacket))
            {
                if (packet->netID == local_player_net_id) {
                    // Local Player: Apply authoritative position immediately (triggers visual stuttering)
                    Node* node = linking_context.get_node(packet->netID);
                    if (node) {
                        Node2D* node_2d = dynamic_cast<Node2D*>(node);
                        if (node_2d) {
                            node_2d->set_position(Vector2(packet->x, packet->y));
                        }
                    }
                } else {
                    // Remote Players: Feed the playback state machine
                    auto& state = interpolation_states[packet->netID];
                    state.snapshots.push_back({now_ms, Vector2(packet->x, packet->y)});

                    while (state.snapshots.size() > 10) {
                        state.snapshots.pop_front();
                    }
                }
            }
        }
        else if (packet_type == PacketType::DESPAWN)
        {
            DespawnPacket* packet = reinterpret_cast<DespawnPacket*>(read_buffer);
            if (bytes_read >= sizeof(DespawnPacket))
            {
                linking_context.despawn_network_object(packet->netID);
                interpolation_states.erase(packet->netID);
                UtilityFunctions::print("[CLIENT][Entity] Despawned ID: ", packet->netID);
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

    // --- SEND INPUT ---
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
        } else {
            UtilityFunctions::print("[CLIENT][Send] INPUT seq=", input_sequence, " DROPPED (simulated loss)");
        }
    }

    input_sequence++;

    // --- PING SYNC (every ~1 second) ---
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
                UtilityFunctions::print("[CLIENT][Send] PING_REQUEST - id=", ping_pkt.id,
                                        " t0=", (int64_t)ping_pkt.t0,
                                        " (last RTT=", current_rtt, "ms)");
            } else {
                UtilityFunctions::print("[CLIENT][Send] PING_REQUEST id=", ping_pkt.id, " DROPPED (simulated loss)");
            }
        }

        last_ping_send_time = now_ms;
    }
}

// ─────────────────────────────────────────────────────────
//  _exit_tree  -  Graceful disconnect
// ─────────────────────────────────────────────────────────

/**
 * @brief Sends a DISCONNECT packet to the server before the node is removed.
 */
void godot::NetworkManager::_exit_tree()
{
    if (Engine::get_singleton()->is_editor_hint()) {
        return;
    }

    if (socket) {
        DisconnectPacket packet;
        packet.type = PacketType::DISCONNECT;
        net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(DisconnectPacket));
        UtilityFunctions::print("[CLIENT][NetworkManager] Sent DISCONNECT to ", server_address, " - shutting down");
    } else {
        UtilityFunctions::print("[CLIENT][NetworkManager] Exiting (no active socket)");
    }
}

// ─────────────────────────────────────────────────────────
//  Godot bindings & property accessors
// ─────────────────────────────────────────────────────────

/**
 * @brief Registers properties and methods with the Godot ClassDB.
 *
 * Exposes:
 * - `simulated_packet_drop_chance` (float, 0–1, step 0.01)
 * - `get_current_rtt()` for UI / debug overlay access
 */
void godot::NetworkManager::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("set_simulated_packet_drop_chance", "chance"), &NetworkManager::set_simulated_packet_drop_chance);
    ClassDB::bind_method(D_METHOD("get_simulated_packet_drop_chance"), &NetworkManager::get_simulated_packet_drop_chance);

    ClassDB::bind_method(D_METHOD("get_current_rtt"), &NetworkManager::get_current_rtt);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_packet_drop_chance", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_simulated_packet_drop_chance", "get_simulated_packet_drop_chance");
}

/** @brief Returns the most recently measured RTT in milliseconds. */
uint32_t godot::NetworkManager::get_current_rtt() const
{
    return current_rtt;
}

/** @brief Sets the probability of artificially dropping an outgoing packet. */
void godot::NetworkManager::set_simulated_packet_drop_chance(double p_chance)
{
    simulated_packet_drop_chance = p_chance;
    UtilityFunctions::print("[CLIENT][NetworkManager] Simulated packet drop chance set to ", p_chance);
}

/** @brief Returns the current simulated packet drop probability. */
double godot::NetworkManager::get_simulated_packet_drop_chance() const
{
    return simulated_packet_drop_chance;
}
