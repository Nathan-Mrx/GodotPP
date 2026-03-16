/**
 * @file register_types.h
 * @brief GDExtension module entry-point declarations.
 *
 * Declares the initializer and terminator functions called by the
 * Godot engine when the GodotPP shared library is loaded and unloaded.
 * The corresponding implementations live in `register_types.cpp`.
 */

#ifndef REGISTER_TYPES_H
#define REGISTER_TYPES_H

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

/**
 * @brief Called by Godot during module initialization.
 * @param p_level  Current initialization phase; we only register classes
 *                 at `MODULE_INITIALIZATION_LEVEL_SCENE`.
 *
 * Registers GDExample and NetworkManager into the ClassDB so they
 * appear as selectable node types in the Godot editor.
 */
void initialize_module(ModuleInitializationLevel p_level);

/**
 * @brief Called by Godot during module teardown.
 * @param p_level  Current teardown phase.
 *
 * Currently a no-op — Godot handles class unregistration automatically.
 */
void uninitialize_module(ModuleInitializationLevel p_level);

#endif // REGISTER_TYPES_H