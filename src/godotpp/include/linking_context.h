/**
 * @file linking_context.h
 * @brief Maps server-assigned NetIDs to local Godot Node instances.
 *
 * The LinkingContext is the client-side registry that bridges the gap
 * between the server's authoritative entity IDs and the Godot scene tree.
 * It owns two bidirectional maps (NetID ↔ Node*) and a factory table
 * keyed by TypeID so that new entities can be instantiated on demand
 * when a SpawnPacket arrives.
 */

#ifndef GODOTPP_LINKING_CONTEXT_H
#define GODOTPP_LINKING_CONTEXT_H

#include <unordered_map>
#include <functional>

#include "../../shared/include/net_protocol.h"

#include <godot_cpp/classes/node.hpp>

using namespace godot;

/**
 * @class LinkingContext
 * @brief Bidirectional NetID ↔ Node* registry with factory-based spawning.
 *
 * Typical lifecycle:
 * 1. `register_type()` is called once per entity type during `_ready()`.
 * 2. When a SpawnPacket arrives, `spawn_network_object()` creates the node.
 * 3. During gameplay, `get_node()` resolves NetIDs to scene-tree nodes.
 * 4. When a DespawnPacket arrives, `despawn_network_object()` frees the node.
 */
class LinkingContext {
public:
    LinkingContext();
    ~LinkingContext();

private:
    /** @brief Forward lookup: server NetID → local Godot node pointer. */
    std::unordered_map<NetID, Node*> network_to_local;

    /** @brief Reverse lookup: local Godot node pointer → server NetID. */
    std::unordered_map<Node*, NetID> local_to_network;

    /**
     * @brief Factory table: TypeID → lambda that instantiates the correct PackedScene.
     *
     * Each factory returns a newly allocated Node* from
     * `PackedScene::instantiate()`.  Ownership is transferred to the
     * caller (typically `NetworkManager` adds it to the scene tree).
     */
    std::unordered_map<TypeID, std::function<Node*()>> types_factories;

public:
    /**
     * @brief Registers a factory function for a given entity type.
     * @param type_id  Unique type identifier (must match the server's TypeComp).
     * @param factory  Callable that returns a freshly instantiated Node*.
     */
    void register_type(TypeID type_id, std::function<Node*()> factory);

    /**
     * @brief Creates a new node for the given NetID using the registered factory.
     * @param net_id   Server-assigned network identifier.
     * @param type_id  Type key used to look up the factory.
     * @return Pointer to the newly created node, or the existing node if
     *         `net_id` was already registered, or `nullptr` if `type_id`
     *         has no registered factory.
     */
    Node* spawn_network_object(NetID net_id, TypeID type_id);

    /**
     * @brief Resolves a NetID to its local Node pointer.
     * @param net_id  Network identifier to look up.
     * @return The corresponding Node*, or `nullptr` if not found.
     */
    Node* get_node(NetID net_id);

    /**
     * @brief Removes a networked object from both maps and queues it for deletion.
     * @param net_id  Network identifier of the entity to despawn.
     */
    void despawn_network_object(NetID net_id);
};

#endif //GODOTPP_LINKING_CONTEXT_H