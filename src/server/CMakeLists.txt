add_executable(GodotPPServer
    src/main.cpp
    ../shared/include/net_protocol.h
    ../shared/include/stream_writer.h
    ../shared/include/stream_reader.h
    ../shared/include/world_packet.h
)

target_link_libraries(GodotPPServer PRIVATE
        snl
        EnTT::EnTT
        nlohmann_json::nlohmann_json)