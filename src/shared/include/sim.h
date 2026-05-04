/**
 * @file sim.h
 * @brief Authoritative simulation step shared by server and client prediction.
 *
 * Both `server/src/main.cpp` and the client-side prediction code include this
 * header and call `sim::simulate_step()`.  Identical code guarantees identical
 * results from identical inputs - a hard requirement for rollback reconciliation.
 *
 * Keep this file free of engine-specific dependencies (no Godot, no EnTT).
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "net_protocol.h"
#include "world_packet.h"

namespace sim {

// ─────────────────────────────────────────────────────────────────────────────
//  Physics constants  (must stay in sync with player.tscn RectangleShape2D)
// ─────────────────────────────────────────────────────────────────────────────

constexpr float PLAYER_HW  = 64.0f;        ///< Player half-width  (128 / 2).
constexpr float PLAYER_HH  = 64.0f;        ///< Player half-height (128 / 2).
constexpr float MOVE_SPEED = 5.0f;         ///< Pixels per tick at 60 Hz.
constexpr float FIXED_DT   = 1.0f / 60.0f; ///< Physics tick duration in seconds.

// ─────────────────────────────────────────────────────────────────────────────
//  Collision resolution
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Push a player AABB out of an Oriented Bounding Box (OBB).
 *
 * Works for any rotation, including zero (degenerate AABB vs AABB case).
 * Uses the Separating Axis Theorem: the player's AABB half-extents are
 * projected onto the OBB's local axes to find the minimum-overlap axis,
 * then the response is rotated back to world space.
 *
 * @param cx,cy   Player center (modified in place).
 * @param phw,phh Player half-extents.
 * @param rx,ry   OBB center.
 * @param rrot    OBB rotation in radians.
 * @param rhw,rhh OBB half-extents in its local frame.
 * @return true if a collision was found and resolved.
 */
inline bool resolve_aabb_vs_obb(float& cx, float& cy, float phw, float phh,
                                 float rx,  float ry,  float rrot,
                                 float rhw, float rhh)
{
    const float c = std::cos(rrot), s = std::sin(rrot);
    const float dx = cx - rx, dy = cy - ry;

    // Transform player center into OBB local space.
    const float lx = dx * c + dy * s;
    const float ly = -dx * s + dy * c;

    // Project player AABB half-extents onto the OBB axes.
    const float ca = std::abs(c), sa = std::abs(s);
    const float proj_hw = phw * ca + phh * sa;
    const float proj_hh = phw * sa + phh * ca;

    const float ox = (proj_hw + rhw) - std::abs(lx);
    const float oy = (proj_hh + rhh) - std::abs(ly);
    if (ox <= 0.0f || oy <= 0.0f) return false;

    // Minimum overlap axis gives the push direction in OBB local space.
    float push_lx = 0.0f, push_ly = 0.0f;
    if (ox < oy) push_lx = (lx >= 0.0f ? ox : -ox);
    else         push_ly = (ly >= 0.0f ? oy : -oy);

    // Rotate push back to world space.
    cx += push_lx * c - push_ly * s;
    cy += push_lx * s + push_ly * c;
    return true;
}

/**
 * @brief Push a player AABB out of a static circle obstacle.
 *
 * @param cx,cy   Player center (modified in place).
 * @param phw,phh Player half-extents.
 * @param ox,oy   Circle center.
 * @param r       Circle radius.
 * @return true if a collision was found and resolved.
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
    const float pen  = r - dist;
    cx -= (dx / dist) * pen;
    cy -= (dy / dist) * pen;
    return true;
}

/** @brief Resolve a player AABB against the full static world geometry. */
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
//  Input application
// ─────────────────────────────────────────────────────────────────────────────

/** @brief Apply one InputState to a position (no collision check). */
inline void apply_input(float& x, float& y, const InputState& input)
{
    if (input.keys & InputFlags::UP)    y -= MOVE_SPEED;
    if (input.keys & InputFlags::DOWN)  y += MOVE_SPEED;
    if (input.keys & InputFlags::LEFT)  x -= MOVE_SPEED;
    if (input.keys & InputFlags::RIGHT) x += MOVE_SPEED;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Authoritative simulation step
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief One complete physics step: apply input then resolve collisions.
 *
 * This is the single source of truth for movement simulation.
 * The server calls it inside its 60 Hz tick.
 * The client prediction code calls it to advance the local predicted state.
 * The client rollback code calls it repeatedly to re-simulate from a
 * server-confirmed snapshot to the current frame.
 *
 * @param x,y   Position to advance (modified in place).
 * @param input Input state for this tick.
 * @param world Static world geometry for collision resolution.
 */
inline void simulate_step(float& x, float& y,
                           const InputState& input,
                           const std::vector<WorldObject>& world)
{
    apply_input(x, y, input);
    resolve_world_collisions(x, y, world);
}

} // namespace sim
