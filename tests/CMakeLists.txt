add_executable(test_abi test_abi.cpp)
target_link_libraries(test_abi PRIVATE Tessera::tessera)
tessera_apply_options(test_abi)
add_test(NAME abi COMMAND test_abi)

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
tessera_apply_options(test_core)
add_test(NAME core COMMAND test_core)
