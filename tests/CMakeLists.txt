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
add_executable(test_core test_core.cpp $<TARGET_OBJECTS:tessera_core>)
target_include_directories(test_core PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_core PRIVATE TESSERA_STATIC)
target_link_libraries(test_core PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(test_core PRIVATE gdi32 user32)
endif()
tessera_apply_options(test_core)
add_test(NAME core COMMAND test_core)

# WGL integration test: public API only, against the real DLL, with a raw WGL context as the "other library".
if(WIN32 AND TESSERA_BUILD_SHARED)
    add_executable(test_wgl test_wgl.cpp)
    target_link_libraries(test_wgl PRIVATE Tessera::tessera opengl32 gdi32 user32)
    tessera_apply_options(test_wgl)
    add_custom_command(TARGET test_wgl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:test_wgl>)
    add_test(NAME wgl COMMAND test_wgl)
    # Needs a real OpenGL driver: excluded from the default (hosted-runner) test preset.
    set_tests_properties(wgl PROPERTIES LABELS "gpu" TIMEOUT 180)
endif()

# GLX integration test: public API only, against the real shared library, with a raw GLX context on its own X
# connection as the "other library". Needs an X server (Xvfb is enough); exits 77 = skipped without one.
if(UNIX AND NOT APPLE AND TESSERA_BUILD_SHARED)
    add_executable(test_glx test_glx.cpp)
    target_include_directories(test_glx PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(test_glx PRIVATE Tessera::tessera Threads::Threads ${CMAKE_DL_LIBS})
    tessera_apply_options(test_glx)
    add_test(NAME glx COMMAND test_glx)
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
endif()

set_tests_properties(abi core header_c PROPERTIES TIMEOUT 60)
