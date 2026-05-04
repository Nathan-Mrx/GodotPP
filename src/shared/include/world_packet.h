#pragma once

#include "net_protocol.h"
#include "stream_reader.h"
#include "stream_writer.h"

#include <optional>
#include <vector>

// ─────────────────────────────────────────────────────────
//  World object shapes
// ─────────────────────────────────────────────────────────

enum class WorldObjectShape : uint8_t
{
    RECT   = 0, ///< Axis-aligned rectangle; param_a = half-width, param_b = half-height.
    CIRCLE = 1, ///< Circle; param_a = radius, param_b unused.
};

// ─────────────────────────────────────────────────────────
//  WorldObject  (one static physics body)
// ─────────────────────────────────────────────────────────

/**
 * @brief Describes a single static collision body in the level.
 *
 * Sent inside a WorldStatePacket from the server to every client
 * that completes the HSK handshake.  The client instantiates a
 * StaticBody2D for each entry.
 */
struct WorldObject
{
    WorldObjectShape shape;  ///< Collision shape type.
    float x;                 ///< World-space center X.
    float y;                 ///< World-space center Y.
    float rotation;          ///< Rotation in radians.
    float param_a;           ///< RECT: half-width;  CIRCLE: radius.
    float param_b;           ///< RECT: half-height; CIRCLE: unused (send 0).

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
        const auto s    = r.read<uint8_t>();
        const auto px   = r.read_float();
        const auto py   = r.read_float();
        const auto rot  = r.read_float();
        const auto pa   = r.read_float();
        const auto pb   = r.read_float();

        if (!s || !px || !py || !rot || !pa || !pb)
            return std::nullopt;

        return WorldObject{
            static_cast<WorldObjectShape>(*s),
            *px, *py, *rot, *pa, *pb
        };
    }
};

// ─────────────────────────────────────────────────────────
//  WorldStatePacket
// ─────────────────────────────────────────────────────────

/**
 * @brief Carries the full static level geometry from server to client.
 *
 * Wire format (produced by to_bytes):
 *   [PacketType::WORLD_STATE (1 B)]
 *   [uint32 object count]
 *   [WorldObject × count]
 *   [StreamWriter bit-buffer]
 *   [bit-byte count footer (uint16)]
 *
 * Sent once per client immediately after the HSK handshake completes.
 * The client instantiates a StaticBody2D + CollisionShape2D for every entry.
 */
struct WorldStatePacket
{
    std::vector<WorldObject> objects;

    /** @brief Serializes to a ready-to-send byte vector. */
    [[nodiscard]] std::vector<uint8_t> to_bytes() const
    {
        StreamWriter w;
        w.write<uint8_t>(static_cast<uint8_t>(PacketType::WORLD_STATE));
        w.write_array(objects, [](StreamWriter& sw, const WorldObject& obj) {
            sw.write_struct(obj);
        });
        return w.finish();
    }

    /**
     * @brief Parses a received buffer (including the PacketType byte at offset 0).
     * @return nullopt on any parse error.
     */
    static std::optional<WorldStatePacket> from_bytes(const uint8_t* buf, size_t len)
    {
        if (len < 1)
            return std::nullopt;

        try {
            // Skip the leading PacketType byte; StreamReader owns the rest.
            StreamReader r(buf + 1, len - 1);
            auto objects = r.read_array<WorldObject>([](StreamReader& sr) {
                return sr.read_struct<WorldObject>();
            });
            if (!objects)
                return std::nullopt;

            return WorldStatePacket{std::move(*objects)};
        }
        catch (...) {
            return std::nullopt;
        }
    }
};
