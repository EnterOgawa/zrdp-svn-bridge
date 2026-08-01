add_library(zrdp_build_options INTERFACE)

target_compile_features(zrdp_build_options INTERFACE cxx_std_20)

target_compile_definitions(
    zrdp_build_options
    INTERFACE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
)

target_compile_options(
    zrdp_build_options
    INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            /EHsc
            /GS
            /guard:cf
            /sdl
            /external:anglebrackets
            /external:W0
        >
)

target_link_options(
    zrdp_build_options
    INTERFACE
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
        /guard:cf
)
