cmake_minimum_required(VERSION 3.14)
project(luna_simulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add -g for debugging
set(CMAKE_BUILD_TYPE Debug)

# Define SIMULATOR macro
add_definitions(-DSIMULATOR=1)
add_definitions(-DLV_CONF_INCLUDE_SIMPLE=1)

# Find SDL2 (installed via Homebrew)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)
pkg_check_modules(CJSON REQUIRED libcjson)

# Fetch LVGL 9.x
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2
)

# Tell LVGL where to find lv_conf.h
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)

# Configure LVGL before making it available
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(lvgl)

# Exclude ARM-specific assembly files that don't compile on macOS
get_target_property(LVGL_SOURCES lvgl SOURCES)
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*helium.*\\.S$")
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*neon.*\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${LVGL_SOURCES}")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/luna_face
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network
    ${SDL2_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${lvgl_SOURCE_DIR}
)

# Source files
set(SOURCES
    main.c
    hal/sdl_display.c
    hal/sdl_mouse.c
    hal/sdl_audio.c
    net/ws_client_sim.c
    stubs/esp_stubs.c
    # Luna face components (from ESP32 project)
    ../components/luna_face/face_renderer.c
    ../components/luna_face/emotions.c
    # Protocol parser (from ESP32 project)
    ../components/network/luna_protocol.c
)

# Create executable
add_executable(luna_simulator ${SOURCES})

# Link libraries
target_link_libraries(luna_simulator
    lvgl
    ${SDL2_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
    ${CJSON_LIBRARIES}
    m
    pthread
)

# Add library paths
target_link_directories(luna_simulator PRIVATE
    ${SDL2_LIBRARY_DIRS}
    ${LIBWEBSOCKETS_LIBRARY_DIRS}
    ${CJSON_LIBRARY_DIRS}
)
