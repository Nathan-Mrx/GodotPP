#ifndef GODOTPP_LINKING_CONTEXT_H
#define GODOTPP_LINKING_CONTEXT_H

#include <unordered_map>
#include <functional>

#include "../../shared/include/net_protocol.h"

#include <godot_cpp/classes/node.hpp>

using namespace godot;

class LinkingContext {
public:
    LinkingContext();
    ~LinkingContext();

private:
    std::unordered_map<NetID, Node*> network_to_local;

    std::unordered_map<Node*, NetID> local_to_network;

    std::unordered_map<TypeID, std::function<Node*()>> types_factories;

public:
    void register_type(TypeID type_id, std::function<Node*()> factory);

    Node* spawn_network_object(NetID net_id, TypeID type_id);

    Node* get_node(NetID net_id);
};

#endif //GODOTPP_LINKING_CONTEXT_H