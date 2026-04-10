cmake_minimum_required(VERSION 3.20)
project(cobra VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_EXPORT_COMPILE_COMMANDS AND UNIX)
    set(_cobra_cc_src "${CMAKE_SOURCE_DIR}/compile_commands.json")
    set(_cobra_cc_bin "${CMAKE_BINARY_DIR}/compile_commands.json")
    if (NOT EXISTS "${_cobra_cc_src}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${_cobra_cc_bin}" "${_cobra_cc_src}"
            OUTPUT_QUIET ERROR_QUIET
        )
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# --- Compiler warnings and hardening ---
set(_cobra_gcc_clang $<CXX_COMPILER_ID:GNU,Clang,AppleClang>)
set(_cobra_msvc $<CXX_COMPILER_ID:MSVC>)
set(_cobra_linux $<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<PLATFORM_ID:Linux>>)

# Warnings
add_compile_options(
    $<${_cobra_gcc_clang}:-Wall>
    $<${_cobra_gcc_clang}:-Wextra>
    $<${_cobra_gcc_clang}:-Wpedantic>
    $<${_cobra_gcc_clang}:-Wconversion>
    $<${_cobra_gcc_clang}:-Wsign-conversion>
    # GCC/Clang warn on designated inits that omit defaulted fields — suppress
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>
    $<${_cobra_msvc}:/W4>
)

# Runtime hardening (GCC/Clang)
add_compile_options(
    $<${_cobra_gcc_clang}:-fstack-protector-strong>
    $<${_cobra_gcc_clang}:-ftrivial-auto-var-init=zero>
    $<$<AND:${_cobra_gcc_clang},$<NOT:$<CONFIG:Debug>>>:-D_FORTIFY_SOURCE=3>
    $<${_cobra_linux}:-fstack-clash-protection>
    $<$<AND:${_cobra_linux},$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>>:-fcf-protection=full>
)

# Runtime hardening (MSVC)
add_compile_options(
    $<${_cobra_msvc}:/sdl>
    $<${_cobra_msvc}:/guard:cf>
    $<${_cobra_msvc}:/GS>
)

# libc++ hardened container bounds checking (~0.3% overhead)
add_compile_options($<$<CXX_COMPILER_ID:Clang,AppleClang>:-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)

# Linker hardening
add_link_options(
    $<${_cobra_linux}:-Wl,-z,relro,-z,now,-z,noexecstack>
    $<${_cobra_msvc}:/DYNAMICBASE>
    $<${_cobra_msvc}:/NXCOMPAT>
    $<${_cobra_msvc}:/CETCOMPAT>
    $<${_cobra_msvc}:/guard:cf>
)

option(COBRA_BUILD_LLVM_PASS "Build the LLVM pass plugin (requires LLVM 19-22)" OFF)
option(COBRA_BUILD_TESTS "Build tests (requires GoogleTest in prefix)" OFF)
option(COBRA_BUILD_PYTHON_BINDINGS "Build Python bindings (nanobind)" OFF)
option(COBRA_ENABLE_TRACE "Enable detailed pipeline tracing to stderr (debug builds)" OFF)
option(COBRA_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)

if(COBRA_BUILD_LLVM_PASS)
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
endif()

find_package(Z3 QUIET CONFIG)
if(Z3_FOUND)
    message(STATUS "Found Z3 ${Z3_VERSION}")
else()
    message(STATUS "Z3 not found — cobra-verify will not be built")
endif()

add_subdirectory(lib/core)
add_subdirectory(tools/cobra-cli)

if(COBRA_ENABLE_TRACE)
    target_compile_definitions(cobra-core PUBLIC COBRA_ENABLE_TRACE)
endif()

if(COBRA_ENABLE_TRACY)
    include(cmake/Tracy.cmake)
    target_compile_definitions(cobra-core PUBLIC COBRA_ENABLE_TRACY)
    target_link_libraries(cobra-core PUBLIC TracyClient)
endif()

if(Z3_FOUND)
    add_subdirectory(lib/verify)
endif()

if(COBRA_BUILD_LLVM_PASS)
    add_subdirectory(lib/llvm)
endif()

if(COBRA_BUILD_PYTHON_BINDINGS)
    add_subdirectory(lib/bindings/python)
endif()

if(COBRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# --- Install rules ---

# Public headers
install(DIRECTORY include/cobra
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Collect installed targets for the package config export set
set(_cobra_install_targets cobra-core cobra-cli)
if(Z3_FOUND)
    list(APPEND _cobra_install_targets cobra-verify)
endif()

install(TARGETS ${_cobra_install_targets}
    EXPORT CobraTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# LLVM pass plugin installs separately (MODULE, not in export set)
if(COBRA_BUILD_LLVM_PASS)
    install(TARGETS CobraPass
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cobra
    )
endif()

# CMake package config
set(_cobra_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/cobra)

install(EXPORT CobraTargets
    FILE CobraTargets.cmake
    NAMESPACE cobra::
    DESTINATION ${_cobra_cmake_dir}
)

configure_package_config_file(
    cmake/CobraConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CobraConfig.cmake
    INSTALL_DESTINATION ${_cobra_cmake_dir}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CobraConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CobraConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CobraConfigVersion.cmake
    DESTINATION ${_cobra_cmake_dir}
)
