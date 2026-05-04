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
#include "../../shared/include/world_packet.h"

#include <algorithm>
#include <random>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/engine.hpp>

#include "godot_cpp/classes/viewport.hpp"
#include <godot_cpp/classes/static_body2d.hpp>
#include <godot_cpp/classes/collision_shape2d.hpp>
#include <godot_cpp/classes/rectangle_shape2d.hpp>
#include <godot_cpp/classes/circle_shape2d.hpp>

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
void godot::NetworkManager::set_connection_state(ConnectionState new_state)
{
    connection_state_   = new_state;
    state_enter_time_ms_ = Time::get_singleton()->get_ticks_msec();
    emit_signal("connection_state_changed", static_cast<int64_t>(new_state));
    UtilityFunctions::print("[CLIENT][State] -> ", static_cast<int>(new_state));
}

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
    set_process(true);

    UtilityFunctions::print("[CLIENT][NetworkManager] Creating UDP socket on 127.0.0.1:0 (ephemeral port)...");
    socket = net_socket_create("127.0.0.1:0");

    if (!socket) {
        UtilityFunctions::print("[CLIENT][NetworkManager] ERROR: Socket creation failed");
        set_connection_state(ConnectionState::DISCONNECTED);
        return;
    }

    // Generate spawn position once; reused across HELLO retransmits.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(-512, 512);
    spawn_x_ = static_cast<int16_t>(distrib(gen));
    spawn_y_ = static_cast<int16_t>(distrib(gen) / 2);

    linking_context = LinkingContext();
    linking_context.register_type(1, []() -> Node*
    {
        Ref<PackedScene> player_scene = ResourceLoader::get_singleton()->load("res://player.tscn");
        return player_scene->instantiate();
    });

    // State machine starts in NOT_CONNECTED; first HELLO is sent from _physics_process.
    set_connection_state(ConnectionState::NOT_CONNECTED);
    UtilityFunctions::print("[CLIENT][NetworkManager] Ready - starting handshake with ", server_address);
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

            // Slew: ±1ms/frame cap, driven by buffer deviation from the 3-frame target.
            // This is a gentler complement to speed_multiplier - it acts earlier and
            // avoids the abrupt jumps that occur when the multiplier threshold is crossed.
            int buffer_excess = static_cast<int>(buffer.size()) - 3;
            double slew_sec = std::clamp(buffer_excess * 0.001, -0.001, 0.001);
            state.playback_t += ((delta + slew_sec) / duration_sec) * speed_multiplier;

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
    if (!socket) return;

    uint64_t now_ms = Time::get_singleton()->get_ticks_msec();

    // ── STATE MACHINE: Outgoing actions ──────────────────────────────────────
    switch (connection_state_)
    {
        case ConnectionState::NOT_CONNECTED:
        {
            if (now_ms - last_hello_time_ms_ >= 100) {
                HelloPacket hello;
                hello.type = PacketType::HELLO;
                hello.x    = spawn_x_;
                hello.y    = spawn_y_;
                net_socket_send(socket, server_address, (uint8_t*)&hello, sizeof(HelloPacket));
                last_hello_time_ms_ = now_ms;
            }
            if (now_ms - state_enter_time_ms_ >= 1000) {
                UtilityFunctions::print("[CLIENT] Connection timed out (no HELLO_ACK)");
                set_connection_state(ConnectionState::DISCONNECTED);
            }
            break;
        }
        case ConnectionState::CONNECTING:
        {
            if (now_ms - last_hsk_time_ms_ >= 100) {
                HskPacket hsk;
                hsk.type = PacketType::HSK;
                net_socket_send(socket, server_address, (uint8_t*)&hsk, sizeof(HskPacket));
                last_hsk_time_ms_ = now_ms;
            }
            if (now_ms - state_enter_time_ms_ >= 1000) {
                UtilityFunctions::print("[CLIENT] Handshake timed out (no HSK_ACK)");
                set_connection_state(ConnectionState::DISCONNECTED);
            }
            break;
        }
        case ConnectionState::CONNECTED:
        {
            if (now_ms - last_data_received_ms_ >= 100) {
                UtilityFunctions::print("[CLIENT] No server data for 100 ms - entering SPURIOUS");
                set_connection_state(ConnectionState::SPURIOUS);
            }
            break;
        }
        case ConnectionState::SPURIOUS:
        {
            if (now_ms - last_spurious_ping_ms_ >= 200) {
                PingRequestPacket ping_pkt;
                ping_pkt.type = PacketType::PING_REQUEST;
                ping_pkt.id   = ping_id++;
                ping_pkt.t0   = now_ms;
                net_socket_send(socket, server_address, (uint8_t*)&ping_pkt, sizeof(PingRequestPacket));
                last_spurious_ping_ms_ = now_ms;
            }
            if (now_ms - state_enter_time_ms_ >= 1000) {
                UtilityFunctions::print("[CLIENT] Spurious timeout - disconnecting");
                set_connection_state(ConnectionState::DISCONNECTED);
            }
            break;
        }
        case ConnectionState::DISCONNECTED:
            return;
    }

    // ── RECEIVE LOOP ─────────────────────────────────────────────────────────
    int32_t bytes_read;
    while ((bytes_read = net_socket_poll(socket, read_buffer, static_cast<int32_t>(kReadBufferSize), sender_address, 128)) > 0)
    {
        PacketType packet_type = (PacketType)read_buffer[0];

        if (packet_type == PacketType::HELLO_ACK)
        {
            HelloAckPacket* packet = reinterpret_cast<HelloAckPacket*>(read_buffer);
            if (bytes_read >= (int32_t)sizeof(HelloAckPacket)
                    && connection_state_ == ConnectionState::NOT_CONNECTED)
            {
                local_player_net_id = packet->assigned_net_id;
                UtilityFunctions::print("[CLIENT] HELLO_ACK - assigned NetID: ", local_player_net_id);
                last_hsk_time_ms_ = 0; // force immediate HSK on next tick
                set_connection_state(ConnectionState::CONNECTING);
            }
        }
        else if (packet_type == PacketType::HSK_ACK)
        {
            if (bytes_read >= (int32_t)sizeof(HskAckPacket)
                    && connection_state_ == ConnectionState::CONNECTING)
            {
                last_data_received_ms_ = now_ms;
                UtilityFunctions::print("[CLIENT] HSK_ACK - fully connected");
                set_connection_state(ConnectionState::CONNECTED);
            }
        }
        else if (packet_type == PacketType::SPAWN)
        {
            SpawnPacket* packet = reinterpret_cast<SpawnPacket*>(read_buffer);
            if (bytes_read >= (int32_t)sizeof(SpawnPacket))
            {
                last_data_received_ms_ = now_ms;

                Node* spawned_node = linking_context.spawn_network_object(packet->netID, packet->typeID);
                if (spawned_node)
                {
                    add_child(spawned_node);
                    Node2D* spawned_node_2d = dynamic_cast<Node2D*>(spawned_node);
                    if (spawned_node_2d) {
                        spawned_node_2d->set_position(Vector2(packet->x, packet->y));
                        UtilityFunctions::print("[CLIENT][Entity] Spawned ID: ", packet->netID);

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
            if (bytes_read >= (int32_t)sizeof(UpdatePacket))
            {
                last_data_received_ms_ = now_ms;
                if (connection_state_ == ConnectionState::SPURIOUS) {
                    set_connection_state(ConnectionState::CONNECTED);
                }

                if (packet->netID == local_player_net_id) {
                    // Store reconciliation anchors for client-side prediction.
                    // When prediction is active, these replace the direct position set:
                    //   1. Find prediction_history_[input_ack_ % kPredictionHistory]
                    //   2. Compare its (x, y) to packet (x, y)
                    //   3. If error > threshold: rollback and re-simulate
                    last_server_tick_ = packet->server_tick;
                    last_input_ack_   = packet->input_ack;

                    Node* node = linking_context.get_node(packet->netID);
                    if (node) {
                        Node2D* node_2d = dynamic_cast<Node2D*>(node);
                        if (node_2d) node_2d->set_position(Vector2(packet->x, packet->y));
                    }
                } else {
                    auto& state = interpolation_states[packet->netID];
                    state.snapshots.push_back({now_ms, Vector2(packet->x, packet->y)});
                    while (state.snapshots.size() > 10) state.snapshots.pop_front();
                }
            }
        }
        else if (packet_type == PacketType::DESPAWN)
        {
            DespawnPacket* packet = reinterpret_cast<DespawnPacket*>(read_buffer);
            if (bytes_read >= (int32_t)sizeof(DespawnPacket))
            {
                last_data_received_ms_ = now_ms;
                linking_context.despawn_network_object(packet->netID);
                interpolation_states.erase(packet->netID);
                UtilityFunctions::print("[CLIENT][Entity] Despawned ID: ", packet->netID);
            }
        }
        else if (packet_type == PacketType::WORLD_STATE)
        {
            if (world_state_received_) {
                // Duplicate from reliability retransmit - ignore silently.
            } else {
                auto pkt = WorldStatePacket::from_bytes(read_buffer, static_cast<size_t>(bytes_read));
                if (!pkt) {
                    UtilityFunctions::print("[CLIENT][World] Failed to parse WORLD_STATE");
                } else {
                    world_state_received_ = true;

                    // Keep the raw geometry for sim::simulate_step() during prediction.
                    world_objects_ = pkt->objects;

                    // Instantiate Godot collision nodes for local physics (visual/audio feedback).
                    for (const auto& obj : world_objects_) {
                        StaticBody2D* body = memnew(StaticBody2D);
                        CollisionShape2D* col = memnew(CollisionShape2D);

                        if (obj.shape == WorldObjectShape::RECT) {
                            Ref<RectangleShape2D> shape;
                            shape.instantiate();
                            shape->set_size(Vector2(obj.param_a * 2.0f, obj.param_b * 2.0f));
                            col->set_shape(shape);
                        } else {
                            Ref<CircleShape2D> shape;
                            shape.instantiate();
                            shape->set_radius(obj.param_a);
                            col->set_shape(shape);
                        }

                        body->add_child(col);
                        body->set_position(Vector2(obj.x, obj.y));
                        body->set_rotation(obj.rotation);
                        add_child(body);
                    }
                    UtilityFunctions::print("[CLIENT][World] Received and stored ",
                        static_cast<int64_t>(world_objects_.size()), " world objects");
                }
            }
        }
        else if (packet_type == PacketType::PING_RESPONSE)
        {
            PingResponsePacket* packet = reinterpret_cast<PingResponsePacket*>(read_buffer);
            if (bytes_read >= (int32_t)sizeof(PingResponsePacket))
            {
                uint64_t t_receive = Time::get_singleton()->get_ticks_msec();
                current_rtt = (uint32_t)(t_receive - packet->t0);

                rtt_ring_[rtt_head_ % kRttSamples] = current_rtt;
                rtt_head_++;
                if (rtt_count_ < kRttSamples) rtt_count_++;

                uint32_t sorted[kRttSamples];
                std::copy(rtt_ring_, rtt_ring_ + rtt_count_, sorted);
                std::sort(sorted, sorted + rtt_count_);
                int low_n = std::max(1, rtt_count_ / 2);
                uint64_t sum = 0;
                for (int i = 0; i < low_n; ++i) sum += sorted[i];
                smoothed_rtt_ms_ = (uint32_t)(sum / low_n);

                int64_t estimated_server_now = (int64_t)packet->t1 + (int64_t)(smoothed_rtt_ms_ / 2);
                clock_offset_ms_ = estimated_server_now - (int64_t)t_receive;

                // Pong received while spurious: connection is alive again
                if (connection_state_ == ConnectionState::SPURIOUS) {
                    set_connection_state(ConnectionState::CONNECTED);
                }
            }
        }
    }

    // ── SEND INPUT (only when in-game) ───────────────────────────────────────
    if (connection_state_ != ConnectionState::CONNECTED
            && connection_state_ != ConnectionState::SPURIOUS) return;

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
    }

    input_history[input_sequence % 20] = current_state;

    InputPacket pkt;
    pkt.type     = PacketType::INPUT;
    pkt.sequence = input_sequence;
    for (int i = 0; i < 20; ++i) {
        pkt.history[i] = (input_sequence >= static_cast<uint32_t>(i))
            ? input_history[(input_sequence - i) % 20]
            : InputState{};
    }

    bool drop_input = (simulated_packet_drop_chance > 0.0)
        && (UtilityFunctions::randf() < simulated_packet_drop_chance);
    if (!drop_input) {
        net_socket_send(socket, server_address, (uint8_t*)&pkt, sizeof(InputPacket));
    } else {
        UtilityFunctions::print("[CLIENT][Send] INPUT seq=", input_sequence, " DROPPED (simulated)");
    }
    input_sequence++;

    // ── PERIODIC PING (every 1 s, only in CONNECTED) ─────────────────────────
    if (connection_state_ == ConnectionState::CONNECTED
            && now_ms - last_ping_send_time >= 1000)
    {
        PingRequestPacket ping_pkt;
        ping_pkt.type = PacketType::PING_REQUEST;
        ping_pkt.id   = ping_id++;
        ping_pkt.t0   = now_ms;

        bool drop_ping = (simulated_packet_drop_chance > 0.0)
            && (UtilityFunctions::randf() < simulated_packet_drop_chance);
        if (!drop_ping) {
            net_socket_send(socket, server_address, (uint8_t*)&ping_pkt, sizeof(PingRequestPacket));
            UtilityFunctions::print("[CLIENT][Send] PING id=", ping_pkt.id, " RTT=", current_rtt, "ms");
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
    ClassDB::bind_method(D_METHOD("get_smoothed_rtt_ms"), &NetworkManager::get_smoothed_rtt_ms);
    ClassDB::bind_method(D_METHOD("get_clock_offset_ms"), &NetworkManager::get_clock_offset_ms);
    ClassDB::bind_method(D_METHOD("get_connection_state"), &NetworkManager::get_connection_state);
    ClassDB::bind_method(D_METHOD("is_connected"), &NetworkManager::is_connected);

    ADD_SIGNAL(MethodInfo("connection_state_changed",
        PropertyInfo(Variant::INT, "new_state")));

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_packet_drop_chance", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_simulated_packet_drop_chance", "get_simulated_packet_drop_chance");
}

/** @brief Returns the most recently measured RTT in milliseconds. */
uint32_t godot::NetworkManager::get_current_rtt() const
{
    return current_rtt;
}

/** @brief Returns the smoothed RTT (mean of lowest 50% samples) in milliseconds. */
uint32_t godot::NetworkManager::get_smoothed_rtt_ms() const
{
    return smoothed_rtt_ms_;
}

/** @brief Returns the estimated server-client clock offset in milliseconds. */
int64_t godot::NetworkManager::get_clock_offset_ms() const
{
    return clock_offset_ms_;
}

/** @brief Returns the current connection state as an integer. */
int64_t godot::NetworkManager::get_connection_state() const
{
    return static_cast<int64_t>(connection_state_);
}

/** @brief Returns true only in CONNECTED or SPURIOUS states. */
bool godot::NetworkManager::is_connected() const
{
    return connection_state_ == ConnectionState::CONNECTED
        || connection_state_ == ConnectionState::SPURIOUS;
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
