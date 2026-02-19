#ifndef GODOTPP_NET_MANAGER_H
#define GODOTPP_NET_MANAGER_H

#include <linking_context.h>

#include "snl.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>

namespace godot {
    class NetworkManager : public Node {
        GDCLASS(NetworkManager, Node)

    protected:
        const char* server_address = "127.0.0.1:5000";
        GameSocket *socket;

        LinkingContext linking_context;

        uint8_t read_buffer[1024];
        char sender_address[128];

    public:
        NetworkManager();
        ~NetworkManager();

        void _ready() override;

        void _process(double delta) override;


    protected:
        static void _bind_methods();
    };
}

#endif //GODOTPP_NET_MANAGER_H