cmake_minimum_required(VERSION 3.25)
project(sharun VERSION 0.8.1 LANGUAGES C
    DESCRIPTION "Run dynamically linked ELF binaries everywhere"
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SHARUN_ELF32       "Support 32-bit ELF binaries"            ON)
option(SHARUN_SETENV      "Environment variable setup"              ON)
option(SHARUN_LIB4BIN     "Embed lib4bin script"                    ON)
option(SHARUN_PYINSTALLER "PyInstaller support"                    ON)
option(SHARUN_STATIC      "Statically link all dependencies"       ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Os -fvisibility=hidden)
    if(SHARUN_STATIC)
        add_compile_options(-ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()
endif()

# zlib is only needed when embedding compressed data
if(SHARUN_LIB4BIN)
    find_package(ZLIB REQUIRED)
    if(SHARUN_STATIC)
        set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
        find_library(ZLIB_STATIC NAMES z)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
        if(NOT ZLIB_STATIC)
            message(FATAL_ERROR "Static zlib (libz.a) not found. "
                "On Alpine, install zlib-static: apk add zlib-static")
        endif()
    endif()
endif()

# Host tool to compress and embed files at build time
if(SHARUN_LIB4BIN)
    add_executable(embed_compress EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_compress.c"
    )
    target_link_libraries(embed_compress PRIVATE ${ZLIB_LIBRARIES})

    set(LIB4BIN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/lib4bin_embedded.h")
    add_custom_command(
        OUTPUT  "${LIB4BIN_HEADER}"
        COMMAND embed_compress
                "${CMAKE_CURRENT_SOURCE_DIR}/lib4bin"
                "${LIB4BIN_HEADER}"
                "lib4bin_data"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lib4bin"
                "${CMAKE_CURRENT_SOURCE_DIR}/embed_compress.c"
                embed_compress
    )
    add_custom_target(lib4bin_embed DEPENDS "${LIB4BIN_HEADER}")
endif()

add_executable(sharun
    src/utils.c
    src/env.c
    src/main.c
    src/embed_data.c
)

if(SHARUN_STATIC)
    target_link_options(sharun PRIVATE -static)
endif()
if(SHARUN_LIB4BIN)
    if(SHARUN_STATIC)
        target_link_libraries(sharun PRIVATE ${ZLIB_STATIC})
    else()
        target_link_libraries(sharun PRIVATE ${ZLIB_LIBRARIES})
    endif()
endif()
target_include_directories(sharun PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(sharun PRIVATE
    SHARUN_NAME="${PROJECT_NAME}"
    SHARUN_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${SHARUN_ELF32}>:SHARUN_ELF32>
    $<$<BOOL:${SHARUN_SETENV}>:SHARUN_SETENV>
    $<$<BOOL:${SHARUN_LIB4BIN}>:SHARUN_LIB4BIN>
    $<$<BOOL:${SHARUN_PYINSTALLER}>:SHARUN_PYINSTALLER>
)

if(SHARUN_LIB4BIN)
    add_dependencies(sharun lib4bin_embed)
endif()

