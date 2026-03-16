/**
 * @file gd_example.cpp
 * @brief Implementation of the GDExample demo node.
 *
 * Animates a Sprite2D in a Lissajous pattern to validate the
 * GDExtension build pipeline.  No networking involved.
 */

#include "gd_example.h"
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

/// No properties or signals to bind for this demo class.
void GDExample::_bind_methods() {
}

/**
 * @brief Constructs the GDExample and zeroes the elapsed-time accumulator.
 */
GDExample::GDExample() {
    time_passed = 0.0;
}

/**
 * @brief Default destructor — no dynamic resources to release.
 */
GDExample::~GDExample() {
}

/**
 * @brief Advances the Lissajous animation by @p delta seconds.
 *
 * The position follows:
 *   x = 10 + 10·sin(2t)
 *   y = 10 + 10·cos(1.5t)
 */
void GDExample::_process(double delta) {
    time_passed += delta;

    Vector2 new_position = Vector2(10.0 + (10.0 * sin(time_passed * 2.0)), 10.0 + (10.0 * cos(time_passed * 1.5)));

    set_position(new_position);
}