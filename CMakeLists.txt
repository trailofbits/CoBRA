cmake_minimum_required(VERSION 3.20)
project(cobra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(COBRA_BUILD_LLVM_PASS "Build the LLVM pass plugin" ON)
option(COBRA_BUILD_TESTS "Build tests" ON)
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
