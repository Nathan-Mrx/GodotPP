/**
 * @file net_protocol.h
 * @brief Shared network protocol definitions for GodotPP.
 *
 * Defines the binary wire format used for UDP communication between
 * the authoritative server and the Godot GDExtension clients.
 * Every packet struct is `#pragma pack(push, 1)` to guarantee a
 * compact, platform-independent memory layout with no padding.
 *
 * @note Both the server (`server/src/main.cpp`) and the client
 *       (`godotpp/`) include this header - keep it free of
 *       engine-specific dependencies.
 */

#ifndef GODOTPP_NET_PROTOCOL_H
#define GODOTPP_NET_PROTOCOL_H

#include <cstdint>

/** @brief Unique identifier assigned by the server to every networked entity. */
using NetID = uint32_t;

/** @brief Identifier for a replicated entity type (maps to a factory on the client). */
using TypeID = uint32_t;

/**
 * @brief Discriminator byte placed at offset 0 of every UDP datagram.
 *
 * The receiver reads `buffer[0]` and casts it to a PacketType to decide
 * which struct to reinterpret the payload as.
 */
enum class PacketType : uint8_t
{
    SPAWN         = 1,  ///< Server → Client : A new entity was created.
    HELLO         = 2,  ///< Client → Server : Initial handshake with desired spawn position.
    INPUT         = 3,  ///< Client → Server : Player input state with redundancy ring-buffer.
    UPDATE        = 4,  ///< Server → Client : Authoritative position update for an entity.
    DISCONNECT    = 5,  ///< Client → Server : Graceful disconnection notification.
    DESPAWN       = 6,  ///< Server → Client : An entity was removed from the world.
    PING_REQUEST  = 7,  ///< Client → Server : RTT measurement probe.
    PING_RESPONSE = 8   ///< Server → Client : Echo of the probe with server timestamp.
};

/**
 * @brief Bitmask flags representing discrete player inputs.
 *
 * Multiple flags can be ORed together in a single `uint8_t`.
 */
enum InputFlags : uint8_t
{
    NONE   = 0,       ///< No keys pressed.
    UP     = 1 << 0,  ///< Up / W key.
    DOWN   = 1 << 1,  ///< Down / S key.
    LEFT   = 1 << 2,  ///< Left / A key.
    RIGHT  = 1 << 3,  ///< Right / D key.
    ACTION = 1 << 4   ///< Primary action / Space key.
};

// ─────────────────────────────────────────────────────────
//  Packet structures (all packed - no padding)
// ─────────────────────────────────────────────────────────

/**
 * @brief Server → Client : Tells a client to instantiate a new networked entity.
 *
 * Sent to every connected client when a new player joins, and also
 * sent individually to a joining player for every entity that already exists.
 */
#pragma pack(push, 1)
struct SpawnPacket {
    PacketType type;   ///< Always `PacketType::SPAWN`.
    NetID      netID;  ///< Server-assigned unique network ID.
    TypeID     typeID; ///< Factory key the client uses to instantiate the correct scene.
    int16_t    x;      ///< Initial X position (world units).
    int16_t    y;      ///< Initial Y position (world units).
};
#pragma pack(pop)

/**
 * @brief Client → Server : First packet a client sends after opening its socket.
 *
 * Contains the desired spawn position. The server creates the player
 * entity and broadcasts a SpawnPacket to all clients.
 */
#pragma pack(push, 1)
struct HelloPacket {
    PacketType type; ///< Always `PacketType::HELLO`.
    int16_t    x;    ///< Requested spawn X.
    int16_t    y;    ///< Requested spawn Y.
};
#pragma pack(pop)

/**
 * @brief Snapshot of a single frame's player input.
 *
 * Packed inside the InputPacket ring-buffer so the server can
 * recover missed frames even when individual UDP datagrams are lost.
 */
#pragma pack(push, 1)
struct InputState {
    uint8_t keys;  ///< Bitfield of `InputFlags`.
    float   aim_x; ///< Mouse / aim X in viewport coordinates.
    float   aim_y; ///< Mouse / aim Y in viewport coordinates.
};
#pragma pack(pop)

/**
 * @brief Client → Server : Player input with a 20-frame redundancy ring-buffer.
 *
 * `history[0]` is the *current* frame (sequence N), `history[1]` is
 * frame N-1, etc.  The server processes only the frames it hasn't
 * seen yet (gap between its `last_sequence` and `sequence`).
 */
#pragma pack(push, 1)
struct InputPacket {
    PacketType type;        ///< Always `PacketType::INPUT`.
    uint32_t   sequence;    ///< Monotonically increasing input frame counter.
    InputState history[20]; ///< Ring-buffer: history[0] = newest, [19] = oldest.
};
#pragma pack(pop)

/**
 * @brief Server → Client : Authoritative position of a networked entity.
 *
 * Broadcast every server tick (~60 Hz) for every entity. The client
 * stores these in its interpolation buffer and renders smoothly
 * between two snapshots offset by `interpolation_delay_ms`.
 */
#pragma pack(push, 1)
struct UpdatePacket {
    PacketType type;  ///< Always `PacketType::UPDATE`.
    NetID      netID; ///< Target entity.
    int16_t    x;     ///< Authoritative X position.
    int16_t    y;     ///< Authoritative Y position.
};
#pragma pack(pop)

/**
 * @brief Client → Server : Graceful disconnection.
 *
 * After sending this the client should close its socket.
 * The server destroys the entity and broadcasts a DespawnPacket.
 */
#pragma pack(push, 1)
struct DisconnectPacket {
    PacketType type; ///< Always `PacketType::DISCONNECT`.
};
#pragma pack(pop)

/**
 * @brief Server → Client : Tells clients to remove a networked entity.
 *
 * Sent to all remaining clients when a player disconnects.
 */
#pragma pack(push, 1)
struct DespawnPacket {
    PacketType type;  ///< Always `PacketType::DESPAWN`.
    NetID      netID; ///< Entity to remove.
};
#pragma pack(pop)

/**
 * @brief Client → Server : RTT measurement probe.
 *
 * The client records `t0` (its local time) before sending.
 * The server echoes `t0` back inside a PingResponsePacket.
 */
#pragma pack(push, 1)
struct PingRequestPacket {
    PacketType type; ///< Always `PacketType::PING_REQUEST`.
    uint32_t   id;   ///< Sequence number for correlating request/response.
    uint64_t   t0;   ///< Client-side timestamp at send time (ms).
};
#pragma pack(pop)

/**
 * @brief Server → Client : Echo of a PingRequestPacket with server timestamp.
 *
 * RTT = `t_receive - t0`.  The `t1` field can be used for clock-offset
 * estimation if one-way delay computation is needed later.
 */
#pragma pack(push, 1)
struct PingResponsePacket {
    PacketType type; ///< Always `PacketType::PING_RESPONSE`.
    uint32_t   id;   ///< Copied from the request.
    uint64_t   t0;   ///< Client timestamp, echoed back unchanged.
    uint64_t   t1;   ///< Server timestamp at the moment of response (ms since epoch).
};
#pragma pack(pop)

#endif //GODOTPP_NET_PROTOCOL_H