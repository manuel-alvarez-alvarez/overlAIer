cmake_minimum_required(VERSION 3.25.1)
project(overlAIer C)

set(CMAKE_C_STANDARD 23)

include(FetchContent)
FetchContent_Declare(zf_log
    GIT_REPOSITORY https://github.com/wonder-mice/zf_log.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(zf_log)

FetchContent_Declare(tomlc17
    GIT_REPOSITORY https://github.com/cktan/tomlc17.git
    GIT_TAG        main
)
FetchContent_Populate(tomlc17)
add_library(tomlc17 STATIC ${tomlc17_SOURCE_DIR}/src/tomlc17.c)
target_include_directories(tomlc17 PUBLIC ${tomlc17_SOURCE_DIR}/src)

find_package(ALSA REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
find_library(LIBYUV_LIB yuv REQUIRED)
find_path(LIBYUV_INCLUDE libyuv.h REQUIRED)
pkg_check_modules(RGA IMPORTED_TARGET librga)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(DISPLAYINFO REQUIRED IMPORTED_TARGET libdisplay-info)

add_executable(overlAIer
    main.c
    receiver/v4l2_caps.c
    receiver/v4l2_capture.c
    receiver/alsa_caps.c
    receiver/receiver.c
    encoder/drm_caps.c
    encoder/drm_output.c
    common/pixfmt.c
    common/edid.c
    common/options.c
    common/config.c
    converter/converter.c
    converter/converter_sw.c
    converter/converter_rga.c
    receiver/alsa_capture.c
    encoder/alsa_playback.c
    overlay/overlay.c
    processor/processor_mgr.c
)

target_include_directories(overlAIer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_include_directories(overlAIer PRIVATE ${LIBYUV_INCLUDE})
target_link_libraries(overlAIer PRIVATE ALSA::ALSA PkgConfig::DRM ${LIBYUV_LIB} PkgConfig::SAMPLERATE PkgConfig::CAIRO PkgConfig::PANGOCAIRO PkgConfig::DISPLAYINFO zf_log tomlc17 pthread dl)

# Log level: VERBOSE in debug, WARN in release (compile-time removal)
target_compile_definitions(overlAIer PRIVATE
    $<$<CONFIG:Debug>:ZF_LOG_LEVEL=ZF_LOG_VERBOSE>
    $<$<CONFIG:Release>:ZF_LOG_LEVEL=ZF_LOG_INFO>
    $<$<CONFIG:RelWithDebInfo>:ZF_LOG_LEVEL=ZF_LOG_INFO>
)

if(RGA_FOUND)
    target_link_libraries(overlAIer PRIVATE PkgConfig::RGA)
    message(STATUS "librga found")
else()
    message(STATUS "librga not found, RGA backend compile-time disabled")
endif()

# --- Linting & formatting ---

# Enable clang-tidy during build (if available)
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    set_target_properties(overlAIer PROPERTIES C_CLANG_TIDY "${CLANG_TIDY}")
    message(STATUS "clang-tidy enabled: ${CLANG_TIDY}")
endif()

# Export compile_commands.json for IDE and tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Custom target: format all source files
file(GLOB_RECURSE ALL_SOURCES "*.c" "*.h")
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCES}
        COMMENT "Formatting source files with clang-format"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ALL_SOURCES}
        COMMENT "Checking source formatting"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    message(STATUS "clang-format targets: 'format', 'format-check'")
endif()
