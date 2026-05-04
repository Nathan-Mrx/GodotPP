include(FetchContent)

add_subdirectory(entt)
add_subdirectory(godot-cpp)
add_subdirectory(snl)

FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)