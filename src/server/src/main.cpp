/**
 * @file main.cpp
 * @brief Authoritative UDP game server - ECS-based, decoupled Fixed Timestep.
 *
 * Runs a headless dedicated server that:
 * 1. Polls UDP packets as fast as possible, queuing inputs.
 * 2. Runs a strict Fixed Physics Update (60 Hz) using a time accumulator.
 * 3. Broadcasts authoritative UPDATE packets only after physics ticks.
 */

#include <chrono>
#include <iostream>
#include <snl.h>
#include <cstring>
#include <string>
#include <thread>
#include <queue>
#include <algorithm>

#include <entt/entt.hpp>

#include "../../shared/include/net_protocol.h"

// ─────────────────────────────────────────────────────────
//  ECS Components
// ─────────────────────────────────────────────────────────

/** @brief Stores the `"ip:port"` string identifying a connected client. */
struct ClientConnection {
    char address[128];
};

/** @brief Holds the unique, server-assigned network identifier for an entity. */
struct NetworkIDComp {
    NetID net_id;
};

/** @brief Stores the entity's type for client-side factory lookup. */
struct TypeComp {
    TypeID type_id;
};

/** @brief Authoritative 2D position maintained by the server. */
struct Position2D {
    int16_t x;
    int16_t y;
};

/** * @brief Tracks input replication state and queues pending inputs.
 * * Decouples network reception from the physics simulation. Inputs are
 * pushed upon receiving an InputPacket, and popped at a fixed 60Hz rate
 * by the PhysicsSystem.
 */
struct ClientInputComp {
    uint32_t last_sequence_received;       ///< Highest sequence processed by the network layer
    std::queue<InputState> pending_inputs; ///< Buffer of inputs waiting for physical simulation
};

// ─────────────────────────────────────────────────────────
//  Main Server Loop
// ─────────────────────────────────────────────────────────

