#include "net_manager.h"

#include <random>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

godot::NetworkManager::NetworkManager() {}

godot::NetworkManager::~NetworkManager() {}

void godot::NetworkManager::_ready()
{
    Node::_ready();

    socket = net_socket_create("127.0.0.1:0");

    if (socket) {
        // Send hello packet with x y
        HelloPacket packet;
        packet.type = PacketType::HELLO;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(-512, 512);

        packet.x = distrib(gen);
        packet.y = distrib(gen) / 2;

        UtilityFunctions::print("[CLIENT] Send hello at: ", packet.x, ", ", packet.y);
        net_socket_send(socket, server_address, (uint8_t*)&packet, sizeof(HelloPacket));
    } else UtilityFunctions::print("[CLIENT] Socket could not be created");

    linking_context = LinkingContext();
    linking_context.register_type(1, []() -> Node*
    {
        Ref<PackedScene> player_scene = ResourceLoader::get_singleton()->load("res://player.tscn");
        return player_scene->instantiate();
    });
}

void godot::NetworkManager::_process(double delta)
{
    Node::_process(delta);

    int32_t bytes_read = net_socket_poll(socket, read_buffer, 1024, sender_address, 128);
    if (bytes_read > 0) { // There is data
        PacketType packet_type = (PacketType)read_buffer[0];

        UtilityFunctions::print("[CLIENT] Packet of type ", (uint8_t)packet_type);
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
                    if (spawned_node_2d != nullptr) spawned_node_2d->set_position(Vector2(packet->x, packet->y));
                    UtilityFunctions::print("[CLIENT] Spawned ID: ", packet->netID, " at: ", packet->x, ", ", packet->y);
                }
            }
        }
        else
        {
            UtilityFunctions::print("[CLIENT] Packet not of type SPAWN ", (uint8_t)packet_type);
        }
    }
}

void godot::NetworkManager::_bind_methods() {}