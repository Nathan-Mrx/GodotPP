#pragma once

#include "net_protocol.h"
#include "stream_reader.h"
#include "stream_writer.h"

#include <optional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  WorldObject
// ─────────────────────────────────────────────────────────────────────────────

enum class WorldObjectShape : uint8_t
{
    RECT   = 0, ///< param_a = half-width, param_b = half-height.
    CIRCLE = 1, ///< param_a = radius, param_b unused.
};

/**
 * @brief One static collision body in the level.
 *
 * Transmitted inside WorldStatePacket. The client instantiates a StaticBody2D
 * per entry; the server feeds them into sim::resolve_world_collisions().
 */
struct WorldObject
{
    WorldObjectShape shape;
    float x, y;
    float rotation; ///< Radians; applied to the OBB before collision tests.
    float param_a;
    float param_b;

    void serialize(StreamWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(shape));
        w.write_float(x);
        w.write_float(y);
        w.write_float(rotation);
        w.write_float(param_a);
        w.write_float(param_b);
    }

    static std::optional<WorldObject> deserialize(StreamReader& r)
    {
        const auto s   = r.read<uint8_t>();
        const auto px  = r.read_float();
        const auto py  = r.read_float();
        const auto rot = r.read_float();
        const auto pa  = r.read_float();
        const auto pb  = r.read_float();

        if (!s || !px || !py || !rot || !pa || !pb)
            return std::nullopt;

        return WorldObject{
            static_cast<WorldObjectShape>(*s),
            *px, *py, *rot, *pa, *pb
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  WorldStatePacket
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Full static level geometry, server → client on HSK completion.
 *
 * Wire format:
 *   [PacketType::WORLD_STATE (1 B)]
 *   [uint32 object count]
 *   [WorldObject × count]
 *   [StreamWriter bit-buffer + uint16 footer]
 *
 * Sent once per client immediately after HSK_ACK. The server schedules up to
 * 3 UDP retransmits at 200 ms intervals for loss resilience; the client
 * ignores duplicates once world_state_received_ is set.
 */
struct WorldStatePacket
{
    std::vector<WorldObject> objects;

    [[nodiscard]] std::vector<uint8_t> to_bytes() const
    {
        StreamWriter w;
        w.write<uint8_t>(static_cast<uint8_t>(PacketType::WORLD_STATE));
        w.write_array(objects, [](StreamWriter& sw, const WorldObject& obj) {
            sw.write_struct(obj);
        });
        return w.finish();
    }

    static std::optional<WorldStatePacket> from_bytes(const uint8_t* buf, size_t len)
    {
        if (len < 1) return std::nullopt;
        try {
            StreamReader r(buf + 1, len - 1);
            auto objects = r.read_array<WorldObject>([](StreamReader& sr) {
                return sr.read_struct<WorldObject>();
            });
            if (!objects) return std::nullopt;
            return WorldStatePacket{std::move(*objects)};
        } catch (...) {
            return std::nullopt;
        }
    }
};
