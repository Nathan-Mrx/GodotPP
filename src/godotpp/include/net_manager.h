/**
 * @file net_manager.h
 * @brief Client-side network manager - handles UDP I/O, interpolation, and input.
 *
 * NetworkManager is the single Godot Node responsible for all client
 * networking.  It opens a UDP socket, sends a HelloPacket on `_ready()`,
 * receives authoritative state from the server in `_physics_process()`,
 * and renders smoothly interpolated positions in `_process()`.
 */

#ifndef GODOTPP_NET_MANAGER_H
#define GODOTPP_NET_MANAGER_H

#include <deque>
#include <linking_context.h>
#include "snl.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <vector>
#include <unordered_map>

namespace godot {

    /**
     * @brief A timestamped 2D position snapshot used for client-side interpolation.
     *
     * Stored in per-entity ring-buffers inside `interpolation_buffers`.
     * The renderer picks two adjacent snapshots that straddle
     * `(now - interpolation_delay_ms)` and lerps between them.
     */
    struct TransformSnapshot {
        uint64_t timestamp; ///< Client-local time (ms) when the snapshot was received.
        Vector2  position;  ///< 2D world position from the server's UpdatePacket.
    };

    /**
     * @class NetworkManager
     * @brief Godot Node that drives all client-side networking.
     *
     * ### Lifecycle
     * | Callback            | Frequency   | Role                                        |
     * |---------------------|-------------|---------------------------------------------|
     * | `_ready()`          | Once        | Opens socket, sends HELLO, registers types. |
     * | `_process()`        | Every frame | Interpolates entity positions for rendering. |
     * | `_physics_process()`| 60 Hz       | Polls packets, sends inputs + pings.         |
     * | `_exit_tree()`      | Once        | Sends DISCONNECT and tears down.             |
     *
     * ### Exposed Properties (Godot Inspector)
     * - `simulated_packet_drop_chance` (float 0–1): Artificially drops outgoing
     *   packets to stress-test the input redundancy mechanism.
     */
    class NetworkManager : public Node {
        GDCLASS(NetworkManager, Node)

    protected:
        /** @brief Target server endpoint (IP:port). */
        const char* server_address = "127.0.0.1:5000";

        /** @brief SNL UDP socket handle, created in `_ready()`. */
        GameSocket *socket;

        /** @brief Client-side NetID ↔ Node* registry. */
        LinkingContext linking_context;

        /** @brief Scratch buffer for incoming UDP datagrams (max 1024 bytes). */
        uint8_t read_buffer[1024];

        /** @brief String buffer filled by `net_socket_poll` with the sender's address. */
        char sender_address[128];

        /** @brief Monotonically increasing input frame counter. */
        uint32_t input_sequence = 0;

        /**
         * @brief Client-side ring-buffer of the last 20 input frames.
         *
         * Indexed by `(input_sequence % 20)`.  Packed into every
         * InputPacket so the server can recover dropped frames.
         */
        InputState input_history[20] = {};

        /** @brief Monotonically increasing ping probe ID. */
        uint32_t ping_id = 0;

        /** @brief Timestamp (ms) of the last ping probe sent. */
        uint64_t last_ping_send_time = 0;

        /** @brief Most recent round-trip time measurement (ms). */
        uint32_t current_rtt = 0;

        /**
         * @brief Probability [0, 1] of artificially dropping an outgoing packet.
         *
         * Exposed to the Godot Inspector for network-condition testing.
         */
        double simulated_packet_drop_chance = 0.0;

        /** @brief The NetID of the local player, assigned by the server upon first SPAWN. */
        NetID local_player_net_id = 0;

        /**
         * @brief State machine for frame-based interpolation playback.
         * Guarantees a strict frame delay regardless of server tick rate.
         */
        struct InterpolationState {
            std::deque<TransformSnapshot> snapshots;
            double playback_t = 0.0;
            bool buffering = true;
        };

        /** @brief Registry of all active interpolators keyed by NetID. */
        std::unordered_map<NetID, InterpolationState> interpolation_states;

    public:
        NetworkManager();
        ~NetworkManager();

        /** @brief Opens the socket, sends HELLO, and registers entity factories. */
        void _ready() override;

        /**
         * @brief Interpolates entity positions between received snapshots.
         * @param delta Seconds since last visual frame.
         *
         * Runs every frame (uncapped FPS).  Uses the interpolation buffer
         * to produce smooth motion even when the physics tick rate is lower.
         */
        void _process(double delta) override;

        /**
         * @brief Polls incoming packets and sends player input + pings.
         * @param delta Seconds since last physics tick (~16 ms).
         *
         * Handles SPAWN, UPDATE, DESPAWN, and PING_RESPONSE packets.
         * Builds an InputPacket with the current + recent 19 frames and
         * sends it to the server.
         */
        void _physics_process(double delta) override;

        /** @brief Sends a DISCONNECT packet before the node leaves the tree. */
        void _exit_tree() override;

        /** @brief Sets the simulated packet drop probability. */
        void set_simulated_packet_drop_chance(double p_chance);

        /** @brief Returns the simulated packet drop probability. */
        double get_simulated_packet_drop_chance() const;

        /** @brief Returns the most recently measured RTT in milliseconds. */
        uint32_t get_current_rtt() const;

    protected:
        /** @brief Binds properties and methods to the Godot ClassDB. */
        static void _bind_methods();
    };

} // namespace godot

#endif //GODOTPP_NET_MANAGER_H