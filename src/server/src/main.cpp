#include <iostream>
#include <snl.h>
#include <vector>
#include <cstring>
#include <algorithm>

#include "../../shared/include/net_protocol.h"

struct Client
{
    char address[128];
    uint32_t id;
};

struct PlayerObject
{
    NetID netID;
    int16_t x;
    int16_t y;
};

int main() {
    std::cout << "Init server" << std::endl;

    GameSocket* socket = net_socket_create("0.0.0.0:5000");
    std::cout << "Listening to port 5000" << std::endl;

    std::vector<Client> clients;
    uint32_t next_userID = 100;

    std::vector<PlayerObject> player_objects;
    uint32_t next_netID = 1;

    uint8_t read_buffer[1024];
    char sender_address[128];

    while (true) {
        int32_t bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128);
        if (bytes_read > 0) { // There is data
            PacketType packet_type = (PacketType)read_buffer[0];
            HelloPacket* hello_packet = reinterpret_cast<HelloPacket*>(read_buffer);

            if (packet_type == PacketType::HELLO) {
                auto it = std::find_if(clients.begin(), clients.end(), [&](const Client& c)
                {
                   if (std::strncmp(sender_address, c.address, 128) == 0)
                   {
                       return true;
                   } else return false;
                });

                bool is_new_client = (it == clients.end());

                if (is_new_client)
                {
                    std::cout << "[SERVER] New connection from " << sender_address << std::endl;

                    Client new_client;
                    memcpy(new_client.address, sender_address, sizeof(new_client.address));
                    new_client.id = next_userID;
                    clients.push_back(new_client);

                    std::cout << "[SERVER] Spawn Node ID " << next_netID << " at " << hello_packet->x << " " << hello_packet->y << std::endl;

                    PlayerObject new_player_object;
                    new_player_object.x = hello_packet->x;
                    new_player_object.y = hello_packet->y;
                    new_player_object.netID = next_netID;
                    player_objects.push_back(new_player_object);

                    SpawnPacket packet;
                    packet.type = PacketType::SPAWN;
                    packet.netID = next_netID;
                    packet.typeID = 1;
                    packet.x = hello_packet->x;
                    packet.y = hello_packet->y;

                    for (const auto& client : clients)
                    {
                        net_socket_send(socket, client.address, (uint8_t*)&packet, sizeof(SpawnPacket));
                    }

                    ++next_userID;
                    ++next_netID;

                    for (const auto& player_object : player_objects)
                    {
                        if (player_object.netID != packet.netID)
                        {
                            std::cout << "[SERVER] Sending previously connected client to new client" << std::endl;

                            SpawnPacket new_packet;
                            new_packet.type = PacketType::SPAWN;
                            new_packet.netID = player_object.netID;
                            new_packet.typeID = 1;
                            new_packet.x = player_object.x;
                            new_packet.y = player_object.y;

                            net_socket_send(socket, sender_address, (uint8_t*)&new_packet, sizeof(SpawnPacket));
                        } else std::cout << "[SERVER] This netID is the new client player" << std::endl;
                    }
                }
                else
                {
                    // TODO: Handle client packet
                    std::cout << "[SERVER] Old connection from " << sender_address << std::endl;
                }
            }
        }
    }

    return 0;
}