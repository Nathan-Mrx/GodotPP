/**
 * @file linking_context.cpp
 * @brief LinkingContext implementation — see linking_context.h.
 */

#include "linking_context.h"
#include "net_manager.h"

LinkingContext::LinkingContext()  {}
LinkingContext::~LinkingContext() {}

void LinkingContext::register_type(TypeID type_id, std::function<Node*()> factory)
{
    UtilityFunctions::print("[CLIENT][LinkingContext] Registered factory for TypeID=", type_id);
    types_factories[type_id] = factory;
}

/// Returns the existing node if net_id is already registered.
/// Returns nullptr if type_id has no factory.
Node* LinkingContext::spawn_network_object(NetID net_id, TypeID type_id)
{
    auto existing = network_to_local.find(net_id);
    if (existing != network_to_local.end()) {
        UtilityFunctions::print("[CLIENT][LinkingContext] Spawn skipped - NetID=", net_id, " already registered");
        return existing->second;
    }

    auto factory_it = types_factories.find(type_id);
    if (factory_it == types_factories.end()) {
        UtilityFunctions::print("[CLIENT][LinkingContext] ERROR: No factory for TypeID=", type_id, " (NetID=", net_id, ")");
        return nullptr;
    }

    Node* node = factory_it->second();
    node->set_name("NetNode_" + String::num_int64(net_id));
    network_to_local[net_id] = node;
    local_to_network[node]   = net_id;

    UtilityFunctions::print("[CLIENT][LinkingContext] Spawned '", node->get_name(),
                            "' NetID=", net_id, " TypeID=", type_id,
                            " (tracked: ", (int64_t)network_to_local.size(), ")");
    return node;
}

Node* LinkingContext::get_node(NetID net_id)
{
    auto it = network_to_local.find(net_id);
    if (it != network_to_local.end()) return it->second;
    UtilityFunctions::print("[CLIENT][LinkingContext] WARNING: get_node() called for unknown NetID=", net_id);
    return nullptr;
}

void LinkingContext::despawn_network_object(NetID net_id)
{
    auto it = network_to_local.find(net_id);
    if (it == network_to_local.end()) {
        UtilityFunctions::print("[CLIENT][LinkingContext] WARNING: despawn for unknown NetID=", net_id);
        return;
    }

    Node* node = it->second;
    if (node) node->queue_free();

    local_to_network.erase(node);
    network_to_local.erase(it);

    UtilityFunctions::print("[CLIENT][LinkingContext] Despawned NetID=", net_id,
                            " (tracked: ", (int64_t)network_to_local.size(), ")");
}
