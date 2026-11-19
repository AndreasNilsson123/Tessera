# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
add_executable(test_abi test_abi.cpp)
target_link_libraries(test_abi PRIVATE Tessera::tessera)
tessera_apply_options(test_abi)
add_test(NAME abi COMMAND test_abi)

# The public header must stay valid C99 (compiled as C, not C++).
add_executable(test_header_c header_c.c)
target_link_libraries(test_header_c PRIVATE Tessera::tessera)
tessera_apply_options(test_header_c)
add_test(NAME header_c COMMAND test_header_c)
if(WIN32 AND TESSERA_BUILD_SHARED)
    add_custom_command(TARGET test_header_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_header_c>)
endif()

# On Windows the test must find tessera.dll next to it.
if(WIN32 AND TESSERA_BUILD_SHARED)
    add_custom_command(TARGET test_abi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_abi>)
endif()

# White-box core tests: compile against the core objects directly (internals are hidden in the DLL).
add_executable(test_core test_core.cpp $<TARGET_OBJECTS:tessera_core> ${TESSERA_PLATFORM_OBJECTS})
target_include_directories(test_core PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_core PRIVATE TESSERA_STATIC)
target_link_libraries(test_core PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(test_core PRIVATE gdi32 user32)
endif()
tessera_apply_options(test_core)
add_test(NAME core COMMAND test_core)

# Seeded fuzzing of config structs, handles, struct sizes, proc names and version strings (see the file header).
add_executable(test_fuzz test_fuzz.cpp $<TARGET_OBJECTS:tessera_core> ${TESSERA_PLATFORM_OBJECTS})
target_include_directories(test_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_fuzz PRIVATE TESSERA_STATIC)
target_link_libraries(test_fuzz PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(test_fuzz PRIVATE gdi32 user32)
endif()
tessera_apply_options(test_fuzz)
add_test(NAME fuzz COMMAND test_fuzz)
set_tests_properties(fuzz PROPERTIES TIMEOUT 300)

# WGL integration test: public API only, against the real DLL, with a raw WGL context as the "other library".
if(WIN32 AND TESSERA_BUILD_SHARED)
    add_executable(test_wgl test_wgl.cpp)
    target_link_libraries(test_wgl PRIVATE Tessera::tessera opengl32 gdi32 user32 dxgi)
    tessera_apply_options(test_wgl)
    add_custom_command(TARGET test_wgl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_wgl>)
    add_test(NAME wgl COMMAND test_wgl)

    # OpenGL <-> Direct3D 11 shared texture through the native handles and proc addresses (WGL_NV_DX_interop2): proves the
    # interop primitives without Tessera having an interop API. Skips (77) where the extension or a matching adapter is missing.
    add_executable(test_dxinterop test_dxinterop.cpp)
    target_link_libraries(test_dxinterop PRIVATE Tessera::tessera opengl32 d3d11 dxgi)
    tessera_apply_options(test_dxinterop)
    add_custom_command(TARGET test_dxinterop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_dxinterop>)
    add_test(NAME dxinterop COMMAND test_dxinterop)
    set_tests_properties(dxinterop PROPERTIES LABELS "gpu" TIMEOUT 120 SKIP_RETURN_CODE 77)
    # Needs a real OpenGL driver: excluded from the default (hosted-runner) test preset.
    set_tests_properties(wgl PROPERTIES LABELS "gpu" TIMEOUT 180)
endif()

# Fault injection against the real backends (the counterpart of the CGL fake-driver sweep): wraps the driver entry points the
# backends call, fails each one in turn, and checks error codes, leaks and the other library's context. White-box (links the core
# objects); needs a real driver, exits 77 without one. One backend per process on Linux.
if(WIN32 OR (UNIX AND NOT APPLE))
    add_executable(test_fault test_fault.cpp $<TARGET_OBJECTS:tessera_core>)
    target_include_directories(test_fault PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(test_fault PRIVATE TESSERA_STATIC)
    target_link_libraries(test_fault PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(test_fault PRIVATE gdi32 user32)
        add_test(NAME fault_wgl COMMAND test_fault)
        set_tests_properties(fault_wgl PROPERTIES LABELS "gpu;fault" TIMEOUT 1500 SKIP_RETURN_CODE 77)
    else()
        add_test(NAME fault_glx COMMAND test_fault glx)
        add_test(NAME fault_egl COMMAND test_fault egl)
        set_tests_properties(fault_glx fault_egl PROPERTIES LABELS "gpu;fault" TIMEOUT 1500 SKIP_RETURN_CODE 77)
    endif()
    tessera_apply_options(test_fault)
endif()

# Linux counterpart of dxinterop: an EGLImage handed from one Tessera context to another through the native handles and proc
# addresses (EGL_KHR_gl_texture_2D_image + GL_OES_EGL_image). Skips (77) where the extensions are missing.
if(UNIX AND NOT APPLE AND TESSERA_BUILD_SHARED)
    add_executable(test_eglimage test_eglimage.cpp)
    target_link_libraries(test_eglimage PRIVATE Tessera::tessera)
    tessera_apply_options(test_eglimage)
    add_test(NAME eglimage COMMAND test_eglimage)
    set_tests_properties(eglimage PROPERTIES LABELS "gpu" TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# Coverage-guided fuzzing (libFuzzer). Needs clang: configure with the `fuzz` preset, which compiles everything with
# -fsanitize=fuzzer-no-link,address,undefined; the targets add the fuzzer runtime. Run: tests/fuzz/corpus/<target> holds the seeds.
if(TESSERA_BUILD_FUZZERS)
    foreach(_fuzz fuzz_api fuzz_glversion)
        add_executable(${_fuzz} fuzz/${_fuzz}.cpp $<TARGET_OBJECTS:tessera_core>)
        target_include_directories(${_fuzz} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
        target_compile_definitions(${_fuzz} PRIVATE TESSERA_STATIC)
        target_link_libraries(${_fuzz} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(${_fuzz} PRIVATE -fsanitize=fuzzer-no-link)
        target_link_options(${_fuzz} PRIVATE -fsanitize=fuzzer)
        tessera_apply_options(${_fuzz})
    endforeach()
endif()

# Create/use/destroy loops with leak tracking (handles, GDI/USER objects, fds, memory). Real driver; exits 77 without one.
# Scale with TESSERA_SOAK_CYCLES / TESSERA_SOAK_SECONDS (see the file header); the default is a ~1 minute smoke run.
if(TESSERA_BUILD_SHARED AND (WIN32 OR (UNIX AND NOT APPLE)))
    add_executable(test_soak test_soak.cpp)
    target_link_libraries(test_soak PRIVATE Tessera::tessera Threads::Threads)
    tessera_apply_options(test_soak)
    if(WIN32)
        add_custom_command(TARGET test_soak POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_soak>)
    endif()
    add_test(NAME soak COMMAND test_soak)
    # The soak sets its own duration (TESSERA_SOAK_SECONDS, up to the 24 h run), so ctest must not cut it short.
    set_tests_properties(soak PROPERTIES LABELS "gpu;soak" TIMEOUT 100000 SKIP_RETURN_CODE 77)
endif()

# GLX integration test: public API only, against the real shared library, with a raw GLX context on its own X
# connection as the "other library". Needs an X server (Xvfb is enough); exits 77 = skipped without one.
if(UNIX AND NOT APPLE AND TESSERA_BUILD_SHARED)
    add_executable(test_glx test_glx.cpp)
    target_include_directories(test_glx PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(test_glx PRIVATE Tessera::tessera Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_glx)
    add_test(NAME glx COMMAND test_glx)
    # Whether the host called XInitThreads first, in two fresh processes (see the comment in test_glx.cpp).
    add_test(NAME glx_xinitthreads_host COMMAND test_glx --xinitthreads=host)
    add_test(NAME glx_xinitthreads_none COMMAND test_glx --xinitthreads=none)
    set_tests_properties(glx_xinitthreads_host glx_xinitthreads_none PROPERTIES LABELS "gpu" TIMEOUT 120 SKIP_RETURN_CODE 77)
    set_tests_properties(glx PROPERTIES LABELS "gpu" TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

# EGL integration test: public API only (backend forced to EGL), with raw EGL contexts as the "other library".
# Needs an EGL implementation with a headless platform (Mesa surfaceless); exits 77 = skipped without one.
if(UNIX AND NOT APPLE AND TESSERA_BUILD_SHARED)
    add_executable(test_egl test_egl.cpp)
    target_include_directories(test_egl PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(test_egl PRIVATE Tessera::tessera Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_egl)
    add_test(NAME egl COMMAND test_egl)
    set_tests_properties(egl PROPERTIES LABELS "gpu" TIMEOUT 180 SKIP_RETURN_CODE 77)

    # Windows of a caller's window system through EGL: an X11 window and a Wayland surface, both made by the test itself.
    add_executable(test_eglwindow test_eglwindow.cpp)
    target_link_libraries(test_eglwindow PRIVATE Tessera::tessera Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_eglwindow)
    add_test(NAME eglwindow_x11 COMMAND test_eglwindow x11)
    add_test(NAME eglwindow_wayland COMMAND test_eglwindow wayland)
    set_tests_properties(eglwindow_x11 eglwindow_wayland PROPERTIES LABELS "gpu" TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# macOS backend logic, run against a fake OpenGL.framework (tests/fake_cgl.cpp) on any POSIX platform. Verifies the
# backend's own behaviour, not Apple's: real-hardware behaviour needs a Mac.
if(UNIX)
    add_library(fake_cgl SHARED fake_cgl.cpp)
    tessera_apply_options(fake_cgl)

    add_executable(test_cgl_fake test_cgl_fake.cpp $<TARGET_OBJECTS:tessera_core> $<TARGET_OBJECTS:tessera_cgl>)
    add_dependencies(test_cgl_fake fake_cgl)
    target_include_directories(test_cgl_fake PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(test_cgl_fake PRIVATE TESSERA_STATIC FAKE_CGL_PATH=\"$<TARGET_FILE:fake_cgl>\")
    target_link_libraries(test_cgl_fake PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_cgl_fake)
    add_test(NAME cgl_fake COMMAND test_cgl_fake)
    # The library is missing / is not OpenGL.framework: a clean UNSUPPORTED (each needs its own process: the framework
    # is loaded once per process).
    add_test(NAME cgl_missing_library COMMAND test_cgl_fake --missing-library)
    add_test(NAME cgl_wrong_library COMMAND test_cgl_fake --wrong-library)
    set_tests_properties(cgl_missing_library cgl_wrong_library PROPERTIES TIMEOUT 60)
    set_tests_properties(cgl_fake PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# Real OpenGL.framework smoke test (macOS only; never run so far - see the header of test_cgl.cpp).
if(APPLE AND TESSERA_BUILD_SHARED)
    add_executable(test_cgl test_cgl.cpp)
    target_include_directories(test_cgl PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(test_cgl PRIVATE Tessera::tessera Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_cgl)
    add_test(NAME cgl COMMAND test_cgl)
    set_tests_properties(cgl PROPERTIES LABELS "gpu" TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

set_tests_properties(abi core header_c PROPERTIES TIMEOUT 60)
