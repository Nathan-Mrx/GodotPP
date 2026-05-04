/**
 * @file main.cpp
 * @brief Authoritative UDP game server - ECS-based, two-tier loop.
 *
 * The main loop has two distinct tiers:
 *
 * FAST PATH (every iteration, unbounded rate):
 *   Polls all pending UDP packets and responds to latency-sensitive packets
 *   (PING, HELLO, DISCONNECT) immediately - no physics tick required.
 *   INPUT packets are queued for the physics tier.
 *
 * PHYSICS PATH (fixed 60 Hz via accumulator):
 *   Consumes queued inputs, advances simulation, then broadcasts UPDATE.
 *   The loop yields CPU only when both tiers had nothing to do.
 */

#include <chrono>
#include <iostream>
#include <fstream>
#include <snl.h>
#include <cstring>
#include <string>
#include <thread>
#include <queue>
#include <algorithm>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include "../../shared/include/net_protocol.h"
#include "../../shared/include/world_packet.h"
#include "../../shared/include/sim.h"

// ─────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────

/** @brief Returns the current time as milliseconds from the steady clock. */
static uint64_t server_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

// ─────────────────────────────────────────────────────────
//  World Loader - reads level.json produced by export_level.gd
//  RECT:   param_a = half-width,  param_b = half-height
//  CIRCLE: param_a = radius,      param_b = 0
// ─────────────────────────────────────────────────────────

static std::vector<WorldObject> load_world_from_json(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[SERVER] WARNING: Cannot open level file '" << path
                  << "' - using empty world." << std::endl;
        return {};
    }

    nlohmann::json doc;
    try {
        file >> doc;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[SERVER] WARNING: JSON parse error in '" << path
                  << "': " << e.what() << " - using empty world." << std::endl;
        return {};
    }

    std::vector<WorldObject> objects;
    for (const auto& obj : doc.at("objects")) {
        WorldObject wo;
        const std::string& shape_str = obj.at("shape").get<std::string>();
        wo.shape    = (shape_str == "circle") ? WorldObjectShape::CIRCLE : WorldObjectShape::RECT;
        wo.x        = obj.at("x").get<float>();
        wo.y        = obj.at("y").get<float>();
        wo.rotation = obj.at("rotation").get<float>();
        wo.param_a  = obj.at("param_a").get<float>();
        wo.param_b  = obj.at("param_b").get<float>();
        objects.push_back(wo);
    }
    return objects;
}

// Collision resolution and physics constants are in sim.h (sim namespace).
// Both server and client prediction use sim::simulate_step() for identical results.

// ─────────────────────────────────────────────────────────
//  ECS Components
// ─────────────────────────────────────────────────────────

/** @brief Stores the `"ip:port"` string identifying a connected client. */
struct ClientConnection {
    char address[128];
};

/**
 * @brief Per-client connection state machine.
 *
 * HANDSHAKING - HELLO received, waiting for HSK. Entity is reserved but
 *               not yet visible to other clients.
 * CONNECTED   - HSK received, fully in-game. Receives UPDATE broadcasts
 *               and counts toward SPAWN storms.
 */
enum class ClientState : uint8_t { HANDSHAKING, CONNECTED };

