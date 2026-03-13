#ifndef GODOTPP_NET_PROTOCOL_H
#define GODOTPP_NET_PROTOCOL_H

#include <cstdint>

using NetID = uint32_t;
using TypeID = uint32_t;

enum class PacketType : uint8_t
{
    SPAWN = 1,
    HELLO = 2,
    INPUT = 3,
    UPDATE = 4,
    DISCONNECT = 5,
    DESPAWN = 6,
    PING_REQUEST = 7,
    PING_RESPONSE = 8
};

enum InputFlags : uint8_t
{
    NONE   = 0,
    UP     = 1 << 0,
    DOWN   = 1 << 1,
    LEFT   = 1 << 2,
    RIGHT  = 1 << 3,
    ACTION = 1 << 4
};

#pragma pack(push, 1)
struct SpawnPacket {
    PacketType type;
    NetID netID;
    TypeID typeID;
    int16_t x;
    int16_t y;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HelloPacket {
    PacketType type;
    int16_t x;
    int16_t y;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct InputState {
    uint8_t keys;
    float aim_x;
    float aim_y;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct InputPacket {
    PacketType type;
    uint32_t sequence;
    InputState history[20];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct UpdatePacket {
    PacketType type;
    NetID netID;
    int16_t x;
    int16_t y;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DisconnectPacket {
    PacketType type;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DespawnPacket {
    PacketType type;
    NetID netID;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct PingRequestPacket {
    PacketType type;
    uint32_t id;
    uint64_t t0; // Timestamp client à l'envoi
};
#pragma pack(pop)

#pragma pack(push, 1)
struct PingResponsePacket {
    PacketType type;
    uint32_t id;
    uint64_t t0; // Recopié depuis la requête
    uint64_t t1; // Timestamp serveur au moment de la réponse
};
#pragma pack(pop)

#endif //GODOTPP_NET_PROTOCOL_H