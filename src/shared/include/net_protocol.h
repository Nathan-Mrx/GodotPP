/**
 * @file net_protocol.h
 * @brief UDP wire protocol shared between server and GDExtension client.
 *
 * All structs are #pragma pack(push, 1) — no padding, platform-independent layout.
 * No engine-specific dependencies; include freely in both translation units.
 */

#ifndef GODOTPP_NET_PROTOCOL_H
#define GODOTPP_NET_PROTOCOL_H

#include <cstdint>

using NetID  = uint32_t; ///< Server-assigned entity identifier.
using TypeID = uint32_t; ///< Client factory key; maps to a PackedScene.

/// Discriminator at byte 0 of every datagram.
enum class PacketType : uint8_t
{
    SPAWN         = 1,
    HELLO         = 2,
    INPUT         = 3,
    UPDATE        = 4,
    DISCONNECT    = 5,
    DESPAWN       = 6,
    PING_REQUEST  = 7,
    PING_RESPONSE = 8,
    HELLO_ACK     = 9,
    HSK           = 10,
    HSK_ACK       = 11,
    WORLD_STATE   = 12,
};

enum InputFlags : uint8_t
{
    NONE   = 0,
    UP     = 1 << 0,
    DOWN   = 1 << 1,
    LEFT   = 1 << 2,
    RIGHT  = 1 << 3,
    ACTION = 1 << 4,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Packet structs
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
/// Server → Client: instantiate a new entity. Sent to all peers on join, and
/// individually to a joining client for every already-connected entity.
struct SpawnPacket {
    PacketType type;
    NetID      netID;
    TypeID     typeID;
    int16_t    x;
    int16_t    y;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HelloPacket {
    PacketType type;
    int16_t    x; ///< Requested spawn X.
    int16_t    y;
};
#pragma pack(pop)

#pragma pack(push, 1)
/// One frame of player input, packed inside the InputPacket ring-buffer.
struct InputState {
    uint8_t keys;  ///< OR'd InputFlags.
    float   aim_x;
    float   aim_y;
};
#pragma pack(pop)

#pragma pack(push, 1)
/// Client → Server: current input frame plus 19 historical frames for
/// loss recovery. history[0] = sequence N (newest), history[19] = oldest.
/// The server processes only frames beyond its last_sequence_received.
struct InputPacket {
    PacketType type;
    uint32_t   sequence;
    InputState history[20];
};
#pragma pack(pop)

#pragma pack(push, 1)
/// Server → Client: authoritative entity position, broadcast every 60 Hz tick.
/// input_ack echoes the last client sequence processed for this entity —
/// the client replays [input_ack+1, current] from its history to forward-
/// extrapolate the server position for reconciliation.
struct UpdatePacket {
    PacketType type;
    NetID      netID;
    int16_t    x;
    int16_t    y;
    uint32_t   server_tick;
    uint32_t   input_ack;
};
#pragma pack(pop)

#pragma pack(push, 1)
/// Client → Server: triggers entity destruction and DESPAWN broadcast.
/// Client must close its socket after sending.
struct DisconnectPacket {
    PacketType type;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DespawnPacket {
    PacketType type;
    NetID      netID;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct PingRequestPacket {
    PacketType type;
    uint32_t   id;
    uint64_t   t0; ///< Client send time (ms); echoed back for RTT = t_recv - t0.
};
#pragma pack(pop)

#pragma pack(push, 1)
/// Server → Client: echo of PingRequestPacket. t1 enables clock-offset estimation
/// via Cristian's algorithm: offset = t1 + RTT/2 - t_receive.
struct PingResponsePacket {
    PacketType type;
    uint32_t   id;
    uint64_t   t0; ///< Echoed client timestamp.
    uint64_t   t1; ///< Server timestamp at response time (ms since epoch).
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HelloAckPacket {
    PacketType type;
    NetID      assigned_net_id;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HskPacket {
    PacketType type;
};
#pragma pack(pop)

#pragma pack(push, 1)
/// Server → Client: handshake complete. Followed immediately by a SPAWN storm:
/// one SpawnPacket for the new client's own entity, then one per already-connected
/// entity, plus the full WORLD_STATE geometry packet.
struct HskAckPacket {
    PacketType type;
};
#pragma pack(pop)

#endif // GODOTPP_NET_PROTOCOL_H
