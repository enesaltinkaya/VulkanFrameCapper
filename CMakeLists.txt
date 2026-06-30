cmake_minimum_required(VERSION 3.16)
project(VulkanFrameCapper LANGUAGES CXX)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(VULKAN_SDK_ROOT "/home/enes/Sdks/VulkanSDK-linux/1.4.350.1/x86_64" CACHE PATH "Vulkan SDK root")

add_library(VulkanFrameCapper SHARED
    src/layer.cpp
    src/key_input.cpp)
target_include_directories(VulkanFrameCapper PRIVATE "${VULKAN_SDK_ROOT}/include")
target_compile_definitions(VulkanFrameCapper PRIVATE
    VK_USE_PLATFORM_XLIB_KHR
    VK_USE_PLATFORM_WAYLAND_KHR)
target_compile_options(VulkanFrameCapper PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(VulkanFrameCapper PRIVATE
    dl
    pthread
    PkgConfig::X11
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::XKBCOMMON)
set_target_properties(VulkanFrameCapper PROPERTIES
    OUTPUT_NAME "VulkanFrameCapper"
    PREFIX "lib")

set(VFC_LIBRARY_PATH "./libVulkanFrameCapper.so")
configure_file(layers/VK_LAYER_MEIN_frame_capper.json.in
               ${CMAKE_BINARY_DIR}/VK_LAYER_MEIN_frame_capper.json @ONLY)

set(VFC_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/libVulkanFrameCapper.so")
configure_file(layers/VK_LAYER_MEIN_frame_capper.json.in
               ${CMAKE_BINARY_DIR}/install-manifest/VK_LAYER_MEIN_frame_capper.json @ONLY)

install(TARGETS VulkanFrameCapper LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_BINARY_DIR}/install-manifest/VK_LAYER_MEIN_frame_capper.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d)
