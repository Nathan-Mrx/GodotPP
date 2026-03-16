/**
 * @file main.cpp
 * @brief Authoritative UDP game server — ECS-based, 60 Hz tick rate.
 *
 * Runs a headless dedicated server that:
 *   1. Listens on UDP port 5000 for client packets.
 *   2. Uses EnTT as its entity-component system to manage player state.
 *   3. Processes HELLO, INPUT, DISCONNECT, and PING_REQUEST packets.
 *   4. Broadcasts authoritative UPDATE packets to all clients every tick.
 *
 * ### ECS Components
 * | Component         | Purpose                                         |
 * |-------------------|-------------------------------------------------|
 * | ClientConnection  | Stores the client's `address:port` string.      |
 * | NetworkIDComp     | Holds the server-assigned NetID.                 |
 * | TypeComp          | Entity type for client-side factory lookup.      |
 * | Position2D        | Authoritative 2D world position.                |
 * | ClientInputComp   | Tracks the last processed input sequence number. |
 *
 * ### Tick Model
 * The main loop spins with a 1 ms sleep and fires a tick (~16 ms / 60 Hz)
 * that broadcasts every entity's position to every connected client.
 */

#include <chrono>
#include <iostream>
#include <snl.h>
#include <cstring>
#include <string>
#include <thread>

#include <entt/entt.hpp>

#include "../../shared/include/net_protocol.h"

// ─────────────────────────────────────────────────────────
//  ECS Components
// ─────────────────────────────────────────────────────────

/**
 * @brief Stores the `"ip:port"` string identifying a connected client.
 *
 * Used to route outgoing packets to the correct endpoint and to match
 * incoming packets to their owning entity.
 */
struct ClientConnection {
    char address[128]; ///< Null-terminated address string (e.g. "127.0.0.1:54321").
};

/**
 * @brief Holds the unique, server-assigned network identifier for an entity.
 */
struct NetworkIDComp {
    NetID net_id; ///< Monotonically increasing ID, starts at 1.
};

/**
 * @brief Stores the entity's type for client-side factory lookup.
 */
struct TypeComp {
    TypeID type_id; ///< Maps to a `LinkingContext::register_type()` key on the client.
};

/**
 * @brief Authoritative 2D position maintained by the server.
 */
struct Position2D {
    int16_t x; ///< World X coordinate.
    int16_t y; ///< World Y coordinate.
};

/**
 * @brief Tracks input replication state for a connected client.
 *
 * The server uses `last_sequence` to determine how many frames to
 * replay from an incoming InputPacket's ring-buffer history.
 */
struct ClientInputComp {
    uint32_t last_sequence; ///< Most recently processed input sequence number.
};


/**
 * @brief Server entry-point — binds the socket and runs the authoritative game loop.
 *
 * The loop has two phases each iteration:
 *   1. **Packet reception** — drains the socket and processes every datagram.
 *   2. **Tick broadcast**   — at 60 Hz, sends UpdatePackets for every entity
 *      to every connected client.
 */
