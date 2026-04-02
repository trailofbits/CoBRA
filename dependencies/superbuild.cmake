# dependencies/superbuild.cmake
# Shared infrastructure for the CoBRA superbuild.
# Included by dependencies/CMakeLists.txt before any dep modules.

include(ExternalProject)

# --- Install prefix ---
set(COBRA_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install"
    CACHE PATH "Common install prefix for all dependencies")
file(MAKE_DIRECTORY "${COBRA_INSTALL_PREFIX}")

# --- Common CMake arguments forwarded to every ExternalProject_Add ---
set(COBRA_COMMON_CMAKE_ARGS
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${COBRA_INSTALL_PREFIX}
    -DCMAKE_CXX_STANDARD=23
    -DCMAKE_CXX_STANDARD_REQUIRED=ON
    -DCMAKE_CXX_EXTENSIONS=OFF
)

# --- Build config validation ---
# Records compiler, platform, and build type on first run.
# Errors on subsequent runs if any value changed.
set(_config_file "${CMAKE_BINARY_DIR}/.build_config")
string(CONCAT _current_config
    "C_COMPILER=${CMAKE_C_COMPILER}\n"
    "CXX_COMPILER=${CMAKE_CXX_COMPILER}\n"
    "BUILD_TYPE=${CMAKE_BUILD_TYPE}\n"
    "SYSTEM=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}\n"
)

if(EXISTS "${_config_file}")
    file(READ "${_config_file}" _saved_config)
    if(NOT "${_saved_config}" STREQUAL "${_current_config}")
        message(FATAL_ERROR
            "Build configuration changed since last run.\n"
            "Saved:\n${_saved_config}\n"
            "Current:\n${_current_config}\n"
            "Use a fresh build directory or delete "
            "'${_config_file}' to reset.")
    endif()
else()
    file(WRITE "${_config_file}" "${_current_config}")
endif()

# --- Sequential chaining wrapper ---
# Wraps ExternalProject_Add so each dep automatically depends on
# the previously declared one. Deps build in declaration order.
# Only chains to deps that have real ExternalProject targets.
set(_COBRA_LAST_EP "" CACHE INTERNAL "Last ExternalProject target")

macro(cobra_add_dependency NAME)
    set(_dep_args ${ARGN})
    if(_COBRA_LAST_EP)
        ExternalProject_Add(${NAME}
            DEPENDS ${_COBRA_LAST_EP}
            ${_dep_args}
        )
    else()
        ExternalProject_Add(${NAME}
            ${_dep_args}
        )
    endif()
    set(_COBRA_LAST_EP "${NAME}" CACHE INTERNAL "Last ExternalProject target")
endmacro()

# Mark a dependency as satisfied without building it.
# Use for forwarding cases where the dep is system-installed.
# Does not affect chaining — only real ExternalProject targets chain.
macro(cobra_mark_satisfied NAME)
    message(STATUS "Dependency '${NAME}' satisfied externally")
endmacro()
