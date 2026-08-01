add_executable(zrdp_build_contract_test unit/build_contract_test.cpp)

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
        ole32
        user32
)

add_test(NAME unit.rdp_host_smoke COMMAND zrdp_rdp_host_smoke_test)

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
