add_executable(zrdp_build_contract_test unit/build_contract_test.cpp)

add_executable(zrdp_agent_deployment_test unit/agent_deployment_test.cpp)
target_link_libraries(zrdp_agent_deployment_test PRIVATE zrdp_agent_settings zrdp_client_update)
add_dependencies(zrdp_agent_deployment_test ZRdpSvnBridge ZRdpSvnAgent)
add_test(NAME unit.agent_deployment COMMAND zrdp_agent_deployment_test "$<TARGET_FILE:ZRdpSvnBridge>" "$<TARGET_FILE:ZRdpSvnAgent>")
set_tests_properties(unit.agent_deployment PROPERTIES TIMEOUT 10)

add_executable(zrdp_agent_deployment_window_test unit/agent_deployment_window_test.cpp "${PROJECT_SOURCE_DIR}/src/client/app/agent_settings.rc")
target_link_libraries(zrdp_agent_deployment_window_test PRIVATE zrdp_agent_settings_ui)
add_test(NAME unit.agent_deployment_window COMMAND zrdp_agent_deployment_window_test)
set_tests_properties(unit.agent_deployment_window PROPERTIES TIMEOUT 10)

add_executable(zrdp_agent_start_window_test unit/agent_start_window_test.cpp "${PROJECT_SOURCE_DIR}/src/client/app/agent_settings.rc")
target_link_libraries(zrdp_agent_start_window_test PRIVATE zrdp_agent_settings_ui)
add_test(NAME unit.agent_start_window COMMAND zrdp_agent_start_window_test)
set_tests_properties(unit.agent_start_window PROPERTIES TIMEOUT 10)

add_executable(zrdp_session_agent_controls_test unit/session_agent_controls_test.cpp
    "${PROJECT_SOURCE_DIR}/src/client/app/main_window.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/app/ZRdpSvnBridge.rc"
    "${PROJECT_SOURCE_DIR}/src/client/app/agent_settings.rc"
    "${PROJECT_SOURCE_DIR}/assets/icons/zrdp-icon.rc")
target_include_directories(zrdp_session_agent_controls_test PRIVATE "${PROJECT_SOURCE_DIR}/src/client/app")
target_link_libraries(zrdp_session_agent_controls_test PRIVATE zrdp_client_bridge zrdp_client_proxy
    zrdp_agent_settings_ui zrdp_connection_history zrdp_rdp zrdp_rdp_window_sizing comctl32 gdi32 ole32 shell32 user32)
add_test(NAME unit.session_agent_controls COMMAND zrdp_session_agent_controls_test)
set_tests_properties(unit.session_agent_controls PROPERTIES TIMEOUT 10)