int main() {
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;
    std::cout << "[SERVER] GodotPP Authoritative Server starting up" << std::endl;
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;

    GameSocket* socket = net_socket_create("0.0.0.0:5000");
    if (!socket) {
        std::cerr << "[SERVER] FATAL: Failed to bind UDP port 5000" << std::endl;
        return 1;
    }
    std::cout << "[SERVER] Socket bound - listening on UDP 0.0.0.0:5000" << std::endl;

    entt::registry registry;
    uint32_t next_netID = 1;

    uint8_t read_buffer[1024];
    char sender_address[128];

    // --- Fixed Timestep Setup ---
    using clock = std::chrono::steady_clock;
    using FloatDuration = std::chrono::duration<float>;

    const float FIXED_DT = 1.0f / 60.0f; // 60 Hz (0.01666... sec)
    float accumulator = 0.0f;
    auto previous_time = clock::now();
    uint64_t tick_count = 0;

    std::cout << "[SERVER] Tick rate configured: 30 Hz (Fixed Timestep)" << std::endl;

    while (true)
    {
        auto current_time = clock::now();
        float frame_time = std::chrono::duration_cast<FloatDuration>(current_time - previous_time).count();
        previous_time = current_time;

        // Spiral of Death protection: Cap frame_time if the server heavily freezes
        if (frame_time > 0.25f) {
            frame_time = 0.25f;
            std::cerr << "[SERVER][Perf] WARNING: Heavy server lag detected. Capping frame time." << std::endl;
        }

        accumulator += frame_time;

        // ─────────────────────────────────────────────
        //  1. NETWORK I/O (As fast as possible)
        // ─────────────────────────────────────────────
        int32_t bytes_read;
        while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
        {
            PacketType packet_type = (PacketType)read_buffer[0];

            if (packet_type == PacketType::HELLO)
            {
                if (bytes_read < sizeof(HelloPacket)) continue;
                HelloPacket* hello_packet = reinterpret_cast<HelloPacket*>(read_buffer);

                bool is_new_client = true;
                auto view = registry.view<ClientConnection>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) == 0) {
                        is_new_client = false;
                        break;
                    }
                }

                if (is_new_client) {
                    std::cout << "[SERVER][Recv] HELLO - New client connected: " << sender_address << std::endl;

                    entt::entity player_entity = registry.create();
                    auto& client_conn = registry.emplace<ClientConnection>(player_entity);
                    std::strncpy(client_conn.address, sender_address, 128);

                    registry.emplace<NetworkIDComp>(player_entity, next_netID);
                    registry.emplace<TypeComp>(player_entity, 1u);
                    registry.emplace<Position2D>(player_entity, hello_packet->x, hello_packet->y);

                    auto& input_comp = registry.emplace<ClientInputComp>(player_entity);
                    input_comp.last_sequence_received = 0;

                    SpawnPacket packet;
                    packet.type = PacketType::SPAWN;
                    packet.netID = next_netID;
                    packet.typeID = 1;
                    packet.x = hello_packet->x;
                    packet.y = hello_packet->y;

                    for (auto entity : view) {
                        const auto& existing_conn = view.get<ClientConnection>(entity);
                        net_socket_send(socket, existing_conn.address, (uint8_t*)&packet, sizeof(SpawnPacket));
                    }

                    next_netID++;

                    auto players_view = registry.view<NetworkIDComp, TypeComp, Position2D>();
                    for (auto entity : players_view) {
                        if (entity == player_entity) continue;
                        const auto& net_comp = players_view.get<NetworkIDComp>(entity);
                        const auto& type_comp = players_view.get<TypeComp>(entity);
                        const auto& pos_comp = players_view.get<Position2D>(entity);

                        SpawnPacket other_player_packet;
                        other_player_packet.type = PacketType::SPAWN;
                        other_player_packet.netID = net_comp.net_id;
                        other_player_packet.typeID = type_comp.type_id;
                        other_player_packet.x = pos_comp.x;
                        other_player_packet.y = pos_comp.y;

                        net_socket_send(socket, sender_address, (uint8_t*)&other_player_packet, sizeof(SpawnPacket));
                    }
                }
            }
            else if (packet_type == PacketType::INPUT)
            {
                if (bytes_read < sizeof(InputPacket)) continue;
                InputPacket* input_packet = reinterpret_cast<InputPacket*>(read_buffer);

                auto view = registry.view<ClientConnection, ClientInputComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) == 0) {
                        auto& input_comp = view.get<ClientInputComp>(entity);

                        int32_t diff = (int32_t)(input_packet->sequence - input_comp.last_sequence_received);
                        if (input_comp.last_sequence_received == 0) diff = 1;

                        if (diff > 0)
                        {
                            int32_t frames_to_process = std::min(diff, (int32_t)20);

                            // Push unseen inputs into the simulation queue
                            for (int32_t i = frames_to_process - 1; i >= 0; --i) {
                                input_comp.pending_inputs.push(input_packet->history[i]);
                            }

                            input_comp.last_sequence_received = input_packet->sequence;

                            // Anti-Cheat / Lag Protection:
                            // Cap the queue size so a lagging client doesn't build up 5 seconds of inputs
                            // and run at super-speed to catch up. (Max 10 frames = ~160ms of buffer).
                            while (input_comp.pending_inputs.size() > 10) {
                                input_comp.pending_inputs.pop();
                            }
                        }
                        break;
                    }
                }
            }
            else if (packet_type == PacketType::DISCONNECT)
            {
                auto view = registry.view<ClientConnection, NetworkIDComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);

                    if (std::strncmp(sender_address, conn.address, 128) == 0)
                    {
                        NetID disconnected_net_id = view.get<NetworkIDComp>(entity).net_id;
                        std::cout << "[SERVER][Recv] DISCONNECT - NetID: " << disconnected_net_id << std::endl;

                        registry.destroy(entity);

                        DespawnPacket despawn_packet;
                        despawn_packet.type = PacketType::DESPAWN;
                        despawn_packet.netID = disconnected_net_id;

                        auto remaining_clients = registry.view<ClientConnection>();
                        for (auto client_entity : remaining_clients) {
                            const auto& remaining_conn = remaining_clients.get<ClientConnection>(client_entity);
                            net_socket_send(socket, remaining_conn.address, (uint8_t*)&despawn_packet, sizeof(DespawnPacket));
                        }
                        break;
                    }
                }
            }
            else if (packet_type == PacketType::PING_REQUEST)
            {
                if (bytes_read < sizeof(PingRequestPacket)) continue;
                PingRequestPacket* ping_req = reinterpret_cast<PingRequestPacket*>(read_buffer);

                uint64_t t1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                PingResponsePacket ping_resp;
                ping_resp.type = PacketType::PING_RESPONSE;
                ping_resp.id = ping_req->id;
                ping_resp.t0 = ping_req->t0;
                ping_resp.t1 = t1;

                net_socket_send(socket, sender_address, (uint8_t*)&ping_resp, sizeof(PingResponsePacket));
            }
        }

        // ─────────────────────────────────────────────
        //  2. FIXED PHYSICS UPDATE (60 Hz)
        // ─────────────────────────────────────────────
        bool state_changed = false;

        while (accumulator >= FIXED_DT)
        {
            tick_count++;

            auto view_move = registry.view<Position2D, ClientInputComp>();
            for (auto entity : view_move) {
                auto& pos = view_move.get<Position2D>(entity);
                auto& input_comp = view_move.get<ClientInputComp>(entity);

                // Clock Drift Compensation: Process more inputs if the buffer bloats
                int32_t inputs_to_process = 1;
                if (input_comp.pending_inputs.size() > 2) {
                    inputs_to_process = 2; // Fast-forward to catch up
                } else if (input_comp.pending_inputs.size() > 5) {
                    inputs_to_process = 3; // Aggressive fast-forward for heavy jitter
                }

                for (int32_t i = 0; i < inputs_to_process; ++i) {
                    if (input_comp.pending_inputs.empty()) {
                        break;
                    }

                    InputState current_input = input_comp.pending_inputs.front();
                    input_comp.pending_inputs.pop();

                    int16_t move_x = 0;
                    int16_t move_y = 0;
                    int16_t speed = 5;

                    if (current_input.keys & InputFlags::UP)    move_y -= speed;
                    if (current_input.keys & InputFlags::DOWN)  move_y += speed;
                    if (current_input.keys & InputFlags::LEFT)  move_x -= speed;
                    if (current_input.keys & InputFlags::RIGHT) move_x += speed;

                    pos.x += move_x;
                    pos.y += move_y;
                }
            }

            accumulator -= FIXED_DT;
            state_changed = true;
        }

        // ─────────────────────────────────────────────
        //  3. STATE BROADCAST
        // ─────────────────────────────────────────────
        if (state_changed)
        {
            auto view_players = registry.view<NetworkIDComp, Position2D>();
            auto view_clients = registry.view<ClientConnection>();

            for (auto entity : view_players) {
                const auto& net = view_players.get<NetworkIDComp>(entity);
                const auto& pos = view_players.get<Position2D>(entity);

                UpdatePacket update_pkt;
                update_pkt.type = PacketType::UPDATE;
                update_pkt.netID = net.net_id;
                update_pkt.x = pos.x;
                update_pkt.y = pos.y;

                for (auto client_entity : view_clients) {
                    const auto& conn = view_clients.get<ClientConnection>(client_entity);
                    net_socket_send(socket, conn.address, (uint8_t*)&update_pkt, sizeof(UpdatePacket));
                }
            }
        }
        else
        {
            // Yield CPU if no physics tick occurred
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return 0;
}