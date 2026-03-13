#ifndef GODOTPP_NET_MANAGER_H
#define GODOTPP_NET_MANAGER_H

#include <linking_context.h>
#include "snl.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <vector>
#include <unordered_map>

namespace godot {

    // Représente une position reçue à un instant T
    struct TransformSnapshot {
        uint64_t timestamp;
        Vector2 position;
    };

    class NetworkManager : public Node {
        GDCLASS(NetworkManager, Node)

    protected:
        const char* server_address = "127.0.0.1:5000";
        GameSocket *socket;

        LinkingContext linking_context;

        uint8_t read_buffer[1024];
        char sender_address[128];

        uint32_t input_sequence = 0;
        InputState input_history[20] = {};

        uint32_t ping_id = 0;
        uint64_t last_ping_send_time = 0;
        uint32_t current_rtt = 0;

        double simulated_packet_drop_chance = 0.0;

        // Map associant un NetID à son historique de positions reçues du serveur
        std::unordered_map<NetID, std::vector<TransformSnapshot>> interpolation_buffers;
        // Délai dans le passé pour l'affichage (Plus il y a de packet loss/gigue, plus il faut l'augmenter)
        uint64_t interpolation_delay_ms = 100;

    public:
        NetworkManager();
        ~NetworkManager();

        void _ready() override;
        void _process(double delta) override;
        void _physics_process(double delta) override;
        void _exit_tree() override;

        void set_simulated_packet_drop_chance(double p_chance);
        double get_simulated_packet_drop_chance() const;

        uint32_t get_current_rtt() const;

    protected:
        static void _bind_methods();
    };
}

#endif //GODOTPP_NET_MANAGER_H