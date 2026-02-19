#include "linking_context.h"

LinkingContext::LinkingContext() {}

LinkingContext::~LinkingContext() {}

void LinkingContext::register_type(TypeID type_id, std::function<Node*()> factory)
{
    UtilityFunctions::print("[CLIENT] Registering new type : ", type_id);
    types_factories[type_id] = factory;
}

Node* LinkingContext::spawn_network_object(NetID net_id, TypeID type_id)
{
    // Check if object already exist
    if (network_to_local.find(net_id) != network_to_local.end())
    {
        UtilityFunctions::print("[CLIENT] Object already exists");
        return network_to_local[net_id];
    }

    // Check if we know the type
    if (types_factories.find(type_id) == types_factories.end())
    {
        UtilityFunctions::print("[CLIENT] Object type does not exists : ", type_id);
        return nullptr;
    }

    Node* new_node = types_factories[type_id]();
    new_node->set_name("NetNode_" + String::num_int64(net_id));

    network_to_local[net_id] = new_node;
    local_to_network[new_node] = net_id;

    return new_node;
}

Node* LinkingContext::get_node(NetID net_id)
{
    if (network_to_local.find(net_id) != network_to_local.end())
    {
        return network_to_local[net_id];
    }
    return nullptr;
}