struct ClientStateComp {
    ClientState state          = ClientState::HANDSHAKING;
    uint64_t    last_packet_ms = 0; ///< Steady-clock ms of the last packet from this client.
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

/**
 * @brief Tracks input replication state and queues pending inputs.
 *
 * Decouples network reception from the physics simulation. Inputs are
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

int main(int argc, char* argv[]) {
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;
    std::cout << "[SERVER] GodotPP Authoritative Server starting up" << std::endl;
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;

    std::string level_path = (argc >= 2) ? argv[1] : "game/level.json";
    std::vector<WorldObject> k_world = load_world_from_json(level_path);
    std::cout << "[SERVER] Level loaded: " << k_world.size()
              << " objects from '" << level_path << "'" << std::endl;

    GameSocket* socket = net_socket_create("0.0.0.0:5000");
    if (!socket) {
        std::cerr << "[SERVER] FATAL: Failed to bind UDP port 5000" << std::endl;
        return 1;
    }
    std::cout << "[SERVER] Socket bound - listening on UDP 0.0.0.0:5000" << std::endl;

    entt::registry registry;
    uint32_t next_netID = 1;

    uint8_t read_buffer[1024]; // server only receives small client packets (max ~185 B)
    char sender_address[128];

    // ─── WORLD_STATE reliability ───────────────────────────────────────────────
    // UDP is unreliable. After sending WORLD_STATE we schedule up to 3 retransmits
    // spaced 200 ms apart so packet loss doesn't leave the client with an empty world.
    struct PendingWorldResend {
        std::string          addr;
        std::vector<uint8_t> bytes;
        int                  resends_left;
        uint64_t             next_ms;
    };
    std::vector<PendingWorldResend> pending_world_resends;

    // --- Fixed Timestep Setup ---
    using clock = std::chrono::steady_clock;
    using FloatDuration = std::chrono::duration<float>;

    float accumulator = 0.0f;
    auto previous_time = clock::now();
    uint64_t tick_count = 0;

    std::cout << "[SERVER] Tick rate configured: " << static_cast<int>(1.0f / sim::FIXED_DT)
              << " Hz (Fixed Timestep)" << std::endl;

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
        //  FAST PATH - Immediate-response packets
        //  PING / HELLO / DISCONNECT are answered here,
        //  without waiting for a physics tick.
        //  INPUT is queued for the physics path below.
        // ─────────────────────────────────────────────
        bool any_packets = false;
        int32_t bytes_read;
        while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
        {
            any_packets = true;
            PacketType packet_type = (PacketType)read_buffer[0];

            if (packet_type == PacketType::HELLO)
            {
                if (bytes_read < sizeof(HelloPacket)) continue;
                HelloPacket* hello_packet = reinterpret_cast<HelloPacket*>(read_buffer);

                // Ignore retransmits from a client already in the registry.
                bool is_new_client = true;
                auto existing_view = registry.view<ClientConnection>();
                for (auto entity : existing_view) {
                    if (std::strncmp(sender_address, existing_view.get<ClientConnection>(entity).address, 128) == 0) {
                        is_new_client = false;
                        break;
                    }
                }

                if (is_new_client) {
                    std::cout << "[SERVER][Recv] HELLO from " << sender_address
                              << " - reserving NetID " << next_netID << std::endl;

                    entt::entity player_entity = registry.create();
                    auto& conn = registry.emplace<ClientConnection>(player_entity);
                    std::strncpy(conn.address, sender_address, 128);

                    registry.emplace<NetworkIDComp>(player_entity, next_netID);
                    registry.emplace<TypeComp>(player_entity, 1u);
                    registry.emplace<Position2D>(player_entity, hello_packet->x, hello_packet->y);
                    registry.emplace<ClientInputComp>(player_entity).last_sequence_received = 0;
                    registry.emplace<ClientStateComp>(player_entity,
                        ClientState::HANDSHAKING, server_now_ms());

                    HelloAckPacket hello_ack;
                    hello_ack.type            = PacketType::HELLO_ACK;
                    hello_ack.assigned_net_id = next_netID;
                    net_socket_send(socket, sender_address,
                                    (uint8_t*)&hello_ack, sizeof(HelloAckPacket));

                    next_netID++;
                }
            }
            else if (packet_type == PacketType::HSK)
            {
                auto view = registry.view<ClientConnection, ClientStateComp,
                                          NetworkIDComp, TypeComp, Position2D>();
                for (auto entity : view) {
                    if (std::strncmp(sender_address,
                            view.get<ClientConnection>(entity).address, 128) != 0) continue;

                    auto& state_comp = view.get<ClientStateComp>(entity);
                    if (state_comp.state != ClientState::HANDSHAKING) break;

                    state_comp.state          = ClientState::CONNECTED;
                    state_comp.last_packet_ms = server_now_ms();

                    const auto& net  = view.get<NetworkIDComp>(entity);
                    const auto& type = view.get<TypeComp>(entity);
                    const auto& pos  = view.get<Position2D>(entity);
                    std::cout << "[SERVER][Recv] HSK - NetID " << net.net_id
                              << " from " << sender_address << " is now CONNECTED" << std::endl;

                    SpawnPacket spawn_pkt;
                    spawn_pkt.type   = PacketType::SPAWN;
                    spawn_pkt.netID  = net.net_id;
                    spawn_pkt.typeID = type.type_id;
                    spawn_pkt.x      = pos.x;
                    spawn_pkt.y      = pos.y;

                    HskAckPacket hsk_ack;
                    hsk_ack.type = PacketType::HSK_ACK;
                    net_socket_send(socket, sender_address,
                                    (uint8_t*)&hsk_ack, sizeof(HskAckPacket));

                    // Send the new player their own SPAWN so they can create their local node
                    net_socket_send(socket, sender_address,
                                    (uint8_t*)&spawn_pkt, sizeof(SpawnPacket));

                    // Broadcast new player's SPAWN to every already-connected client

                    auto clients = registry.view<ClientConnection, ClientStateComp>();
                    for (auto other : clients) {
                        if (other == entity) continue;
                        if (clients.get<ClientStateComp>(other).state != ClientState::CONNECTED) continue;
                        net_socket_send(socket,
                            clients.get<ClientConnection>(other).address,
                            (uint8_t*)&spawn_pkt, sizeof(SpawnPacket));
                    }

                    // Send SPAWN for every already-connected entity to the new client
                    auto players = registry.view<NetworkIDComp, TypeComp, Position2D, ClientStateComp>();
                    for (auto other : players) {
                        if (other == entity) continue;
                        if (players.get<ClientStateComp>(other).state != ClientState::CONNECTED) continue;

                        SpawnPacket other_pkt;
                        other_pkt.type   = PacketType::SPAWN;
                        other_pkt.netID  = players.get<NetworkIDComp>(other).net_id;
                        other_pkt.typeID = players.get<TypeComp>(other).type_id;
                        other_pkt.x      = players.get<Position2D>(other).x;
                        other_pkt.y      = players.get<Position2D>(other).y;
                        net_socket_send(socket, sender_address,
                                        (uint8_t*)&other_pkt, sizeof(SpawnPacket));
                    }

                    // Send the full static world geometry to the new client.
                    // Schedule 3 retransmits (200 ms apart) for UDP reliability.
                    {
                        WorldStatePacket world_pkt;
                        world_pkt.objects = k_world;
                        auto bytes = world_pkt.to_bytes();
                        net_socket_send(socket, sender_address,
                                        bytes.data(), static_cast<int32_t>(bytes.size()));
                        std::cout << "[SERVER] Sent WORLD_STATE ("
                                  << world_pkt.objects.size() << " objects, "
                                  << bytes.size() << " bytes) to " << sender_address << std::endl;
                        pending_world_resends.push_back({
                            std::string(sender_address),
                            std::move(bytes),
                            3,
                            server_now_ms() + 200
                        });
                    }
                    break;
                }
            }
            else if (packet_type == PacketType::INPUT)
            {
                if (bytes_read < sizeof(InputPacket)) continue;
                InputPacket* input_packet = reinterpret_cast<InputPacket*>(read_buffer);

                auto view = registry.view<ClientConnection, ClientInputComp, ClientStateComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) != 0) continue;

                    // Only process inputs from fully connected clients
                    if (view.get<ClientStateComp>(entity).state != ClientState::CONNECTED) break;

                    view.get<ClientStateComp>(entity).last_packet_ms = server_now_ms();
                    auto& input_comp = view.get<ClientInputComp>(entity);

                    int32_t diff = (int32_t)(input_packet->sequence - input_comp.last_sequence_received);
                    if (input_comp.last_sequence_received == 0) diff = 1;

                    if (diff > 0)
                    {
                        int32_t frames_to_process = std::min(diff, (int32_t)20);

                        for (int32_t i = frames_to_process - 1; i >= 0; --i) {
                            input_comp.pending_inputs.push(input_packet->history[i]);
                        }

                        input_comp.last_sequence_received = input_packet->sequence;

                        // Cap queue: prevent a lagging client from replaying seconds of inputs
                        while (input_comp.pending_inputs.size() > 10) {
                            input_comp.pending_inputs.pop();
                        }
                    }
                    break;
                }
            }
            else if (packet_type == PacketType::DISCONNECT)
            {
                auto view = registry.view<ClientConnection, NetworkIDComp, ClientStateComp>();
                for (auto entity : view) {
                    if (std::strncmp(sender_address,
                            view.get<ClientConnection>(entity).address, 128) != 0) continue;

                    NetID disconnected_net_id = view.get<NetworkIDComp>(entity).net_id;
                    std::cout << "[SERVER][Recv] DISCONNECT - NetID: " << disconnected_net_id << std::endl;

                    registry.destroy(entity);

                    // Cancel any pending WORLD_STATE retransmits for this client.
                    pending_world_resends.erase(
                        std::remove_if(pending_world_resends.begin(), pending_world_resends.end(),
                            [&](const PendingWorldResend& r) {
                                return r.addr == std::string(sender_address);
                            }),
                        pending_world_resends.end());

                    DespawnPacket despawn_packet;
                    despawn_packet.type  = PacketType::DESPAWN;
                    despawn_packet.netID = disconnected_net_id;

                    // Only notify fully-connected clients
                    auto remaining = registry.view<ClientConnection, ClientStateComp>();
                    for (auto other : remaining) {
                        if (remaining.get<ClientStateComp>(other).state != ClientState::CONNECTED) continue;
                        net_socket_send(socket,
                            remaining.get<ClientConnection>(other).address,
                            (uint8_t*)&despawn_packet, sizeof(DespawnPacket));
                    }
                    break;
                }
            }
            else if (packet_type == PacketType::PING_REQUEST)
            {
                if (bytes_read < sizeof(PingRequestPacket)) continue;
                PingRequestPacket* ping_req = reinterpret_cast<PingRequestPacket*>(read_buffer);

                // Update keepalive timestamp for this client
                auto kv = registry.view<ClientConnection, ClientStateComp>();
                for (auto entity : kv) {
                    if (std::strncmp(sender_address,
                            kv.get<ClientConnection>(entity).address, 128) == 0) {
                        kv.get<ClientStateComp>(entity).last_packet_ms = server_now_ms();
                        break;
                    }
                }

                uint64_t t1 = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );

                PingResponsePacket ping_resp;
                ping_resp.type = PacketType::PING_RESPONSE;
                ping_resp.id   = ping_req->id;
                ping_resp.t0   = ping_req->t0;
                ping_resp.t1   = t1;
                net_socket_send(socket, sender_address,
                                (uint8_t*)&ping_resp, sizeof(PingResponsePacket));
            }
        }

        // ─────────────────────────────────────────────
        //  TIMEOUT DETECTION (once per second)
        //  Auto-disconnect clients that stopped sending
        //  packets for more than 5 seconds.
        // ─────────────────────────────────────────────
        static uint64_t last_timeout_check_ms = 0;
        uint64_t now_ms = server_now_ms();
        if (now_ms - last_timeout_check_ms >= 1000) {
            last_timeout_check_ms = now_ms;

            std::vector<entt::entity> timed_out;
            auto tv = registry.view<ClientStateComp, NetworkIDComp>();
            for (auto entity : tv) {
                if (now_ms - tv.get<ClientStateComp>(entity).last_packet_ms > 5000) {
                    timed_out.push_back(entity);
                }
            }

            for (auto entity : timed_out) {
                NetID id = registry.get<NetworkIDComp>(entity).net_id;
                std::string timed_out_addr = registry.get<ClientConnection>(entity).address;
                std::cout << "[SERVER] Timeout: NetID " << id << " auto-disconnected" << std::endl;
                registry.destroy(entity);

                // Cancel any pending WORLD_STATE retransmits for this client.
                pending_world_resends.erase(
                    std::remove_if(pending_world_resends.begin(), pending_world_resends.end(),
                        [&](const PendingWorldResend& r) { return r.addr == timed_out_addr; }),
                    pending_world_resends.end());

                DespawnPacket despawn;
                despawn.type  = PacketType::DESPAWN;
                despawn.netID = id;

                auto remaining = registry.view<ClientConnection, ClientStateComp>();
                for (auto other : remaining) {
                    if (remaining.get<ClientStateComp>(other).state != ClientState::CONNECTED) continue;
                    net_socket_send(socket,
                        remaining.get<ClientConnection>(other).address,
                        (uint8_t*)&despawn, sizeof(DespawnPacket));
                }
            }
        }

        // ─────────────────────────────────────────────
        //  PHYSICS PATH - Fixed 60 Hz simulation
        //  Consumes queued inputs, advances positions.
        // ─────────────────────────────────────────────
        bool state_changed = false;

        while (accumulator >= sim::FIXED_DT)
        {
            tick_count++;

            auto view_move = registry.view<Position2D, ClientInputComp>();
            for (auto entity : view_move) {
                auto& pos = view_move.get<Position2D>(entity);
                auto& input_comp = view_move.get<ClientInputComp>(entity);

                // Clock Drift Compensation: Process more inputs if the buffer bloats.
                // Check from largest threshold downward so each branch is reachable.
                int32_t inputs_to_process = 1;
                if (input_comp.pending_inputs.size() > 5) {
                    inputs_to_process = 3; // Aggressive catch-up for heavy jitter
                } else if (input_comp.pending_inputs.size() > 2) {
                    inputs_to_process = 2; // Gentle catch-up
                }

                for (int32_t i = 0; i < inputs_to_process; ++i) {
                    if (input_comp.pending_inputs.empty()) {
                        break;
                    }

                    InputState current_input = input_comp.pending_inputs.front();
                    input_comp.pending_inputs.pop();

                    float fx = static_cast<float>(pos.x);
                    float fy = static_cast<float>(pos.y);
                    sim::simulate_step(fx, fy, current_input, k_world);
                    pos.x = static_cast<int16_t>(fx);
                    pos.y = static_cast<int16_t>(fy);
                }
            }

            accumulator -= sim::FIXED_DT;
            state_changed = true;
        }

        // ─────────────────────────────────────────────
        //  WORLD_STATE retransmits (reliability)
        //  Drains the pending resend queue so UDP packet loss
        //  doesn't leave a client with an empty world.
        // ─────────────────────────────────────────────
        {
            uint64_t now_ms = server_now_ms();
            for (auto& r : pending_world_resends) {
                if (r.resends_left > 0 && now_ms >= r.next_ms) {
                    net_socket_send(socket, r.addr.c_str(),
                                    r.bytes.data(), static_cast<int32_t>(r.bytes.size()));
                    r.resends_left--;
                    r.next_ms += 200;
                }
            }
            pending_world_resends.erase(
                std::remove_if(pending_world_resends.begin(), pending_world_resends.end(),
                               [](const PendingWorldResend& r) { return r.resends_left == 0; }),
                pending_world_resends.end());
        }

        // ─────────────────────────────────────────────
        //  BROADCAST - Authoritative state to all clients
        //  Only runs after a physics tick to avoid sending
        //  stale data between simulation steps.
        // ─────────────────────────────────────────────
        if (state_changed)
        {
            // Only broadcast to CONNECTED clients; HANDSHAKING ones don't have
            // their SPAWN yet, so sending UPDATE for unknown NetIDs is useless.
            auto view_players = registry.view<NetworkIDComp, Position2D, ClientStateComp, ClientInputComp>();
            auto view_clients = registry.view<ClientConnection, ClientStateComp>();

            for (auto entity : view_players) {
                if (view_players.get<ClientStateComp>(entity).state != ClientState::CONNECTED) continue;

                const auto& net = view_players.get<NetworkIDComp>(entity);
                const auto& pos = view_players.get<Position2D>(entity);
                const auto& inp = view_players.get<ClientInputComp>(entity);

                UpdatePacket update_pkt;
                update_pkt.type        = PacketType::UPDATE;
                update_pkt.netID       = net.net_id;
                update_pkt.x           = pos.x;
                update_pkt.y           = pos.y;
                update_pkt.server_tick = static_cast<uint32_t>(tick_count);
                update_pkt.input_ack   = inp.last_sequence_received;

                for (auto client_entity : view_clients) {
                    if (view_clients.get<ClientStateComp>(client_entity).state != ClientState::CONNECTED) continue;
                    net_socket_send(socket,
                        view_clients.get<ClientConnection>(client_entity).address,
                        (uint8_t*)&update_pkt, sizeof(UpdatePacket));
                }
            }
        }
        else if (!any_packets)
        {
            // Truly idle: no packets arrived and no physics tick ran.
            // Yield CPU briefly. Skip the sleep if packets came in so that
            // back-to-back pings or bursts are drained without added latency.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return 0;
}