int main() {
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;
    std::cout << "[SERVER] GodotPP Authoritative Server starting up" << std::endl;
    std::cout << "[SERVER] ═══════════════════════════════════════════" << std::endl;

    GameSocket* socket = net_socket_create("0.0.0.0:5000");
    if (!socket) {
        std::cerr << "[SERVER] FATAL: Failed to bind UDP port 5000 — is another instance running?" << std::endl;
        return 1;
    }
    std::cout << "[SERVER] Socket bound — listening on UDP 0.0.0.0:5000" << std::endl;

    entt::registry registry;
    uint32_t next_netID = 1;

    uint8_t read_buffer[1024];
    char sender_address[128];

    // --- Tick-rate setup: ~60 Hz (16 ms per tick) ---
    using clock = std::chrono::steady_clock;
    const std::chrono::milliseconds tick_rate(16);
    auto next_tick_time = clock::now() + tick_rate;
    uint64_t tick_count = 0;

    std::cout << "[SERVER] Tick rate configured: 60 Hz (16 ms interval)" << std::endl;
    std::cout << "[SERVER] Waiting for client connections..." << std::endl;

    while (true)
    {
        // ─────────────────────────────────────────────
        //  1. PACKET RECEPTION
        // ─────────────────────────────────────────────
        int32_t bytes_read;
        while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
        {
            PacketType packet_type = (PacketType)read_buffer[0];

            // ── HELLO ───────────────────────────────
            if (packet_type == PacketType::HELLO)
            {
                if (bytes_read < sizeof(HelloPacket)) {
                    std::cerr << "[SERVER][Recv] WARNING: HELLO packet too small ("
                              << bytes_read << " bytes, expected " << sizeof(HelloPacket) << ") from "
                              << sender_address << " — ignoring" << std::endl;
                    continue;
                }
                HelloPacket* hello_packet = reinterpret_cast<HelloPacket*>(read_buffer);

                // Check if this client is already connected
                bool is_new_client = true;
                auto view = registry.view<ClientConnection>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) == 0) {
                        is_new_client = false;
                        std::cout << "[SERVER][Recv] HELLO from " << sender_address
                                  << " — client already connected, ignoring duplicate" << std::endl;
                        break;
                    }
                }

                if (is_new_client) {
                    auto total_clients = registry.view<ClientConnection>().size();
                    std::cout << "[SERVER][Recv] ─────────────────────────────────────" << std::endl;
                    std::cout << "[SERVER][Recv] HELLO — New client connected!" << std::endl;
                    std::cout << "[SERVER][Recv]   Address : " << sender_address << std::endl;
                    std::cout << "[SERVER][Recv]   Position: (" << hello_packet->x << ", " << hello_packet->y << ")" << std::endl;
                    std::cout << "[SERVER][Recv]   NetID   : " << next_netID << std::endl;
                    std::cout << "[SERVER][Recv]   Clients : " << (total_clients + 1) << " total" << std::endl;
                    std::cout << "[SERVER][Recv] ─────────────────────────────────────" << std::endl;

                    // Create the player entity with all required components
                    entt::entity player_entity = registry.create();
                    auto& client_conn = registry.emplace<ClientConnection>(player_entity);
                    std::strncpy(client_conn.address, sender_address, 128);

                    registry.emplace<NetworkIDComp>(player_entity, next_netID);
                    registry.emplace<TypeComp>(player_entity, 1u);
                    registry.emplace<Position2D>(player_entity, hello_packet->x, hello_packet->y);

                    auto& input_comp = registry.emplace<ClientInputComp>(player_entity);
                    input_comp.last_sequence = 0;

                    // Broadcast the new player's SPAWN to all existing clients
                    SpawnPacket packet;
                    packet.type = PacketType::SPAWN;
                    packet.netID = next_netID;
                    packet.typeID = 1;
                    packet.x = hello_packet->x;
                    packet.y = hello_packet->y;

                    int broadcast_count = 0;
                    for (auto entity : view) {
                        const auto& existing_conn = view.get<ClientConnection>(entity);
                        net_socket_send(socket, existing_conn.address, (uint8_t*)&packet, sizeof(SpawnPacket));
                        broadcast_count++;
                    }
                    std::cout << "[SERVER][Send] SPAWN NetID=" << next_netID
                              << " broadcast to " << broadcast_count << " client(s)" << std::endl;

                    next_netID++;

                    // Send all existing entities to the new client
                    auto players_view = registry.view<NetworkIDComp, TypeComp, Position2D>();
                    int existing_count = 0;
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
                        existing_count++;
                    }
                    if (existing_count > 0) {
                        std::cout << "[SERVER][Send] Sent " << existing_count
                                  << " existing entity SPAWN(s) to new client " << sender_address << std::endl;
                    }
                }
            }
            // ── INPUT ───────────────────────────────
            else if (packet_type == PacketType::INPUT)
            {
                if (bytes_read < sizeof(InputPacket)) {
                    std::cerr << "[SERVER][Recv] WARNING: INPUT packet too small ("
                              << bytes_read << " bytes, expected " << sizeof(InputPacket) << ") from "
                              << sender_address << " — ignoring" << std::endl;
                    continue;
                }
                InputPacket* input_packet = reinterpret_cast<InputPacket*>(read_buffer);

                auto view = registry.view<ClientConnection, ClientInputComp, Position2D, NetworkIDComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) == 0) {
                        auto& input_comp = view.get<ClientInputComp>(entity);

                        int32_t diff = (int32_t)(input_packet->sequence - input_comp.last_sequence);

                        // First packet ever received for this client
                        if (input_comp.last_sequence == 0) {
                            diff = 1;
                        }

                        // Old/duplicate packet — ignore silently
                        if (diff > 0)
                        {
                            auto& pos = view.get<Position2D>(entity);

                            // Limit recovery to ring-buffer size (20)
                            int32_t frames_to_process = std::min(diff, (int32_t)20);

                            // Process frames in chronological order (oldest first)
                            for (int32_t i = frames_to_process - 1; i >= 0; --i)
                            {
                                InputState current_input = input_packet->history[i];

                                int16_t move_x = 0;
                                int16_t move_y = 0;
                                // TODO: Extract into a proper EnTT PhysicsSystem
                                int16_t speed = 5;

                                if (current_input.keys & InputFlags::UP)    move_y -= speed;
                                if (current_input.keys & InputFlags::DOWN)  move_y += speed;
                                if (current_input.keys & InputFlags::LEFT)  move_x -= speed;
                                if (current_input.keys & InputFlags::RIGHT) move_x += speed;

                                pos.x += move_x;
                                pos.y += move_y;
                            }

                            input_comp.last_sequence = input_packet->sequence;

                            NetID entity_net_id = view.get<NetworkIDComp>(entity).net_id;

                            if (diff > 1) {
                                std::cout << "[SERVER][Recv] INPUT — NetID=" << entity_net_id
                                          << " seq=" << input_packet->sequence
                                          << " recovered " << (diff - 1) << " dropped frame(s)"
                                          << " (processed " << frames_to_process << " total)"
                                          << " → pos=(" << pos.x << ", " << pos.y << ")" << std::endl;
                            }
                        }
                        break;
                    }
                }
            }
            // ── DISCONNECT ──────────────────────────
            else if (packet_type == PacketType::DISCONNECT)
            {
                auto view = registry.view<ClientConnection, NetworkIDComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);

                    if (std::strncmp(sender_address, conn.address, 128) == 0)
                    {
                        NetID disconnected_net_id = view.get<NetworkIDComp>(entity).net_id;

                        std::cout << "[SERVER][Recv] ─────────────────────────────────────" << std::endl;
                        std::cout << "[SERVER][Recv] DISCONNECT — Client leaving" << std::endl;
                        std::cout << "[SERVER][Recv]   Address: " << sender_address << std::endl;
                        std::cout << "[SERVER][Recv]   NetID  : " << disconnected_net_id << std::endl;

                        // Destroy server-side entity
                        registry.destroy(entity);

                        // Notify remaining clients
                        DespawnPacket despawn_packet;
                        despawn_packet.type = PacketType::DESPAWN;
                        despawn_packet.netID = disconnected_net_id;

                        // Re-fetch view since the registry was modified by destroy()
                        auto remaining_clients = registry.view<ClientConnection>();
                        int notified = 0;
                        for (auto client_entity : remaining_clients) {
                            const auto& remaining_conn = remaining_clients.get<ClientConnection>(client_entity);
                            net_socket_send(socket, remaining_conn.address, (uint8_t*)&despawn_packet, sizeof(DespawnPacket));
                            notified++;
                        }

                        std::cout << "[SERVER][Send] DESPAWN NetID=" << disconnected_net_id
                                  << " broadcast to " << notified << " remaining client(s)" << std::endl;
                        std::cout << "[SERVER][Recv]   Remaining clients: " << remaining_clients.size() << std::endl;
                        std::cout << "[SERVER][Recv] ─────────────────────────────────────" << std::endl;

                        break;
                    }
                }

                // TODO: Implement a Timeout / LastSeenTimestamp mechanism to kick
                // clients that crashed without sending a DISCONNECT packet.
            }
            // ── PING_REQUEST ────────────────────────
            else if (packet_type == PacketType::PING_REQUEST)
            {
                if (bytes_read < sizeof(PingRequestPacket)) {
                    std::cerr << "[SERVER][Recv] WARNING: PING_REQUEST packet too small ("
                              << bytes_read << " bytes, expected " << sizeof(PingRequestPacket) << ") from "
                              << sender_address << " — ignoring" << std::endl;
                    continue;
                }
                PingRequestPacket* ping_req = reinterpret_cast<PingRequestPacket*>(read_buffer);

                // Capture server time for clock-offset estimation
                uint64_t t1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                PingResponsePacket ping_resp;
                ping_resp.type = PacketType::PING_RESPONSE;
                ping_resp.id = ping_req->id;
                ping_resp.t0 = ping_req->t0; // Echo client's timestamp back
                ping_resp.t1 = t1;           // Attach server timestamp

                // Fire-and-forget — no entity lookup needed
                net_socket_send(socket, sender_address, (uint8_t*)&ping_resp, sizeof(PingResponsePacket));

                std::cout << "[SERVER][Recv] PING_REQUEST id=" << ping_req->id
                          << " from " << sender_address
                          << " — responded with t1=" << t1 << std::endl;
            }
            // ── UNKNOWN ─────────────────────────────
            else
            {
                std::cerr << "[SERVER][Recv] WARNING: Unknown packet type="
                          << (int)packet_type << " (" << bytes_read << " bytes) from "
                          << sender_address << " — ignoring" << std::endl;
            }
        }

        // ─────────────────────────────────────────────
        //  2. TICK SYNC & BROADCAST
        // ─────────────────────────────────────────────
        auto now = clock::now();
        if (now >= next_tick_time)
        {
            tick_count++;

            auto view_players = registry.view<NetworkIDComp, Position2D>();
            auto view_clients = registry.view<ClientConnection>();

            size_t entity_count = 0;
            size_t client_count = view_clients.size();

            // Build and broadcast an UpdatePacket for every entity to every client
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
                entity_count++;
            }

            // Log tick summary every 300 ticks (~5 seconds) to avoid spam
            if (tick_count % 300 == 0 && client_count > 0) {
                std::cout << "[SERVER][Tick] #" << tick_count
                          << " — broadcast " << entity_count << " entity UPDATE(s) to "
                          << client_count << " client(s) ("
                          << (entity_count * client_count) << " packets total)" << std::endl;
            }

            // Advance to the next tick
            next_tick_time += tick_rate;
        }
        else
        {
            // Sleep to avoid 100% CPU usage on one core
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return 0;
}