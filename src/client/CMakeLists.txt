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
        app/rdp_container_full_screen.h
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

add_library(
    zrdp_agent_settings
    STATIC
        agent_settings/agent_settings_model.cpp
        agent_settings/agent_settings_model.h
        agent_settings/agent_settings_service.cpp
        agent_settings/agent_settings_service.h
        agent_settings/agent_settings_windows.cpp
        agent_deployment/agent_deployment.cpp
        agent_deployment/agent_deployment.h
        agent_deployment/agent_deployment_clipboard.cpp
)

target_include_directories(zrdp_agent_settings PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(zrdp_agent_settings PUBLIC zrdp_agent_allowlist zrdp_agent_setup PRIVATE zrdp_client_update bcrypt crypt32 ole32 shell32 shlwapi uuid)

add_library(
    zrdp_agent_settings_ui
    STATIC
        app/agent_settings_window.cpp
        app/agent_settings_window.h
        app/agent_deployment_window.cpp
        app/agent_deployment_window.h
        app/agent_start_window.cpp
        app/agent_start_window.h
)
target_link_libraries(zrdp_agent_settings_ui PUBLIC zrdp_agent_settings PRIVATE comctl32 comdlg32 user32 gdi32)

add_library(
    zrdp_client_update
    STATIC
        update/github_release_source.cpp
        update/github_release_source.h
        update/update_apply.cpp
        update/update_apply.h
        update/update_image_verifier.cpp
        update/update_image_verifier.h
        update/update_manifest.cpp
        update/update_manifest.h
        update/update_service.cpp
        update/update_service.h
        update/update_windows.cpp
        update/update_windows.h
        update/update_windows_handles.h
)

target_include_directories(zrdp_client_update PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(
    zrdp_client_update
    PUBLIC
        zrdp_build_options
    PRIVATE
        bcrypt
        crypt32
        shell32
        version
        winhttp
        wintrust
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
        app/agent_settings.rc
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
        zrdp_client_update
        zrdp_agent_settings_ui
        zrdp_connection_history
        zrdp_rdp
        zrdp_rdp_window_sizing
        comctl32
        gdi32
        ole32
        shell32
        user32
)
