# Java bindings: one multi-release jar (dev.tessera) with the native library embedded.
#
#   base classes + JNI binding   compiled for Java 11   -> the jar root
#   FFM (java.lang.foreign)      compiled for Java 22   -> META-INF/versions/22 (only when the build JDK is 22+)
#
# Plain javac/jar are driven from here (no Maven/Gradle needed). The JNI glue lives inside the tessera shared library
# itself (bindings/java/src/jni), so the same native file serves both bindings.
#
# Two build-system rules that cost real debugging time, kept here so they are not rediscovered:
#  * Each stage is its own custom target, ordered with add_dependencies, and always runs (javac/jar take about a second).
#    An OUTPUT shared between several targets is re-run by each of them, in parallel, under the Visual Studio generator.
#  * Multi-entry classpaths never go on a command line (';' does not survive CMake's quoting on Windows). They live in
#    @argfiles, which both javac and java read (the java launcher since JDK 9).

if(NOT TESSERA_BUILD_SHARED)
    message(FATAL_ERROR "TESSERA_BUILD_JAVA needs TESSERA_BUILD_SHARED=ON (JNI and FFM load a shared library)")
endif()

set(JAVA_CLASSES ${CMAKE_CURRENT_BINARY_DIR}/classes)
set(JAVA_STAGE ${CMAKE_CURRENT_BINARY_DIR}/stage)
set(JAVA_ARGS ${CMAKE_CURRENT_BINARY_DIR}/args)
set(TESSERA_JAR ${CMAKE_CURRENT_BINARY_DIR}/tessera.jar)
file(MAKE_DIRECTORY ${JAVA_ARGS})

