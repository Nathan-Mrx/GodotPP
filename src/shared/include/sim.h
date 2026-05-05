/**
 * @file sim.h
 * @brief Shared authoritative simulation kernel.
 *
 * Included by both the server tick loop and the client prediction path.
 * All callers must use sim::simulate_step() for movement — identical code
 * on both sides is the invariant that keeps prediction error near zero.
 *
 * No engine-specific dependencies (no Godot, no EnTT).
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "net_protocol.h"
#include "world_packet.h"

namespace sim {

// ─────────────────────────────────────────────────────────────────────────────
//  Constants  — must match player.tscn RectangleShape2D and server loop rate
// ─────────────────────────────────────────────────────────────────────────────

constexpr float PLAYER_HW  = 64.0f;
constexpr float PLAYER_HH  = 64.0f;
constexpr float MOVE_SPEED = 5.0f;         ///< px per tick at 60 Hz.
constexpr float FIXED_DT   = 1.0f / 60.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  Collision resolution
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Push a player AABB out of an Oriented Bounding Box (OBB).
 *
 * Projects the player's AABB half-extents onto the OBB's local axes (SAT),
 * finds the minimum-overlap axis, and rotates the MTV back to world space.
 * Handles rotation=0 as the degenerate AABB-vs-AABB case.
 *
 * @param cx,cy   Player center, modified in place.
 * @param phw,phh Player half-extents.
 * @param rx,ry   OBB world-space center.
 * @param rrot    OBB rotation (radians).
 * @param rhw,rhh OBB half-extents in its local frame.
 * @return true if penetrating and a response was applied.
 */
inline bool resolve_aabb_vs_obb(float& cx, float& cy, float phw, float phh,
                                 float rx,  float ry,  float rrot,
                                 float rhw, float rhh)
{
    const float c = std::cos(rrot), s = std::sin(rrot);
    const float dx = cx - rx, dy = cy - ry;

    const float lx = dx * c + dy * s;
    const float ly = -dx * s + dy * c;

    const float ca = std::abs(c), sa = std::abs(s);
    const float proj_hw = phw * ca + phh * sa;
    const float proj_hh = phw * sa + phh * ca;

    const float ox = (proj_hw + rhw) - std::abs(lx);
    const float oy = (proj_hh + rhh) - std::abs(ly);
    if (ox <= 0.0f || oy <= 0.0f) return false;

    float push_lx = 0.0f, push_ly = 0.0f;
    if (ox < oy) push_lx = (lx >= 0.0f ? ox : -ox);
    else         push_ly = (ly >= 0.0f ? oy : -oy);

    cx += push_lx * c - push_ly * s;
    cy += push_lx * s + push_ly * c;
    return true;
}

/**
 * @brief Push a player AABB out of a static circle.
 *
 * Clamps the circle center to the AABB to find the closest point, then
 * pushes along the circle-to-closest-point vector by the penetration depth.
 *
 * @param cx,cy   Player center, modified in place.
 * @param phw,phh Player half-extents.
 * @param ox,oy   Circle center.
 * @param r       Circle radius.
 * @return true if penetrating and a response was applied.
 */
inline bool resolve_aabb_vs_circle(float& cx, float& cy, float phw, float phh,
                                    float ox,  float oy,  float r)
{
    const float ncx = std::clamp(ox, cx - phw, cx + phw);
    const float ncy = std::clamp(oy, cy - phh, cy + phh);
    const float dx = ox - ncx, dy = oy - ncy;
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq >= r * r) return false;
    if (dist_sq < 0.0001f) { cx += r + phw; return true; }
    const float dist = std::sqrt(dist_sq);
    cx -= (dx / dist) * (r - dist);
    cy -= (dy / dist) * (r - dist);
    return true;
}

/** @brief Resolve the player AABB against all static world objects. */
inline void resolve_world_collisions(float& x, float& y,
                                      const std::vector<WorldObject>& world)
{
    for (const auto& obj : world) {
        if (obj.shape == WorldObjectShape::RECT)
            resolve_aabb_vs_obb(x, y, PLAYER_HW, PLAYER_HH,
                                obj.x, obj.y, obj.rotation, obj.param_a, obj.param_b);
        else
            resolve_aabb_vs_circle(x, y, PLAYER_HW, PLAYER_HH,
                                   obj.x, obj.y, obj.param_a);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Simulation step
// ─────────────────────────────────────────────────────────────────────────────

inline void apply_input(float& x, float& y, const InputState& input)
{
    if (input.keys & InputFlags::UP)    y -= MOVE_SPEED;
    if (input.keys & InputFlags::DOWN)  y += MOVE_SPEED;
    if (input.keys & InputFlags::LEFT)  x -= MOVE_SPEED;
    if (input.keys & InputFlags::RIGHT) x += MOVE_SPEED;
}

/**
 * @brief One authoritative simulation tick: apply input, resolve collisions.
 *
 * The server calls this once per 60 Hz tick. The client prediction path
 * calls it once per physics frame, and the reconciliation path replays it
 * over the history window [input_ack+1, current] to forward-extrapolate
 * the server's confirmed position.
 */
inline void simulate_step(float& x, float& y,
                           const InputState& input,
                           const std::vector<WorldObject>& world)
{
    apply_input(x, y, input);
    resolve_world_collisions(x, y, world);
}

} // namespace sim
