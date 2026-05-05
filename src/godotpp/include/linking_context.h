/**
 * @file linking_context.h
 * @brief Bidirectional NetID ↔ Node* registry with factory-based spawning.
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
 * @brief Maps server-assigned NetIDs to scene-tree Node pointers.
 *
 * Maintains two unordered_maps (NetID→Node* and Node*→NetID) plus a
 * TypeID-keyed factory table. Factories must return a freshly instantiated
 * Node* whose ownership passes to the caller (NetworkManager adds it to
 * the scene tree via add_child).
 */
class LinkingContext {
public:
    LinkingContext();
    ~LinkingContext();

private:
    std::unordered_map<NetID, Node*>                    network_to_local;
    std::unordered_map<Node*, NetID>                    local_to_network;
    std::unordered_map<TypeID, std::function<Node*()>>  types_factories;

public:
    void  register_type(TypeID type_id, std::function<Node*()> factory);

    /// Instantiates a node via the registered factory and records the NetID↔Node*
    /// mapping. Returns the existing node if net_id is already registered, or
    /// nullptr if type_id has no factory.
    Node* spawn_network_object(NetID net_id, TypeID type_id);

    /// Returns nullptr if net_id is not registered.
    Node* get_node(NetID net_id);

    void  despawn_network_object(NetID net_id);
};

#endif // GODOTPP_LINKING_CONTEXT_H
