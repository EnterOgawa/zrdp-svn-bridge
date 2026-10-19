add_library(
    zrdp_protocol
    STATIC
        frame.cpp
        frame.h
        hello.cpp
        hello.h
        multiplexer.cpp
        multiplexer.h
)

target_include_directories(
    zrdp_protocol
    PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(
    zrdp_protocol
    PUBLIC
        zrdp_common
)
