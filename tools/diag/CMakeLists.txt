# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
# tessera-diag: capability report built on the public API only (tesseraProbe).
include(GNUInstallDirs)
add_executable(tessera-diag tessera_diag.c)
target_link_libraries(tessera-diag PRIVATE Tessera::tessera)
tessera_apply_options(tessera-diag)
if(WIN32 AND TESSERA_BUILD_SHARED)
    add_custom_command(TARGET tessera-diag POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera> $<TARGET_FILE_DIR:tessera-diag>)
endif()
install(TARGETS tessera-diag RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TESSERA_BUILD_TESTS)
    # Smoke tests: the tool exits 0 when some backend can create a context and the JSON has its top-level structure. They need a
    # real driver (exit 1 = no backend works), hence the gpu label; the usage check needs none.
    add_test(NAME diag_text COMMAND tessera-diag --quick)
    add_test(NAME diag_json COMMAND tessera-diag --json --quick)
    set_tests_properties(diag_text diag_json PROPERTIES LABELS "gpu" TIMEOUT 120)
    set_tests_properties(diag_json PROPERTIES PASS_REGULAR_EXPRESSION "\"backends\":")
    add_test(NAME diag_usage COMMAND tessera-diag --bogus)
    set_tests_properties(diag_usage PROPERTIES PASS_REGULAR_EXPRESSION "usage:")
endif()
