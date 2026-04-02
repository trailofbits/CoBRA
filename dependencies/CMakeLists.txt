# dependencies/CMakeLists.txt
# Standalone superbuild project for CoBRA dependencies.
#
# Usage:
#   cmake -S dependencies -B build-deps [options]
#   cmake --build build-deps
#
# Options:
#   USE_EXTERNAL_LLVM       (default ON)  - Use system LLVM vs build from source
#   USE_EXTERNAL_HIGHWAY    (default OFF) - Use system highway vs build from source
#   COBRA_BUILD_TESTS       (default OFF) - Build GoogleTest for tests
#   USE_EXTERNAL_GOOGLETEST (default OFF) - Use system GoogleTest vs build from source
#   COBRA_ENABLE_Z3         (default OFF) - Enable Z3 dependency
#   USE_EXTERNAL_Z3         (default OFF) - Use system Z3 vs build from source

cmake_minimum_required(VERSION 3.20)
project(cobra-dependencies LANGUAGES C CXX)

option(USE_EXTERNAL_LLVM "Use system LLVM instead of building from source" ON)
option(USE_EXTERNAL_ABSEIL "Use system abseil instead of building from source" OFF)
option(USE_EXTERNAL_HIGHWAY "Use system highway instead of building from source" OFF)
option(COBRA_BUILD_TESTS "Build GoogleTest for CoBRA tests" OFF)
option(USE_EXTERNAL_GOOGLETEST "Use system GoogleTest instead of building from source" OFF)
option(COBRA_ENABLE_Z3 "Enable Z3 dependency (requires lib/verify to be implemented)" OFF)
option(USE_EXTERNAL_Z3 "Use system Z3 instead of building from source" OFF)

include(superbuild.cmake)

include(abseil.cmake)
include(highway.cmake)
include(llvm.cmake)

if(COBRA_BUILD_TESTS)
    include(googletest.cmake)
endif()

if(COBRA_ENABLE_Z3)
    include(z3.cmake)
endif()
