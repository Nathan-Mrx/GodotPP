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

#include "../../shared/include/net_protocol.h"
#include "../../shared/include/world_packet.h"

namespace godot {

    /**
     * @brief Connection state machine for the client.
     *
     * NOT_CONNECTED  - Sending HELLO every 100 ms; times out after 1 s.
     * CONNECTING     - HELLO_ACK received; sending HSK every 100 ms; times out after 1 s.
     * CONNECTED      - Fully in-game; transitions to SPURIOUS if no UPDATE for 100 ms.
     * SPURIOUS       - No recent data; sends extra pings; back to CONNECTED on PONG,
     *                  or DISCONNECTED after 1 s.
     * DISCONNECTED   - Terminal state; networking stops.
     */
    enum class ConnectionState : uint8_t {
        NOT_CONNECTED = 0,
        CONNECTING    = 1,
        CONNECTED     = 2,
        SPURIOUS      = 3,
        DISCONNECTED  = 4
    };

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

        /** @brief Scratch buffer for incoming UDP datagrams.
         *  Sized to accommodate large WORLD_STATE packets:
         *  5 B header + N * 21 B per WorldObject → 8 KB handles ~390 objects. */
        static constexpr size_t kReadBufferSize = 8192;
        uint8_t read_buffer[kReadBufferSize];

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

        // ── Clock sync (Cristian's algorithm) ──────────────────────────────
        static constexpr int kRttSamples = 8;

        /** @brief Circular buffer of recent RTT measurements. */
        uint32_t rtt_ring_[kRttSamples] = {};

        /** @brief Write head for rtt_ring_ (mod kRttSamples gives slot index). */
        int rtt_head_ = 0;

        /** @brief Number of valid entries in rtt_ring_ (up to kRttSamples). */
        int rtt_count_ = 0;

        /** @brief Smoothed one-way delay: mean of the lowest 50% RTT samples (ms). */
        uint32_t smoothed_rtt_ms_ = 0;

        /**
         * @brief Estimated offset of the server clock relative to the local clock (ms).
         * Positive means the server is ahead; negative means the client is ahead.
         */
        int64_t clock_offset_ms_ = 0;

        /**
         * @brief Probability [0, 1] of artificially dropping an outgoing packet.
         *
         * Exposed to the Godot Inspector for network-condition testing.
         */
        double simulated_packet_drop_chance = 0.0;

        /** @brief The NetID of the local player, assigned in the HELLO_ACK. */
        NetID local_player_net_id = 0;

        // ── Desired spawn position (generated once in _ready, reused on HELLO retries) ──
        int16_t spawn_x_ = 0;
        int16_t spawn_y_ = 0;

        // ── Connection state machine ──────────────────────────────────────────────────
        ConnectionState connection_state_    = ConnectionState::NOT_CONNECTED;
        uint64_t state_enter_time_ms_        = 0; ///< When we last transitioned states.
        uint64_t last_hello_time_ms_         = 0; ///< Last HELLO send time (for 100 ms retry).
        uint64_t last_hsk_time_ms_           = 0; ///< Last HSK send time (for 100 ms retry).
        uint64_t last_data_received_ms_      = 0; ///< Last UPDATE/SPAWN/DESPAWN receive time.
        uint64_t last_spurious_ping_ms_      = 0; ///< Last ping sent in SPURIOUS state.

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

        // ── World geometry ─────────────────────────────────────────────────────
        /** @brief Static collision objects received from the server (WORLD_STATE).
         *  Kept in memory so the client prediction code can run sim::simulate_step()
         *  against the same geometry as the server. */
        std::vector<WorldObject> world_objects_;

        /** @brief Guard against processing duplicate WORLD_STATE retransmits. */
        bool world_state_received_ = false;

        // ── Client-side prediction infrastructure ─────────────────────────────
        /**
         * @brief One entry in the prediction history ring-buffer.
         *
         * The client stores (sequence, input, predicted position) for every
         * local physics tick. When a server UPDATE arrives with input_ack=K,
         * the reconciliation code looks up prediction_history_[K % kPredictionHistory]
         * to compare the local prediction against the authoritative result.
         */
        struct PredictionFrame {
            uint32_t   sequence; ///< Input sequence this frame corresponds to.
            InputState input;    ///< Input applied this frame.
            float      x, y;    ///< Predicted position AFTER applying input + collision.
        };

        static constexpr int kPredictionHistory = 64; ///< Must be power-of-two >= max expected RTT frames.
        PredictionFrame prediction_history_[kPredictionHistory] = {};

        /** @brief Current client-predicted position of the local player.
         *  Written every physics tick; used to set the node position locally
         *  before the server confirmation arrives. */
        Vector2 predicted_position_ = {};

        /** @brief server_tick echoed from the last UPDATE for the local player. */
        uint32_t last_server_tick_ = 0;

        /** @brief Last client input sequence the server confirmed for the local player.
         *  Used as the rollback point: re-simulate from this frame to current_sequence
         *  whenever the server correction exceeds the snap threshold. */
        uint32_t last_input_ack_ = 0;

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

        /** @brief Returns the smoothed RTT (mean of lowest 50% samples) in milliseconds. */
        uint32_t get_smoothed_rtt_ms() const;

        /** @brief Returns the estimated server-client clock offset in milliseconds. */
        int64_t get_clock_offset_ms() const;

        /** @brief Returns the current connection state as an integer (see ConnectionState enum). */
        int64_t get_connection_state() const;

        /** @brief Returns true only when fully connected and receiving game data. */
        bool is_connected() const;

    protected:
        /** @brief Transitions to a new state, updates the enter-time, and emits the signal. */
        void set_connection_state(ConnectionState new_state);

        /** @brief Binds properties and methods to the Godot ClassDB. */
        static void _bind_methods();
    };

} // namespace godot

#endif //GODOTPP_NET_MANAGER_H