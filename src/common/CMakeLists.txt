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
