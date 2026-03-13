#include <chrono>
#include <iostream>
#include <snl.h>
#include <cstring>
#include <string>
#include <thread>

#include <entt/entt.hpp>

#include "../../shared/include/net_protocol.h"

// --- COMPOSANTS ECS ---

struct ClientConnection {
    char address[128];
};

struct NetworkIDComp {
    NetID net_id;
};

struct TypeComp {
    TypeID type_id;
};

struct Position2D {
    int16_t x;
    int16_t y;
};

// Stocke l'état réseau des inputs du client
struct ClientInputComp {
    uint32_t last_sequence;
};


int main() {
    std::cout << "[SERVER] Init server" << std::endl;

    GameSocket* socket = net_socket_create("0.0.0.0:5000");
    if (!socket) {
        std::cerr << "[SERVER] Failed to bind port 5000" << std::endl;
        return 1;
    }
    std::cout << "[SERVER] Listening on UDP 5000" << std::endl;

    entt::registry registry;
    uint32_t next_netID = 1;

    uint8_t read_buffer[1024];
    char sender_address[128];

    // Mise en place du Tickrate (60 Tick Per Second)
    using clock = std::chrono::steady_clock;
    const std::chrono::milliseconds tick_rate(16); // ~60Hz
    auto next_tick_time = clock::now() + tick_rate;

    while (true)
    {
        // 1. LECTURE DES PAQUETS
        int32_t bytes_read;
        while ((bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128)) > 0)
        {
            PacketType packet_type = (PacketType)read_buffer[0];

            if (packet_type == PacketType::HELLO)
            {
                // ... (Garde ta logique HELLO existante, instanciation et envoi SPAWN) ...
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
                    std::cout << "[SERVER] New connection from " << sender_address << std::endl;

                    entt::entity player_entity = registry.create();
                    auto& client_conn = registry.emplace<ClientConnection>(player_entity);
                    std::strncpy(client_conn.address, sender_address, 128);

                    registry.emplace<NetworkIDComp>(player_entity, next_netID);
                    registry.emplace<TypeComp>(player_entity, 1u);
                    registry.emplace<Position2D>(player_entity, hello_packet->x, hello_packet->y);

                    auto& input_comp = registry.emplace<ClientInputComp>(player_entity);
                    input_comp.last_sequence = 0;

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

                auto view = registry.view<ClientConnection, ClientInputComp, Position2D, NetworkIDComp>();
                for (auto entity : view) {
                    const auto& conn = view.get<ClientConnection>(entity);
                    if (std::strncmp(sender_address, conn.address, 128) == 0) {
                        auto& input_comp = view.get<ClientInputComp>(entity);

                        int32_t diff = (int32_t)(input_packet->sequence - input_comp.last_sequence);

                        // Cas particulier : premier paquet reçu par le serveur pour ce client
                        if (input_comp.last_sequence == 0) {
                            diff = 1;
                        }

                        // Si diff <= 0, c'est un vieux paquet ou un doublon. On l'ignore silencieusement.
                        if (diff > 0)
                        {
                            auto& pos = view.get<Position2D>(entity);

                            // On limite la récupération à la taille maximale de notre Ring Buffer (20)
                            int32_t frames_to_process = std::min(diff, (int32_t)20);

                            // On dépile à l'envers pour respecter la chronologie des événements
                            for (int32_t i = frames_to_process - 1; i >= 0; --i)
                            {
                                InputState current_input = input_packet->history[i];

                                int16_t move_x = 0;
                                int16_t move_y = 0;
                                // TODO: Extraire ça dans un vrai PhysicsSystem EnTT
                                int16_t speed = 5;

                                if (current_input.keys & InputFlags::UP)    move_y -= speed;
                                if (current_input.keys & InputFlags::DOWN)  move_y += speed;
                                if (current_input.keys & InputFlags::LEFT)  move_x -= speed;
                                if (current_input.keys & InputFlags::RIGHT) move_x += speed;

                                pos.x += move_x;
                                pos.y += move_y;
                            }

                            input_comp.last_sequence = input_packet->sequence;

                            if (diff > 1) {
                                std::cout << "[SERVER] Recovered " << (diff - 1)
                                          << " dropped inputs for NetID " << view.get<NetworkIDComp>(entity).net_id << std::endl;
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
                        std::cout << "[SERVER] Client disconnected: " << sender_address
                                  << " (NetID " << disconnected_net_id << ")" << std::endl;

                        // On détruit l'entité côté serveur
                        registry.destroy(entity);

                        // On avertit les clients restants
                        DespawnPacket despawn_packet;
                        despawn_packet.type = PacketType::DESPAWN;
                        despawn_packet.netID = disconnected_net_id;

                        // Attention, on re-récupère une vue car la précédente a été modifiée par destroy()
                        auto remaining_clients = registry.view<ClientConnection>();
                        for (auto client_entity : remaining_clients) {
                            const auto& remaining_conn = remaining_clients.get<ClientConnection>(client_entity);
                            net_socket_send(socket, remaining_conn.address, (uint8_t*)&despawn_packet, sizeof(DespawnPacket));
                        }

                        break;
                    }
                }

                // TODO: Gérer un mécanisme de Timeout / LastSeenTimestamp pour kick les clients
                // qui ont crashé ou perdu leur connexion sans envoyer de paquet DISCONNECT.
            }
            else if (packet_type == PacketType::PING_REQUEST)
            {
                if (bytes_read < sizeof(PingRequestPacket)) continue;
                PingRequestPacket* ping_req = reinterpret_cast<PingRequestPacket*>(read_buffer);

                // On récupère le temps absolu du serveur en millisecondes
                uint64_t t1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                PingResponsePacket ping_resp;
                ping_resp.type = PacketType::PING_RESPONSE;
                ping_resp.id = ping_req->id;
                ping_resp.t0 = ping_req->t0; // On renvoie le t0 du client intact
                ping_resp.t1 = t1;          // On attache le temps serveur

                // On répond immédiatement à l'expéditeur, sans même avoir besoin
                // de chercher son entité EnTT. C'est du fire-and-forget.
                net_socket_send(socket, sender_address, (uint8_t*)&ping_resp, sizeof(PingResponsePacket));
            }
        }

        // 2. TICK SYNC ET BROADCAST (Envoi au client)
        auto now = clock::now();
        if (now >= next_tick_time)
        {
            auto view_players = registry.view<NetworkIDComp, Position2D>();
            auto view_clients = registry.view<ClientConnection>();

            // On construit le paquet et on l'envoie à tout le monde
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

            // On recule le prochain tick de 16ms
            next_tick_time += tick_rate;
        }
        else
        {
            // On dort pour libérer le processeur (Evite le 100% d'utilisation sur 1 core)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return 0;
}