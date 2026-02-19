add_executable(GodotPPServer
    src/main.cpp
        ../shared/include/net_protocol.h
)

target_link_libraries(GodotPPServer PRIVATE
        snl
        EnTT::EnTT)