# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Conan 2 recipe for Tessera. Build from a checkout:  conan create packaging/conan --build=missing  (see docs/releasing.md).

The sources are taken from the repository two directories up (`export_sources` below), so the recipe always matches the checkout.
"""
import os
import re

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class TesseraConan(ConanFile):
    name = "tessera"
    license = "Apache-2.0"
    url = "https://github.com/AndreasNilsson123/Tessera"
    homepage = "https://github.com/AndreasNilsson123/Tessera"
    description = "OpenGL / OpenGL ES context creation that coexists safely with other OpenGL users in the same process"
    topics = ("opengl", "opengles", "context", "wgl", "glx", "egl", "cgl")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False], "tools": [True, False]}
    default_options = {"shared": True, "fPIC": True, "tools": True}

    def set_version(self):
        header = os.path.join(self.recipe_folder, "..", "..", "include", "tessera", "tessera.h")
        text = open(header, encoding="utf-8").read()
        self.version = ".".join(re.search(r"#define TESSERA_VERSION_%s (\d+)" % p, text).group(1) for p in ("MAJOR", "MINOR", "PATCH"))

    def export_sources(self):
        top = os.path.join(self.recipe_folder, "..", "..")
        for pattern in ("CMakeLists.txt", "cmake/*", "include/*", "src/*", "tools/*", "LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            copy(self, pattern, src=top, dst=self.export_sources_folder)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["TESSERA_BUILD_SHARED"] = bool(self.options.shared)
        tc.variables["TESSERA_BUILD_TESTS"] = False
        tc.variables["TESSERA_BUILD_JAVA"] = False
        tc.variables["TESSERA_BUILD_TOOLS"] = bool(self.options.tools)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "Tessera")
        self.cpp_info.set_property("cmake_target_name", "Tessera::tessera")
        self.cpp_info.libs = ["tessera"]
        if not self.options.shared:
            self.cpp_info.defines = ["TESSERA_STATIC"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["dl", "pthread"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["gdi32", "user32"]
        if self.options.tools:
            self.buildenv_info.prepend_path("PATH", os.path.join(self.package_folder, "bin"))
