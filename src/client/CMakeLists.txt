set(_zrdp_mstscax_type_library "$ENV{SystemRoot}/System32/mstscax.dll")
file(TO_CMAKE_PATH "${_zrdp_mstscax_type_library}" _zrdp_mstscax_type_library)

if(NOT EXISTS "${_zrdp_mstscax_type_library}")
    message(FATAL_ERROR "The Windows Remote Desktop ActiveX type library was not found.")
endif()

add_library(
    zrdp_rdp
    STATIC
        rdp/rdp_audio_settings.h
        rdp/rdp_control_host.cpp
        rdp/rdp_control_host.h
        rdp/rdp_import.h
)

target_include_directories(zrdp_rdp PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_compile_definitions(
    zrdp_rdp
    PRIVATE
        ZRDP_MSTSCAX_TYPE_LIBRARY="${_zrdp_mstscax_type_library}"
)

target_link_libraries(
    zrdp_rdp
    PUBLIC
        zrdp_common
    PRIVATE
        zrdp_protocol
        ole32
        oleaut32
        urlmon
        user32
        gdi32
        uuid
)

add_library(
    zrdp_connection_history
    STATIC
        app/connection_history.cpp
        app/connection_history.h
        app/rdp_user_identity.cpp
        app/rdp_user_identity.h
)

target_include_directories(
    zrdp_connection_history
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_connection_history
    PUBLIC
        zrdp_build_options
    PRIVATE
        crypt32
        ole32
        shell32
)

add_library(
    zrdp_rdp_window_sizing
    STATIC
        app/rdp_window_sizing.cpp
        app/rdp_window_sizing.h
)

target_include_directories(
    zrdp_rdp_window_sizing
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_rdp_window_sizing
    PUBLIC
        zrdp_build_options
)

add_library(
    zrdp_client_bridge
    STATIC
        bridge/client_bridge.cpp
        bridge/client_bridge.h
)

target_include_directories(
    zrdp_client_bridge
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_client_bridge
    PUBLIC
        zrdp_protocol
)

add_library(
    zrdp_client_proxy
    STATIC
        proxy/client_proxy_config.cpp
        proxy/client_proxy_config.h
        proxy/client_proxy_session.cpp
        proxy/client_proxy_session.h
        proxy/client_proxy_worker.cpp
        proxy/client_proxy_worker.h
        proxy/http_request_parser.cpp
        proxy/http_request_parser.h
)

target_include_directories(
    zrdp_client_proxy
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_client_proxy
    PUBLIC
        zrdp_protocol
    PRIVATE
        Ws2_32
)

add_executable(
    ZRdpSvnBridge
    WIN32
        app/main.cpp
        app/client_application.cpp
        app/client_application.h
        app/connection_manager_window.cpp
        app/connection_manager_window.h
        app/dynamic_resolution_retry.h
        app/dynamic_resolution_target.h
        app/main_window.cpp
        app/main_window.h
        app/pre_login_connection_retry.h
        app/resource.h
        app/session_disconnect_policy.h
        app/session_launch_settings.h
        app/session_proxy_port_allocator.cpp
        app/session_proxy_port_allocator.h
        app/ZRdpSvnBridge.manifest
        app/ZRdpSvnBridge.rc
        "${PROJECT_SOURCE_DIR}/assets/icons/zrdp-icon.rc"
)

set_target_properties(
    ZRdpSvnBridge
    PROPERTIES
        OUTPUT_NAME "ZRdpSvnBridge"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ZRDP_PORTABLE_OUTPUT_DIRECTORY}"
)

target_link_libraries(
    ZRdpSvnBridge
    PRIVATE
        zrdp_build_options
        zrdp_client_bridge
        zrdp_client_proxy
        zrdp_connection_history
        zrdp_rdp
        zrdp_rdp_window_sizing
        comctl32
        gdi32
        ole32
        shell32
        user32
)