file(GLOB_RECURSE TESSERA_JAVA_MAIN CONFIGURE_DEPENDS src/main/java/*.java)
file(GLOB_RECURSE TESSERA_JAVA_FFM CONFIGURE_DEPENDS src/ffm/java/*.java)
file(GLOB_RECURSE TESSERA_JAVA_TEST CONFIGURE_DEPENDS src/test/java/*.java)

# Where the embedded library goes inside the jar, and what it is called there (matches NativeLoader).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|x64)$")
    set(JAVA_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64)$")
    set(JAVA_ARCH aarch64)
else()
    set(JAVA_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(WIN32)
    set(JAVA_OS windows)
    set(JAVA_NATIVE_NAME tessera.dll)
    set(JAVA_CP_SEP ";")
elseif(APPLE)
    set(JAVA_OS macos)
    set(JAVA_NATIVE_NAME libtessera.dylib)
    set(JAVA_CP_SEP ":")
else()
    set(JAVA_OS linux)
    set(JAVA_NATIVE_NAME libtessera.so)
    set(JAVA_CP_SEP ":")
endif()
set(JAVA_NATIVE_DIR ${JAVA_STAGE}/natives/${JAVA_OS}-${JAVA_ARCH})

set(JAVAC_COMMON -encoding UTF-8 -Xlint:all -Werror)

# javac with a classpath: `javac_args(<file> <classpath...>)` writes "-cp <joined>" for @file.
function(tessera_cp_file file)
    string(JOIN "${JAVA_CP_SEP}" joined ${ARGN})
    file(WRITE ${file} "-cp\n\"${joined}\"\n")
endfunction()

# 1. Base classes (Java 11) and, when the build JDK can target it, the FFM binding (Java 22).
set(_class_commands
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/main
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/main
    COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${JAVAC_COMMON} -d ${JAVA_CLASSES}/main ${TESSERA_JAVA_MAIN})
set(JAVA_JAR_INPUTS -C ${JAVA_CLASSES}/main . -C ${JAVA_STAGE} .)
if(Java_VERSION_MAJOR GREATER_EQUAL 22)
    list(APPEND _class_commands
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/ffm
        COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/ffm
        # [restricted] lint is expected here: calling restricted FFM methods is the point; --enable-native-access is the runtime opt-in.
        COMMAND ${Java_JAVAC_EXECUTABLE} --release 22 -encoding UTF-8 -Xlint:all,-restricted -Werror -cp ${JAVA_CLASSES}/main
                -d ${JAVA_CLASSES}/ffm ${TESSERA_JAVA_FFM})
    list(APPEND JAVA_JAR_INPUTS --release 22 -C ${JAVA_CLASSES}/ffm .)
else()
    message(STATUS "Java ${Java_VERSION}: the FFM binding (Java 22+) is not built; the jar will use JNI only")
endif()
add_custom_target(tessera_java_classes ${_class_commands} COMMENT "javac: dev.tessera" VERBATIM)

# 2. The jar, with the native library embedded.
add_custom_target(tessera_java_jar
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_NATIVE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tessera> ${JAVA_NATIVE_DIR}/${JAVA_NATIVE_NAME}
    COMMAND ${Java_JAR_EXECUTABLE} --create --file ${TESSERA_JAR} ${JAVA_JAR_INPUTS}
    COMMENT "jar: tessera.jar" VERBATIM)
add_dependencies(tessera_java_jar tessera tessera_java_classes)

# 3. Tests (compiled for Java 11 so they can run on every supported runtime).
add_custom_target(tessera_java_tests
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/test
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/test
    COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${JAVAC_COMMON} -cp ${JAVA_CLASSES}/main -d ${JAVA_CLASSES}/test ${TESSERA_JAVA_TEST}
    COMMENT "javac: dev.tessera tests" VERBATIM)
add_dependencies(tessera_java_tests tessera_java_classes)

add_custom_target(tessera_java ALL)
add_dependencies(tessera_java tessera_java_jar tessera_java_tests)

# JDK 24+ warns about the restricted System.load unless native access is enabled; the flag exists since Java 17.
set(JAVA_NATIVE_ACCESS "")
if(Java_VERSION_MAJOR GREATER_EQUAL 22)
    set(JAVA_NATIVE_ACCESS --enable-native-access=ALL-UNNAMED)
endif()

# Registers a test that runs `java @<argfile>`. The argfile (generated, so it can use generator expressions) carries the
# JVM options, the classpath and the main class: nothing with a path separator ever reaches a command line.
#   tessera_java_test(<name> <main class> BINDING jni|ffm [NATIVE_LIB] CLASSPATH <entries...> [NATIVE_ACCESS])
function(tessera_java_test name main_class)
    cmake_parse_arguments(T "NATIVE_LIB;NATIVE_ACCESS" "BINDING" "CLASSPATH;MODULE_PATH;JVM_OPTIONS" ${ARGN})
    string(JOIN "${JAVA_CP_SEP}" classpath ${T_CLASSPATH})
    set(content "")
    if(T_NATIVE_ACCESS)
        string(APPEND content "--enable-native-access=ALL-UNNAMED\n")
    endif()
    if(T_MODULE_PATH)
        string(JOIN "${JAVA_CP_SEP}" modulepath ${T_MODULE_PATH})
        string(APPEND content "--module-path\n\"${modulepath}\"\n--add-modules\njavafx.graphics\n")
    endif()
    foreach(option ${T_JVM_OPTIONS})
        string(APPEND content "${option}\n")
    endforeach()
    string(APPEND content "-Dtessera.binding=${T_BINDING}\n")
    if(T_NATIVE_LIB)
        string(APPEND content "\"-Dtessera.library.path=$<TARGET_FILE:tessera>\"\n")
    endif()
    string(APPEND content "-cp\n\"${classpath}\"\n${main_class}\n")
    # One file per configuration: the content names this configuration's library (a multi-config generator would otherwise
    # be asked to write the same file with different content).
    file(GENERATE OUTPUT ${JAVA_ARGS}/${name}-$<CONFIG>.args CONTENT "${content}")
    add_test(NAME ${name} COMMAND ${Java_JAVA_EXECUTABLE} @${JAVA_ARGS}/${name}-$<CONFIG>.args)
endfunction()

if(BUILD_TESTING AND TESSERA_BUILD_TESTS)
    set(_cp ${TESSERA_JAR} ${JAVA_CLASSES}/test)
    set(_native_access "")
    if(Java_VERSION_MAJOR GREATER_EQUAL 22)
        set(_native_access NATIVE_ACCESS)
    endif()

    # Against the library as built (-Dtessera.library.path), through JNI.
    tessera_java_test(java_jni dev.tessera.SelfTest BINDING jni NATIVE_LIB CLASSPATH ${_cp} ${_native_access})
    # Through the copy embedded in the jar (extract, checksum, load): no property, so the loader must find its own file.
    tessera_java_test(java_jni_embedded dev.tessera.SelfTest BINDING jni CLASSPATH ${_cp} ${_native_access})
    tessera_java_test(java_shutdown dev.tessera.ShutdownTest BINDING jni CLASSPATH ${_cp} ${_native_access})
    set(JAVA_TESTS java_jni java_jni_embedded java_shutdown)
    set(JAVA_SHUTDOWN_TESTS java_shutdown)
    if(Java_VERSION_MAJOR GREATER_EQUAL 22)
        tessera_java_test(java_ffm dev.tessera.SelfTest BINDING ffm NATIVE_LIB NATIVE_ACCESS CLASSPATH ${_cp})
        tessera_java_test(java_ffm_embedded dev.tessera.SelfTest BINDING ffm NATIVE_ACCESS CLASSPATH ${_cp})
        tessera_java_test(java_shutdown_ffm dev.tessera.ShutdownTest BINDING ffm NATIVE_ACCESS CLASSPATH ${_cp})
        list(APPEND JAVA_TESTS java_ffm java_ffm_embedded java_shutdown_ffm)
        list(APPEND JAVA_SHUTDOWN_TESTS java_shutdown_ffm)
    endif()
    # They need a real OpenGL driver, like the other integration tests.
    set_tests_properties(${JAVA_TESTS} PROPERTIES LABELS "gpu;java" TIMEOUT 180)
    set_tests_properties(${JAVA_SHUTDOWN_TESTS} PROPERTIES TIMEOUT 60) # the JVM must exit promptly with contexts still alive
endif()

# ---- Optional LWJGL integration (adapter jar + a rendering test) -----------------------------------------------------
# Built only when the LWJGL jars are present: python scripts/fetch-java-deps.py   (or -DTESSERA_JAVA_DEPS_DIR=<dir>)
option(TESSERA_JAVA_STRESS "Also register the long JavaFX coexistence run (label stress)" OFF)
set(TESSERA_JAVA_STRESS_FRAMES 6000 CACHE STRING "Frames of the JavaFX stress run")
set(TESSERA_JAVA_DEPS_DIR "${PROJECT_SOURCE_DIR}/third_party/java" CACHE PATH "Directory holding the optional third-party jars (LWJGL, JavaFX)")
file(GLOB TESSERA_LWJGL_JARS CONFIGURE_DEPENDS ${TESSERA_JAVA_DEPS_DIR}/lwjgl-*.jar)
set(_have_lwjgl_api FALSE)
set(_have_lwjgl_gl FALSE)
set(_have_lwjgl_glfw FALSE)
foreach(_jar ${TESSERA_LWJGL_JARS})
    if(_jar MATCHES "/lwjgl-[0-9.]+\\.jar$")
        set(_have_lwjgl_api TRUE)
    endif()
    if(_jar MATCHES "/lwjgl-opengl-[0-9.]+\\.jar$")
        set(_have_lwjgl_gl TRUE)
    endif()
    if(_jar MATCHES "/lwjgl-glfw-[0-9.]+\\.jar$")
        set(_have_lwjgl_glfw TRUE)
    endif()
endforeach()

if(_have_lwjgl_api AND _have_lwjgl_gl)
    file(GLOB_RECURSE TESSERA_JAVA_LWJGL CONFIGURE_DEPENDS src/lwjgl/java/*.java)
    file(GLOB_RECURSE TESSERA_JAVA_LWJGL_TEST CONFIGURE_DEPENDS src/lwjgl-test/java/*.java)
    set(TESSERA_LWJGL_JAR ${CMAKE_CURRENT_BINARY_DIR}/tessera-lwjgl.jar)
    tessera_cp_file(${JAVA_ARGS}/lwjgl-compile.args ${TESSERA_LWJGL_JARS} ${JAVA_CLASSES}/main)
    tessera_cp_file(${JAVA_ARGS}/lwjgl-test-compile.args ${TESSERA_LWJGL_JARS} ${JAVA_CLASSES}/main ${TESSERA_LWJGL_JAR})

    add_custom_target(tessera_java_lwjgl ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/lwjgl
        COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/lwjgl
        COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${JAVAC_COMMON} @${JAVA_ARGS}/lwjgl-compile.args
                -d ${JAVA_CLASSES}/lwjgl ${TESSERA_JAVA_LWJGL}
        COMMAND ${Java_JAR_EXECUTABLE} --create --file ${TESSERA_LWJGL_JAR} -C ${JAVA_CLASSES}/lwjgl .
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/lwjgl-test
        COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/lwjgl-test
        COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${JAVAC_COMMON} @${JAVA_ARGS}/lwjgl-test-compile.args
                -d ${JAVA_CLASSES}/lwjgl-test ${TESSERA_JAVA_LWJGL_TEST}
        COMMENT "javac: dev.tessera.lwjgl adapter and test" VERBATIM)
    add_dependencies(tessera_java_lwjgl tessera_java_classes)

    if(BUILD_TESTING AND TESSERA_BUILD_TESTS)
        set(_lwjgl_cp ${TESSERA_JAR} ${TESSERA_LWJGL_JAR} ${JAVA_CLASSES}/lwjgl-test ${TESSERA_LWJGL_JARS})
        tessera_java_test(java_lwjgl dev.tessera.lwjgl.LwjglTest BINDING jni NATIVE_LIB CLASSPATH ${_lwjgl_cp} ${_native_access})
        set(_lwjgl_tests java_lwjgl)
        if(Java_VERSION_MAJOR GREATER_EQUAL 22)
            tessera_java_test(java_lwjgl_ffm dev.tessera.lwjgl.LwjglTest BINDING ffm NATIVE_LIB NATIVE_ACCESS CLASSPATH ${_lwjgl_cp})
            list(APPEND _lwjgl_tests java_lwjgl_ffm)
        endif()
        set_tests_properties(${_lwjgl_tests} PROPERTIES LABELS "gpu;java;lwjgl" TIMEOUT 180)
        # GLFW (through LWJGL) in the same process as Tessera: its own window and context, Tessera around it and in its windows.
        # Exits 77 (skipped) where GLFW cannot open a window at all (no display).
        if(_have_lwjgl_glfw)
            tessera_java_test(java_glfw dev.tessera.lwjgl.GlfwCoexistence BINDING jni NATIVE_LIB CLASSPATH ${_lwjgl_cp} ${_native_access})
            set(_glfw_tests java_glfw)
            if(Java_VERSION_MAJOR GREATER_EQUAL 22)
                tessera_java_test(java_glfw_ffm dev.tessera.lwjgl.GlfwCoexistence BINDING ffm NATIVE_LIB NATIVE_ACCESS CLASSPATH ${_lwjgl_cp})
                list(APPEND _glfw_tests java_glfw_ffm)
            endif()
            set_tests_properties(${_glfw_tests} PROPERTIES LABELS "gpu;java;lwjgl;glfw" TIMEOUT 180 SKIP_RETURN_CODE 77)
        endif()
        # Java2D's OpenGL pipeline (WGL on Windows, GLX on Linux) inside the same JVM: AWT/Swing owns GL contexts too.
        # Skips (77) where the pipeline does not come up (no display, or a driver Java2D does not accept).
        tessera_java_test(java_awt_opengl dev.tessera.lwjgl.AwtCoexistence BINDING jni NATIVE_LIB CLASSPATH ${_lwjgl_cp}
            JVM_OPTIONS -Dsun.java2d.opengl=True ${_native_access})
        set(_awt_tests java_awt_opengl)
        set_tests_properties(${_awt_tests} PROPERTIES LABELS "gpu;java;lwjgl;awt" TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()
else()
    message(STATUS "LWJGL jars not found in ${TESSERA_JAVA_DEPS_DIR}: skipping the LWJGL adapter and its test (run scripts/fetch-java-deps.py)")
endif()

# ---- Optional JavaFX coexistence test ------------------------------------------------------------------------------
# JavaFX and Tessera/LWJGL in one JVM, both drawing: python scripts/fetch-java-deps.py   (needs the LWJGL jars as well)
file(GLOB TESSERA_JAVAFX_JARS CONFIGURE_DEPENDS ${TESSERA_JAVA_DEPS_DIR}/javafx-*.jar)
if(TARGET tessera_java_lwjgl AND TESSERA_JAVAFX_JARS)
    file(GLOB_RECURSE TESSERA_JAVA_FX_TEST CONFIGURE_DEPENDS src/javafx-test/java/*.java)
    string(JOIN "${JAVA_CP_SEP}" _fx_module_path ${TESSERA_JAVAFX_JARS})
    string(JOIN "${JAVA_CP_SEP}" _fx_compile_cp ${TESSERA_LWJGL_JARS} ${JAVA_CLASSES}/main ${TESSERA_LWJGL_JAR})
    file(WRITE ${JAVA_ARGS}/javafx-compile.args "--module-path\n\"${_fx_module_path}\"\n--add-modules\njavafx.graphics\n-cp\n\"${_fx_compile_cp}\"\n")
    add_custom_target(tessera_java_javafx ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${JAVA_CLASSES}/javafx-test
        COMMAND ${CMAKE_COMMAND} -E make_directory ${JAVA_CLASSES}/javafx-test
        COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${JAVAC_COMMON} @${JAVA_ARGS}/javafx-compile.args
                -d ${JAVA_CLASSES}/javafx-test ${TESSERA_JAVA_FX_TEST}
        COMMENT "javac: JavaFX coexistence test" VERBATIM)
    add_dependencies(tessera_java_javafx tessera_java_lwjgl)

    if(BUILD_TESTING AND TESSERA_BUILD_TESTS)
        set(_fx_cp ${TESSERA_JAR} ${TESSERA_LWJGL_JAR} ${JAVA_CLASSES}/javafx-test ${TESSERA_LWJGL_JARS})
        # Which JavaFX pipelines exist depends on the platform: Windows ships Direct3D (a non-GL host) and software only;
        # the OpenGL pipeline (es2, JavaFX's own GLX context: the hard case, two OpenGL users in one process) exists on Linux.
        # A pipeline that cannot start makes JavaFX fall back to software SILENTLY, which would turn the test into a different
        # test than its name says, so a fallback is a failure (prism.verbose prints it).
        if(WIN32)
            set(_fx_pipelines d3d sw)
        elseif(APPLE)
            set(_fx_pipelines sw)
        else()
            set(_fx_pipelines es2 sw)
        endif()
        foreach(_pipeline ${_fx_pipelines})
            # JavaFX only accepts GPUs on its own allow-list for es2 ("Failed Graphics Hardware Qualifier check") and rejects
            # Mesa's software and D3D12-over-WSL renderers, which are all a CI runner or WSL has. prism.forceGPU skips that
            # list; the pipeline is still JavaFX's real OpenGL one (the test output shows ES2Pipeline and its frame counts).
            set(_fx_extra "")
            if(_pipeline STREQUAL "es2")
                set(_fx_extra -Dprism.forceGPU=true)
            endif()
            tessera_java_test(java_javafx_${_pipeline} dev.tessera.javafx.FxCoexistence BINDING jni NATIVE_LIB
                CLASSPATH ${_fx_cp} MODULE_PATH ${TESSERA_JAVAFX_JARS}
                JVM_OPTIONS -Dprism.order=${_pipeline} -Dprism.verbose=true ${_fx_extra} ${_native_access})
            set_tests_properties(java_javafx_${_pipeline} PROPERTIES LABELS "gpu;java;javafx" TIMEOUT 120)
            if(NOT _pipeline STREQUAL "sw")
                set_tests_properties(java_javafx_${_pipeline} PROPERTIES FAIL_REGULAR_EXPRESSION "Fallback to Prism SW pipeline")
            endif()
        endforeach()
        # The same test for minutes instead of seconds on the pipeline that shares a GL API with Tessera (Direct3D on Windows is the
        # non-GL control): -DTESSERA_JAVA_STRESS=ON adds it, label `stress`. ~4 frames per 100 ms: 6000 frames is about 2.5 minutes.
        if(TESSERA_JAVA_STRESS)
            list(GET _fx_pipelines 0 _stress_pipeline)
            set(_stress_extra "")
            if(_stress_pipeline STREQUAL "es2")
                set(_stress_extra -Dprism.forceGPU=true)
            endif()
            tessera_java_test(java_javafx_${_stress_pipeline}_stress dev.tessera.javafx.FxCoexistence BINDING jni NATIVE_LIB
                CLASSPATH ${_fx_cp} MODULE_PATH ${TESSERA_JAVAFX_JARS}
                JVM_OPTIONS -Dprism.order=${_stress_pipeline} -Dprism.verbose=true -Dfxtest.frames=${TESSERA_JAVA_STRESS_FRAMES} ${_stress_extra} ${_native_access})
            set_tests_properties(java_javafx_${_stress_pipeline}_stress PROPERTIES LABELS "gpu;java;javafx;stress" TIMEOUT 3000
                FAIL_REGULAR_EXPRESSION "Fallback to Prism SW pipeline")
        endif()
    endif()
else()
    message(STATUS "JavaFX jars not found in ${TESSERA_JAVA_DEPS_DIR}: skipping the JavaFX coexistence test (run scripts/fetch-java-deps.py)")
endif()
