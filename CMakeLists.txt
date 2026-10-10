cmake_minimum_required(VERSION 3.31)

project(
    ZRdpSvnBridge
    VERSION 0.7.21
    DESCRIPTION "RDP client with a constrained SVN bridge over an RDP virtual channel"
    LANGUAGES CXX RC
)

set(
    ZRDP_PORTABLE_OUTPUT_DIRECTORY
    "${PROJECT_SOURCE_DIR}/out"
    CACHE PATH
    "Portable executable output directory"
)

if(NOT WIN32)
    message(FATAL_ERROR "ZRdp SVN Bridge supports Windows only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "ZRdp SVN Bridge requires the Visual Studio 2022 MSVC toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ZRdp SVN Bridge is currently built for Windows x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(CTest)
include(cmake/ZRdpCompilerOptions.cmake)

add_subdirectory(src)

if(BUILD_TESTING)
    # 新規cloneでも安全に検査できる隔離fixture基点だけを作る。
    file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/work/tmp")
    add_subdirectory(tests)
endif()
