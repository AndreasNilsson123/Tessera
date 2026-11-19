# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
cmake_minimum_required(VERSION 3.21)
project(consumer C)
find_package(Tessera CONFIG REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE Tessera::tessera)
