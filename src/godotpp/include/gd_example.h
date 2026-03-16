/**
 * @file gd_example.h
 * @brief Demo GDExtension node that animates a Sprite2D in a Lissajous pattern.
 *
 * GDExample is a minimal "hello world" node shipped with the GodotPP
 * template.  It moves its Sprite2D position every frame using sin/cos
 * to verify that the GDExtension build pipeline works correctly.
 */

#ifndef GDEXAMPLE_H
#define GDEXAMPLE_H

#include <godot_cpp/classes/sprite2d.hpp>

namespace godot {

    /**
     * @class GDExample
     * @brief Sprite2D subclass that oscillates in a Lissajous figure-eight pattern.
     *
     * Registered in `register_types.cpp` and available in the Godot editor
     * as a custom node type.
     */
    class GDExample : public Sprite2D {
        GDCLASS(GDExample, Sprite2D)

    private:
        double time_passed; ///< Accumulated elapsed time used for the oscillation formula.

    protected:
        /** @brief Binds methods/properties to the Godot ClassDB (none for this demo). */
        static void _bind_methods();

    public:
        /** @brief Initializes `time_passed` to zero. */
        GDExample();

        /** @brief Default destructor. */
        ~GDExample();

        /**
         * @brief Called every visual frame by the engine.
         * @param delta Time in seconds since the last `_process` call.
         *
         * Updates the Sprite2D position along a Lissajous curve.
         */
        void _process(double delta) override;
    };

} // namespace godot

#endif // GDEXAMPLE_H