foreach(_agent_settings_test IN ITEMS model service windows)
    add_executable(zrdp_agent_settings_${_agent_settings_test}_test unit/agent_settings_${_agent_settings_test}_test.cpp)
    target_link_libraries(zrdp_agent_settings_${_agent_settings_test}_test PRIVATE zrdp_agent_settings)
    add_test(NAME unit.agent_settings_${_agent_settings_test} COMMAND zrdp_agent_settings_${_agent_settings_test}_test)
    set_tests_properties(unit.agent_settings_${_agent_settings_test} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(zrdp_agent_settings_window_test unit/agent_settings_window_test.cpp "${PROJECT_SOURCE_DIR}/src/client/app/agent_settings.rc")
target_link_libraries(zrdp_agent_settings_window_test PRIVATE zrdp_agent_settings_ui comctl32 user32)
add_test(NAME unit.agent_settings_window COMMAND zrdp_agent_settings_window_test)
set_tests_properties(unit.agent_settings_window PROPERTIES TIMEOUT 15)

foreach(_update_test IN ITEMS policy apply service)
    add_executable(zrdp_client_update_${_update_test}_test unit/client_update_${_update_test}_test.cpp)
    target_link_libraries(zrdp_client_update_${_update_test}_test PRIVATE zrdp_client_update)
    add_test(NAME unit.client_update_${_update_test} COMMAND zrdp_client_update_${_update_test}_test)
    set_tests_properties(unit.client_update_${_update_test} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(zrdp_client_update_windows_test unit/client_update_windows_test.cpp)
target_link_libraries(zrdp_client_update_windows_test PRIVATE zrdp_client_update)
add_dependencies(zrdp_client_update_windows_test ZRdpSvnBridge ZRdpSvnAgent)
add_test(NAME unit.client_update_windows COMMAND zrdp_client_update_windows_test "$<TARGET_FILE:ZRdpSvnBridge>" "$<TARGET_FILE:ZRdpSvnAgent>")
set_tests_properties(unit.client_update_windows PROPERTIES TIMEOUT 30)

add_test(NAME unit.client_update_invalid_helper COMMAND "$<TARGET_FILE:ZRdpSvnBridge>" --zrdp-maintenance 0 0)
add_test(NAME unit.client_update_invalid_restart COMMAND "$<TARGET_FILE:ZRdpSvnBridge>" --zrdp-update-restarted 0 0 ../outside)
set_tests_properties(unit.client_update_invalid_helper unit.client_update_invalid_restart PROPERTIES WILL_FAIL TRUE TIMEOUT 5)

find_program(ZRDP_TEST_POWERSHELL NAMES pwsh powershell)
if(ZRDP_TEST_POWERSHELL)
    add_test(
        NAME unit.client_update_manifest_script
        COMMAND "${ZRDP_TEST_POWERSHELL}" -NoProfile -NonInteractive -File
            "${PROJECT_SOURCE_DIR}/tests/unit/client_update_manifest_script_test.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
            -ClientExecutable "$<TARGET_FILE:ZRdpSvnBridge>"
            -WorkingDirectory "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(unit.client_update_manifest_script PROPERTIES TIMEOUT 30)
endif()

target_link_libraries(zrdp_build_contract_test PRIVATE zrdp_build_options)

add_test(NAME unit.build_contract COMMAND zrdp_build_contract_test)

add_executable(
    zrdp_source_comment_contract_test
    unit/source_comment_contract_test.cpp
)

target_link_libraries(
    zrdp_source_comment_contract_test
    PRIVATE
        zrdp_build_options
)

add_test(
    NAME unit.source_comment_contract
    COMMAND
        zrdp_source_comment_contract_test
        "${PROJECT_SOURCE_DIR}"
)

add_executable(
    zrdp_resource_contract_test
    unit/resource_contract_test.cpp
)

target_include_directories(
    zrdp_resource_contract_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_resource_contract_test
    PRIVATE
        zrdp_build_options
        user32
)

add_dependencies(
    zrdp_resource_contract_test
    ZRdpSvnAgent
    ZRdpSvnBridge
)

add_test(
    NAME unit.resource_contract
    COMMAND
        zrdp_resource_contract_test
        "$<TARGET_FILE:ZRdpSvnBridge>"
        "$<TARGET_FILE:ZRdpSvnAgent>"
)

add_executable(
    zrdp_portable_binary_contract_test
    unit/portable_binary_contract_test.cpp
)

target_link_libraries(
    zrdp_portable_binary_contract_test
    PRIVATE
        zrdp_build_options
        version
)

add_dependencies(
    zrdp_portable_binary_contract_test
    ZRdpSvnAgent
    ZRdpSvnBridge
)

add_test(
    NAME unit.portable_binary_contract
    COMMAND
        zrdp_portable_binary_contract_test
        "$<TARGET_FILE:ZRdpSvnBridge>"
        "$<TARGET_FILE:ZRdpSvnAgent>"
)

add_executable(zrdp_rdp_host_smoke_test unit/rdp_host_smoke_test.cpp)

target_link_libraries(
    zrdp_rdp_host_smoke_test
    PRIVATE
        zrdp_build_options
        zrdp_rdp
        zrdp_agent_setup
        ole32
        user32
)

target_compile_definitions(zrdp_rdp_host_smoke_test PRIVATE ZRDP_MSTSCAX_TYPE_LIBRARY="$ENV{SystemRoot}/System32/mstscax.dll")
add_test(NAME unit.rdp_host_smoke COMMAND zrdp_rdp_host_smoke_test "${PROJECT_SOURCE_DIR}/work/tmp")
set_tests_properties(unit.rdp_host_smoke PROPERTIES TIMEOUT 30)

add_executable(
    zrdp_diagnostic_log_test
    unit/diagnostic_log_test.cpp
)

target_link_libraries(
    zrdp_diagnostic_log_test
    PRIVATE
        zrdp_common
)

add_test(NAME unit.diagnostic_log COMMAND zrdp_diagnostic_log_test)

add_executable(
    zrdp_connection_history_test
    unit/connection_history_test.cpp
)

target_link_libraries(
    zrdp_connection_history_test
    PRIVATE
        zrdp_connection_history
)

add_test(
    NAME unit.connection_history
    COMMAND zrdp_connection_history_test
)

add_executable(
    zrdp_session_proxy_port_allocator_test
    unit/session_proxy_port_allocator_test.cpp
    "${PROJECT_SOURCE_DIR}/src/client/app/session_proxy_port_allocator.cpp"
)

target_include_directories(
    zrdp_session_proxy_port_allocator_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_session_proxy_port_allocator_test
    PRIVATE
        zrdp_build_options
)

add_test(
    NAME unit.session_proxy_port_allocator
    COMMAND zrdp_session_proxy_port_allocator_test
)

add_executable(
    zrdp_session_disconnect_policy_test
    unit/session_disconnect_policy_test.cpp
)

target_include_directories(
    zrdp_session_disconnect_policy_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_session_disconnect_policy_test
    PRIVATE
        zrdp_build_options
)

add_test(
    NAME unit.session_disconnect_policy
    COMMAND zrdp_session_disconnect_policy_test
)

add_executable(
    zrdp_dynamic_resolution_retry_test
    unit/dynamic_resolution_retry_test.cpp
)

target_include_directories(
    zrdp_dynamic_resolution_retry_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_dynamic_resolution_retry_test
    PRIVATE
        zrdp_build_options
)

add_test(
    NAME unit.dynamic_resolution_retry
    COMMAND zrdp_dynamic_resolution_retry_test
)

add_executable(
    zrdp_rdp_window_sizing_test
    unit/rdp_window_sizing_test.cpp
)

target_link_libraries(
    zrdp_rdp_window_sizing_test
    PRIVATE
        zrdp_rdp_window_sizing
)

add_test(
    NAME unit.rdp_window_sizing
    COMMAND zrdp_rdp_window_sizing_test
)

add_executable(
    zrdp_protocol_frame_test
    protocol/frame_test.cpp
)

target_link_libraries(
    zrdp_protocol_frame_test
    PRIVATE
        zrdp_protocol
)

add_test(
    NAME protocol.frame
    COMMAND zrdp_protocol_frame_test
)

add_executable(
    zrdp_protocol_hello_test
    protocol/hello_test.cpp
)

target_link_libraries(
    zrdp_protocol_hello_test
    PRIVATE
        zrdp_protocol
)

add_test(
    NAME protocol.hello
    COMMAND zrdp_protocol_hello_test
)

add_executable(
    zrdp_protocol_multiplexer_test
    protocol/multiplexer_test.cpp
)

target_link_libraries(
    zrdp_protocol_multiplexer_test
    PRIVATE
        zrdp_protocol
)

add_test(
    NAME protocol.multiplexer
    COMMAND zrdp_protocol_multiplexer_test
)

add_executable(
    zrdp_client_bridge_test
    protocol/client_bridge_test.cpp
)

target_link_libraries(
    zrdp_client_bridge_test
    PRIVATE
        zrdp_client_bridge
)

add_test(
    NAME protocol.client_bridge
    COMMAND zrdp_client_bridge_test
)

add_executable(
    zrdp_http_request_parser_test
    proxy/http_request_parser_test.cpp
)

target_link_libraries(
    zrdp_http_request_parser_test
    PRIVATE
        zrdp_client_proxy
)

add_test(
    NAME proxy.http_request_parser
    COMMAND zrdp_http_request_parser_test
)

add_executable(
    zrdp_trac_http_compatibility_test
    proxy/trac_http_compatibility_test.cpp
)

target_link_libraries(
    zrdp_trac_http_compatibility_test
    PRIVATE
        zrdp_client_proxy
)

add_test(
    NAME proxy.trac_http_compatibility
    COMMAND zrdp_trac_http_compatibility_test
)

add_executable(
    zrdp_client_proxy_config_test
    proxy/client_proxy_config_test.cpp
)

target_link_libraries(
    zrdp_client_proxy_config_test
    PRIVATE
        zrdp_client_proxy
)

add_test(
    NAME proxy.client_config
    COMMAND zrdp_client_proxy_config_test
)

add_executable(
    zrdp_client_proxy_session_test
    proxy/client_proxy_session_test.cpp
)

target_link_libraries(
    zrdp_client_proxy_session_test
    PRIVATE
        zrdp_client_proxy
)

add_test(
    NAME proxy.client_session
    COMMAND zrdp_client_proxy_session_test
)

add_executable(
    zrdp_client_proxy_worker_test
    proxy/client_proxy_worker_test.cpp
)

target_link_libraries(
    zrdp_client_proxy_worker_test
    PRIVATE
        zrdp_client_proxy
        Iphlpapi
        Ws2_32
)

add_test(
    NAME proxy.client_worker
    COMMAND zrdp_client_proxy_worker_test
)

add_executable(
    zrdp_agent_session_validation_test
    agent/session_validation_test.cpp
)

target_link_libraries(
    zrdp_agent_session_validation_test
    PRIVATE
        zrdp_agent_runtime
)

add_test(
    NAME agent.session_validation
    COMMAND zrdp_agent_session_validation_test
)

add_executable(
    zrdp_agent_allowlist_test
    agent/allowlist_test.cpp
)

target_link_libraries(
    zrdp_agent_allowlist_test
    PRIVATE
        zrdp_agent_network
)

add_test(
    NAME agent.allowlist
    COMMAND
        zrdp_agent_allowlist_test
        "${PROJECT_SOURCE_DIR}/config/examples/agent.ini.example"
)

add_executable(
    zrdp_agent_stream_relay_test
    agent/agent_stream_relay_test.cpp
)

target_link_libraries(
    zrdp_agent_stream_relay_test
    PRIVATE
        zrdp_agent_bridge
)

add_test(
    NAME agent.stream_relay
    COMMAND zrdp_agent_stream_relay_test
)

add_executable(
    zrdp_agent_bridge_test
    agent/agent_bridge_test.cpp
)

target_link_libraries(
    zrdp_agent_bridge_test
    PRIVATE
        zrdp_agent_bridge
)

add_test(
    NAME agent.bridge
    COMMAND zrdp_agent_bridge_test
)

add_executable(
    zrdp_phase1_echo_integration_test
    integration/phase1_echo_test.cpp
)

target_link_libraries(
    zrdp_phase1_echo_integration_test
    PRIVATE
        zrdp_agent_bridge
        zrdp_client_bridge
)

add_test(
    NAME integration.phase1_echo
    COMMAND zrdp_phase1_echo_integration_test
)

add_executable(
    zrdp_phase2_multiplexer_integration_test
    integration/phase2_multiplexer_test.cpp
)

target_link_libraries(
    zrdp_phase2_multiplexer_integration_test
    PRIVATE
        zrdp_protocol
)

add_test(
    NAME integration.phase2_multiplexer
    COMMAND zrdp_phase2_multiplexer_integration_test
)

add_executable(
    zrdp_phase3_agent_tcp_integration_test
    integration/phase3_agent_tcp_test.cpp
)

target_link_libraries(
    zrdp_phase3_agent_tcp_integration_test
    PRIVATE
        zrdp_agent_bridge
        Ws2_32
)

add_test(
    NAME integration.phase3_agent_tcp
    COMMAND zrdp_phase3_agent_tcp_integration_test
)

add_executable(
    zrdp_phase4_http_proxy_integration_test
    integration/phase4_http_proxy_test.cpp
)

target_link_libraries(
    zrdp_phase4_http_proxy_integration_test
    PRIVATE
        zrdp_agent_bridge
        zrdp_client_bridge
        zrdp_client_proxy
        Ws2_32
)

add_test(
    NAME integration.phase4_http_proxy
    COMMAND zrdp_phase4_http_proxy_integration_test
)

add_executable(
    zrdp_phase5_http_svn_compatibility_integration_test
    integration/phase5_http_svn_compatibility_test.cpp
)

target_link_libraries(
    zrdp_phase5_http_svn_compatibility_integration_test
    PRIVATE
        zrdp_agent_bridge
        zrdp_client_bridge
        zrdp_client_proxy
        Ws2_32
)

add_test(
    NAME integration.phase5_http_svn_compatibility
    COMMAND
        zrdp_phase5_http_svn_compatibility_integration_test
)

add_executable(
    zrdp_phase6_hardening_integration_test
    integration/phase6_hardening_test.cpp
)

add_executable(zrdp_agent_setup_test unit/agent_setup_test.cpp)
target_link_libraries(zrdp_agent_setup_test PRIVATE zrdp_agent_setup)
add_test(NAME unit.agent_setup COMMAND zrdp_agent_setup_test "${PROJECT_SOURCE_DIR}/work/tmp" "$<TARGET_FILE:ZRdpSvnAgent>")
set_tests_properties(unit.agent_setup PROPERTIES TIMEOUT 30)

add_executable(zrdp_agent_bootstrap_ui_test unit/agent_bootstrap_ui_test.cpp "${PROJECT_SOURCE_DIR}/src/client/app/agent_settings.rc")
target_link_libraries(zrdp_agent_bootstrap_ui_test PRIVATE zrdp_agent_settings_ui)
add_test(NAME unit.agent_bootstrap_ui COMMAND zrdp_agent_bootstrap_ui_test "${PROJECT_SOURCE_DIR}/work/tmp")
set_tests_properties(unit.agent_bootstrap_ui PROPERTIES TIMEOUT 10)

target_link_libraries(
    zrdp_phase6_hardening_integration_test
    PRIVATE
        zrdp_agent_bridge
        zrdp_client_bridge
)

add_test(
    NAME integration.phase6_hardening
    COMMAND zrdp_phase6_hardening_integration_test
)
