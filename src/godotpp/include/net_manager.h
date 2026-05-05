/**
 * @file net_manager.h
 * @brief Client-side authoritative network manager.
 *
 * Single Godot Node responsible for the full client networking stack:
 * connection state machine, packet I/O, entity interpolation, and
 * client-side prediction with spring-damper reconciliation.
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
     * @brief Client connection state machine.
     *
     * NOT_CONNECTED → sends HELLO every 100 ms, times out after 1 s.
     * CONNECTING    → sends HSK every 100 ms after HELLO_ACK, times out after 1 s.
     * CONNECTED     → in-game; transitions to SPURIOUS if no UPDATE for 100 ms.
     * SPURIOUS      → sends pings; returns to CONNECTED on PONG, DISCONNECTED on timeout.
     * DISCONNECTED  → terminal; networking halted.
     */
    enum class ConnectionState : uint8_t {
        NOT_CONNECTED = 0,
        CONNECTING    = 1,
        CONNECTED     = 2,
        SPURIOUS      = 3,
        DISCONNECTED  = 4
    };

    /** @brief Timestamped position sample for the interpolation ring-buffer. */
    struct TransformSnapshot {
        uint64_t timestamp; ///< Client-local receive time (ms).
        Vector2  position;
    };

    /**
     * @class NetworkManager
     * @brief Drives all client-side networking for the local player and remote entities.
     *
     * | Callback             | Rate     | Responsibility                              |
     * |----------------------|----------|---------------------------------------------|
     * | `_ready()`           | once     | Open socket, send HELLO, register factories.|
     * | `_physics_process()` | 60 Hz    | Poll packets, predict, send input + ping.   |
     * | `_process()`         | uncapped | Interpolate remote entity positions.        |
     * | `_exit_tree()`       | once     | Send DISCONNECT, tear down socket.          |
     */
    class NetworkManager : public Node {
        GDCLASS(NetworkManager, Node)

    protected:
        const char* server_address = "127.0.0.1:5000";

        GameSocket* socket;
        LinkingContext linking_context;

        /// Receive buffer sized for the largest expected packet (WORLD_STATE).
        /// At 21 B per WorldObject + 5 B header, 8 KB covers ~390 objects.
        static constexpr size_t kReadBufferSize = 8192;
        uint8_t read_buffer[kReadBufferSize];
        char    sender_address[128];

        uint32_t input_sequence    = 0;
        InputState input_history[20] = {};

        uint32_t ping_id            = 0;
        uint64_t last_ping_send_time = 0;
        uint32_t current_rtt        = 0;

        // Cristian's algorithm — smoothed RTT from the lowest 50% of recent samples.
        static constexpr int kRttSamples = 8;
        uint32_t rtt_ring_[kRttSamples] = {};
        int      rtt_head_  = 0;
        int      rtt_count_ = 0;
        uint32_t smoothed_rtt_ms_ = 0;

        /// Estimated server-clock offset relative to local clock (ms).
        /// Positive = server ahead. Derived from Cristian's algorithm on each PONG.
        int64_t clock_offset_ms_ = 0;

        double simulated_packet_drop_chance = 0.0;

        NetID   local_player_net_id = 0;
        int16_t spawn_x_ = 0, spawn_y_ = 0;

        ConnectionState connection_state_   = ConnectionState::NOT_CONNECTED;
        uint64_t state_enter_time_ms_       = 0;
        uint64_t last_hello_time_ms_        = 0;
        uint64_t last_hsk_time_ms_          = 0;
        uint64_t last_data_received_ms_     = 0;
        uint64_t last_spurious_ping_ms_     = 0;

        // ── Interpolation (remote entities) ───────────────────────────────────
        struct InterpolationState {
            std::deque<TransformSnapshot> snapshots;
            double playback_t = 0.0;
            bool   buffering  = true;
        };
        std::unordered_map<NetID, InterpolationState> interpolation_states;

        // ── World geometry ────────────────────────────────────────────────────
        std::vector<WorldObject> world_objects_;
        bool world_state_received_ = false;

        // ── Client-side prediction ────────────────────────────────────────────

        /**
         * @brief One slot of the prediction history ring-buffer.
         *
         * Indexed by (sequence % kPredictionHistory). When a server UPDATE
         * arrives with input_ack=K, the reconciliation forward-simulates the
         * server's confirmed position from K to the current frame by replaying
         * the stored inputs, then drives pred toward the result via the spring.
         */
        struct PredictionFrame {
            uint32_t   sequence;
            InputState input;
            float      x, y; ///< Predicted position after input + collision resolution.
        };

        static constexpr int kPredictionHistory = 64;
        PredictionFrame prediction_history_[kPredictionHistory] = {};

        Vector2  predicted_position_ = {};
        uint32_t last_server_tick_   = 0;
        uint32_t last_input_ack_     = 0;

        /// Base frames prepended to the RTT-derived prediction offset (see UPDATE handler).
        static constexpr int kPredictionBuffer = 4;

        float pred_x_ = 0.0f, pred_y_ = 0.0f;

        /**
         * @brief Server-authoritative position forward-extrapolated to the current frame.
         *
         * Set each time a local-player UPDATE is received. The spring correction
         * drives pred toward this value over subsequent ticks.
         */
        Vector2 server_reconcile_pos_ = {};
        bool    has_server_reconcile_ = false;

        // ── Correction parameters ─────────────────────────────────────────────
        static constexpr float kCorrectNoneThreshold = 5.0f;    ///< px — below this, suppress correction.
        static constexpr float kCorrectSnapThreshold = 1500.0f; ///< px — above this, hard-teleport.

        /// Spring stiffness. Correction per frame ≈ K * error * dt.
        /// For stable convergence without overshoot: K * dt < 1 (i.e. K < 60 at 60 Hz).
        float correction_K_ = 60.0f;

        /// Damping coefficient in the exponential term exp(-error * W).
        /// Controls how aggressively the correction tapers near the target.
        float correction_W_ = 21.0f;

        /// Debug: upward drift applied to pred while LEFT is held, in px/frame.
        /// server_reconcile_pos_ is unaffected; the spring must counteract the drift.
        float simulated_error_px_ = 0.0f;

    public:
        NetworkManager();
        ~NetworkManager();

        void _ready() override;
        void _process(double delta) override;
        void _physics_process(double delta) override;
        void _exit_tree() override;

        void   set_simulated_packet_drop_chance(double p_chance);
        double get_simulated_packet_drop_chance() const;

        void  set_correction_K(float k) { correction_K_ = k; }
        float get_correction_K() const  { return correction_K_; }

        void  set_correction_W(float w) { correction_W_ = w; }
        float get_correction_W() const  { return correction_W_; }

        void  set_simulated_error_px(float px) { simulated_error_px_ = px; }
        float get_simulated_error_px() const   { return simulated_error_px_; }

        uint32_t get_current_rtt() const;
        uint32_t get_smoothed_rtt_ms() const;
        int64_t  get_clock_offset_ms() const;
        int64_t  get_connection_state() const;
        bool     is_connected() const;

    protected:
        void set_connection_state(ConnectionState new_state);
        static void _bind_methods();
    };

} // namespace godot

#endif // GODOTPP_NET_MANAGER_H
