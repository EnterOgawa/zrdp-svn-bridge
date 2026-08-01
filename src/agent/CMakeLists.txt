add_library(
    zrdp_agent_network
    STATIC
        network/allowlist.cpp
        network/allowlist.h
        network/upstream_connection.cpp
        network/upstream_connection.h
)

target_include_directories(
    zrdp_agent_network
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_agent_network
    PUBLIC
        zrdp_protocol
    PRIVATE
        Ws2_32
)

add_library(
    zrdp_agent_bridge
    STATIC
        bridge/agent_bridge.cpp
        bridge/agent_bridge.h
        bridge/agent_stream_relay.cpp
        bridge/agent_stream_relay.h
)

target_include_directories(
    zrdp_agent_bridge
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_agent_bridge
    PUBLIC
        zrdp_agent_network
        zrdp_protocol
)

add_library(
    zrdp_agent_runtime
    STATIC
        app/session_validation.cpp
        app/session_validation.h
        channel/wts_virtual_channel.cpp
        channel/wts_virtual_channel.h
)

target_include_directories(
    zrdp_agent_runtime
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_agent_runtime
    PUBLIC
        zrdp_common
        zrdp_protocol
    PRIVATE
        Wtsapi32
)

add_executable(
    ZRdpSvnAgent
        app/main.cpp
        app/ZRdpSvnAgent.manifest
        app/ZRdpSvnAgent.rc
        "${PROJECT_SOURCE_DIR}/assets/icons/zrdp-icon.rc"
)

set_target_properties(
    ZRdpSvnAgent
    PROPERTIES
        OUTPUT_NAME "ZRdpSvnAgent"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE
            "${ZRDP_PORTABLE_OUTPUT_DIRECTORY}"
)

target_link_libraries(
    ZRdpSvnAgent
    PRIVATE
        zrdp_agent_bridge
        zrdp_agent_runtime
        zrdp_build_options
        zrdp_common
        Wtsapi32
        user32
)
