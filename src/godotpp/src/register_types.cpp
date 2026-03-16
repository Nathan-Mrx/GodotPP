/**
 * @file register_types.cpp
 * @brief GDExtension module entry-point — registers custom nodes with Godot.
 *
 * This file is the bridge between the compiled shared library and the
 * Godot engine.  The exported C function `godotpp_library_init` is
 * declared in the `.gdextension` manifest and called by Godot at load
 * time.  It wires up `initialize_module` / `uninitialize_module` so
 * that our custom classes are available in the editor and at runtime.
 */

#include "register_types.h"
#include "gd_example.h"
#include "net_manager.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

/**
 * @brief Registers all GodotPP custom classes into the ClassDB.
 *
 * Only acts at `MODULE_INITIALIZATION_LEVEL_SCENE` — earlier levels
 * (core, servers) are skipped because our classes depend on the
 * scene tree being available.
 */
void initialize_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    ClassDB::register_class<GDExample>();
    ClassDB::register_class<NetworkManager>();
}

/**
 * @brief Module teardown callback (currently a no-op).
 *
 * Godot handles unregistration of classes automatically; this
 * function is provided for forward-compatibility if cleanup logic
 * is needed in the future.
 */
void uninitialize_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

/**
 * @brief C entry-point exported by the shared library.
 *
 * Called by Godot when the `.gdextension` file references this library.
 * Sets up the init/terminator callbacks and the minimum initialization
 * level required for our classes.
 *
 * @param p_get_proc_address  Godot function-pointer resolver.
 * @param p_library           Opaque handle to this extension library.
 * @param r_initialization    Output struct filled with our callbacks.
 * @return `true` on success.
 */
extern "C" {
    GDExtensionBool GDE_EXPORT godotpp_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
        godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

        init_obj.register_initializer(initialize_module);
        init_obj.register_terminator(uninitialize_module);
        init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

        return init_obj.init();
    }
}