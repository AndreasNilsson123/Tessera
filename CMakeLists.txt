cmake_minimum_required(VERSION 3.21)

# Single source of truth for the version: include/tessera/tessera.h
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/tessera/tessera.h" _tessera_header)
foreach(_part MAJOR MINOR PATCH)
    string(REGEX MATCH "#define TESSERA_VERSION_${_part} ([0-9]+)" _m "${_tessera_header}")
    if(NOT _m)
        message(FATAL_ERROR "Could not parse TESSERA_VERSION_${_part} from tessera.h")
    endif()
    set(TESSERA_VERSION_${_part} "${CMAKE_MATCH_1}")
endforeach()

project(Tessera
    VERSION ${TESSERA_VERSION_MAJOR}.${TESSERA_VERSION_MINOR}.${TESSERA_VERSION_PATCH}
    DESCRIPTION "OpenGL / OpenGL ES context creation library"
    LANGUAGES C CXX)

include(CTest)

option(TESSERA_BUILD_SHARED "Build Tessera as a shared library" ON)
option(TESSERA_BUILD_TESTS "Build Tessera tests" ${BUILD_TESTING})
option(TESSERA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TESSERA_CLANG_TIDY "Run clang-tidy on the library sources during the build" OFF)
option(TESSERA_SANITIZE "Sanitizer to enable: address, undefined, thread (or empty)" "")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Hide everything by default; only TESSERA_API symbols are exported.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(cmake/TesseraOptions.cmake)

set(TESSERA_CORE_SOURCES
    src/core/api.cpp
    src/core/config.cpp
    src/core/env.cpp
    src/core/error.cpp
    src/core/library.cpp
    src/core/log.cpp
    src/core/runtime.cpp
    src/core/thread_affinity.cpp
    src/core/version.cpp
    src/platform/builtin.cpp
    src/platform/common/arb_context.cpp
    src/platform/common/gl_debug.cpp
    src/platform/common/gl_query.cpp)
if(WIN32)
    list(APPEND TESSERA_CORE_SOURCES
        src/platform/win32/window_host.cpp
        src/platform/win32/wgl_api.cpp
        src/platform/win32/wgl_backend.cpp)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND TESSERA_CORE_SOURCES
        src/platform/x11/glx_api.cpp
        src/platform/x11/glx_backend.cpp
        src/platform/egl/egl_api.cpp
        src/platform/egl/egl_backend.cpp)
endif()

# Compiled once, shared by the public library and the white-box unit tests (which need internals that
# the shared library deliberately does not export).
add_library(tessera_core OBJECT ${TESSERA_CORE_SOURCES})
target_include_directories(tessera_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src)
target_compile_definitions(tessera_core PRIVATE TESSERA_BUILDING)
if(NOT TESSERA_BUILD_SHARED)
    target_compile_definitions(tessera_core PUBLIC TESSERA_STATIC)
endif()
tessera_apply_options(tessera_core)
if(TESSERA_CLANG_TIDY)
    find_program(TESSERA_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set_target_properties(tessera_core PROPERTIES CXX_CLANG_TIDY "${TESSERA_CLANG_TIDY_EXE}")
endif()

# macOS backend (CGL). It uses only plain C++ and dlopen, no Apple headers, so it is compiled on every platform to
# keep it under the same warnings-as-errors rules everywhere and to let tests run it against a fake library; it is only
# linked into (and registered by) the library on Apple platforms.
add_library(tessera_cgl OBJECT
    src/platform/cgl/cgl_api.cpp
    src/platform/cgl/cgl_backend.cpp)
target_include_directories(tessera_cgl
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src)
target_compile_definitions(tessera_cgl PRIVATE TESSERA_BUILDING)
if(NOT TESSERA_BUILD_SHARED)
    target_compile_definitions(tessera_cgl PUBLIC TESSERA_STATIC)
endif()
tessera_apply_options(tessera_cgl)

set(TESSERA_PLATFORM_OBJECTS "")
if(APPLE)
    set(TESSERA_PLATFORM_OBJECTS $<TARGET_OBJECTS:tessera_cgl>)
endif()

if(TESSERA_BUILD_SHARED)
    add_library(tessera SHARED $<TARGET_OBJECTS:tessera_core> ${TESSERA_PLATFORM_OBJECTS})
else()
    add_library(tessera STATIC $<TARGET_OBJECTS:tessera_core> ${TESSERA_PLATFORM_OBJECTS})
    target_compile_definitions(tessera PUBLIC TESSERA_STATIC)
endif()
add_library(Tessera::tessera ALIAS tessera)

find_package(Threads REQUIRED)
target_link_libraries(tessera PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(tessera PRIVATE gdi32 user32)
endif()
target_include_directories(tessera
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>)

set_target_properties(tessera PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME tessera)

if(TESSERA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---- install / find_package ------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS tessera EXPORT TesseraTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/tessera DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT TesseraTargets NAMESPACE Tessera:: FILE TesseraTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tessera)
configure_package_config_file(cmake/TesseraConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TesseraConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tessera)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/TesseraConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TesseraConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/TesseraConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tessera)
