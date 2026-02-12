# cmake/DownloadGodot.cmake
# Usage: cmake -P cmake/DownloadGodot.cmake

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(APPLE 1)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(WIN32 1)
else()
    set(UNIX 1)
endif()

if(APPLE)
    set(GODOT_FILENAME "Godot_v${GODOT_VERSION}_macos.universal")
    set(GODOT_URL "https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/${GODOT_FILENAME}.zip")
    set(GODOT_BIN "${GODOT_CACHE_DIR}/Godot.app/Contents/MacOS/Godot")
elseif(WIN32)
    set(GODOT_FILENAME "/Godot_v${GODOT_VERSION}_win64.exe")
    set(GODOT_URL "https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/${GODOT_FILENAME}.zip")
    set(GODOT_BIN "${GODOT_CACHE_DIR}/${GODOT_FILENAME}")
elseif(UNIX)
    set(GODOT_FILENAME "Godot_v${GODOT_VERSION}_linux.x86_64")
    set(GODOT_URL "https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/${GODOT_FILENAME}.zip")
    set(GODOT_BIN "${GODOT_CACHE_DIR}/${GODOT_FILENAME}")
endif()

if(EXISTS "${GODOT_BIN}")
    message(STATUS "Godot Engine found at: ${GODOT_BIN}")
    return()
endif()

message(STATUS "Downloading Godot ${GODOT_VERSION} from ${GODOT_URL}...")
file(MAKE_DIRECTORY "${GODOT_CACHE_DIR}")
file(DOWNLOAD "${GODOT_URL}" "${GODOT_CACHE_DIR}/godot.zip" SHOW_PROGRESS)

message(STATUS "Extracting...")
file(ARCHIVE_EXTRACT INPUT "${GODOT_CACHE_DIR}/godot.zip" DESTINATION "${GODOT_CACHE_DIR}")

file(REMOVE "${GODOT_CACHE_DIR}/godot.zip")
if(UNIX OR APPLE)
    execute_process(COMMAND chmod +x "${GODOT_BIN}")
endif()

message(STATUS "Godot installed to: ${GODOT_BIN}")