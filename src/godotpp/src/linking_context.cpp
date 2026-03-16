/**
 * @file linking_context.cpp
 * @brief Implementation of the client-side NetID ↔ Node* registry.
 *
 * Manages the mapping between server-assigned network IDs and local
 * Godot scene-tree nodes.  Handles spawning via factory functions,
 * lookup, and despawning with full cleanup of both maps.
 */

#include "linking_context.h"

#include "net_manager.h"

LinkingContext::LinkingContext() {}

LinkingContext::~LinkingContext() {}

/**
 * @brief Registers a factory callable for the given type ID.
 *
 * Must be called before any SpawnPacket referencing this type arrives.
 */
void LinkingContext::register_type(TypeID type_id, std::function<Node*()> factory)
{
    UtilityFunctions::print("[CLIENT][LinkingContext] Registered factory for TypeID=", type_id);
    types_factories[type_id] = factory;
}

/**
 * @brief Instantiates (or returns) a node for the given NetID.
 *
 * Three possible outcomes:
 * 1. NetID already known → returns existing node (no-op).
 * 2. TypeID has no factory → logs an error and returns nullptr.
 * 3. Success → creates the node, registers both maps, names it
 *    `NetNode_<id>`, and returns it.
 */
Node* LinkingContext::spawn_network_object(NetID net_id, TypeID type_id)
{
    // --- Guard: already exists ---
    if (network_to_local.find(net_id) != network_to_local.end())
    {
        UtilityFunctions::print("[CLIENT][LinkingContext] Spawn skipped — NetID=", net_id, " already mapped to an existing node");
        return network_to_local[net_id];
    }

    // --- Guard: unknown type ---
    if (types_factories.find(type_id) == types_factories.end())
    {
        UtilityFunctions::print("[CLIENT][LinkingContext] ERROR: No factory registered for TypeID=", type_id, " — cannot spawn NetID=", net_id);
        return nullptr;
    }

    // --- Instantiate & register ---
    Node* new_node = types_factories[type_id]();
    new_node->set_name("NetNode_" + String::num_int64(net_id));

    network_to_local[net_id] = new_node;
    local_to_network[new_node] = net_id;

    UtilityFunctions::print("[CLIENT][LinkingContext] Spawned new node '", new_node->get_name(),
                            "' — NetID=", net_id, " TypeID=", type_id,
                            " (total tracked: ", (int64_t)network_to_local.size(), ")");

    return new_node;
}

/**
 * @brief Resolves a NetID to a local Node pointer.
 *
 * Returns nullptr and logs a warning if the ID is unknown.
 */
Node* LinkingContext::get_node(NetID net_id)
{
    if (network_to_local.find(net_id) != network_to_local.end())
    {
        return network_to_local[net_id];
    }
    UtilityFunctions::print("[CLIENT][LinkingContext] WARNING: get_node() — NetID=", net_id, " not found in registry");
    return nullptr;
}

/**
 * @brief Despawns a networked object: queues the node for deletion
 *        and removes it from both lookup maps.
 */
void LinkingContext::despawn_network_object(NetID net_id)
{
    if (network_to_local.find(net_id) != network_to_local.end())
    {
        Node* node = network_to_local[net_id];
        if (node)
        {
            node->queue_free();
        }

        local_to_network.erase(node);
        network_to_local.erase(net_id);

        UtilityFunctions::print("[CLIENT][LinkingContext] Despawned NetID=", net_id,
                                " (remaining tracked: ", (int64_t)network_to_local.size(), ")");
    }
    else
    {
        UtilityFunctions::print("[CLIENT][LinkingContext] WARNING: despawn requested for unknown NetID=", net_id, " — ignoring");
    }
}
