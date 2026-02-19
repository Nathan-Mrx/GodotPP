#ifndef GODOTPP_NET_PROTOCOL_H
#define GODOTPP_NET_PROTOCOL_H

#include <cstdint>

using NetID = uint32_t;
using TypeID = uint32_t;

enum class PacketType : uint8_t
{
    SPAWN = 1,
    HELLO = 2
};

#pragma pack(push, 1)
struct SpawnPacket
{
    PacketType type; // 8 bits
    NetID netID; // 32 bits
    TypeID typeID; // 32 bits
    int16_t x; // 16 bits
    int16_t y; // 16 bits
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HelloPacket
{
    PacketType type; // 8 bits
    int16_t x; // 16 bits
    int16_t y; // 16 bits
};
#pragma pack(pop)

#endif //GODOTPP_NET_PROTOCOL_H