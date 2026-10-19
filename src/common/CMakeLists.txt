add_library(
    zrdp_common
    STATIC
        diagnostic_log.cpp
        diagnostic_log.h
)

target_include_directories(
    zrdp_common
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_definitions(
    zrdp_common
    PRIVATE
        ZRDP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(
    zrdp_common
    PUBLIC
        zrdp_build_options
    PRIVATE
        ole32
)

add_library(zrdp_agent_setup STATIC agent_setup.cpp agent_setup.h)
target_include_directories(zrdp_agent_setup PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(zrdp_agent_setup PUBLIC zrdp_build_options PRIVATE zrdp_client_update zrdp_agent_allowlist zrdp_agent_runtime advapi32 shell32 ole32)
