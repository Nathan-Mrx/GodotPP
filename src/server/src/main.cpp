#include <iostream>
#include <snl.h>

int main() {
    std::cout << "Hello, World!" << std::endl;

    std::cout << "SNL version: " << net_get_version() << std::endl;

    std::cout << "Creating socket..." << std::endl;
    GameSocket* sock = net_socket_create("127.0.0.1:5000");
    std::cout << "Socket created!" << std::endl;

    uint8_t out_data[1024];
    char out_sender[128];

    std::cout << "Waiting for packets..." << std::endl;

    bool stop = false;
    while (!stop) {

        uint32_t bytes_read = net_socket_poll(sock, out_data, 1024, out_sender, 128);
        if (bytes_read > 0) {
            std::cout << "Packet received!" << std::endl;
        }

    };

    net_socket_destroy(sock);

    return 0;
}