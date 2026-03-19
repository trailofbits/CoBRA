# dependencies/z3.cmake
# Z3 dependency: forwarding config (external) or source build.
# Only included when COBRA_ENABLE_Z3=ON.

if(USE_EXTERNAL_Z3)
    find_package(Z3 REQUIRED)
    message(STATUS "Using external Z3 ${Z3_VERSION}")

    # Write a forwarding Z3Config.cmake that re-exports the
    # variables discovered by find_package above.  System Z3
    # (e.g. from apt) is typically found via module mode and
    # has no Z3Config.cmake to delegate to, so we write the
    # variables directly.
    #
    # CMake's FindZ3 doesn't always populate Z3_INCLUDE_DIRS,
    # so we fall back to common system paths.
    if(NOT Z3_INCLUDE_DIRS)
        find_path(_z3_inc z3.h)
        if(_z3_inc)
            set(Z3_INCLUDE_DIRS "${_z3_inc}")
        endif()
    endif()

    set(_z3_config_dir
        "${COBRA_INSTALL_PREFIX}/lib/cmake/z3")
    file(MAKE_DIRECTORY "${_z3_config_dir}")
    file(WRITE "${_z3_config_dir}/Z3Config.cmake"
        "# Generated forwarding config for system Z3\n"
        "set(Z3_FOUND TRUE)\n"
        "set(Z3_INCLUDE_DIRS \"${Z3_INCLUDE_DIRS}\")\n"
        "set(Z3_LIBRARIES \"${Z3_LIBRARIES}\")\n"
        "set(Z3_LIBRARY \"${Z3_LIBRARIES}\")\n"
        "\n"
        "if(NOT TARGET z3::libz3)\n"
        "    add_library(z3::libz3 UNKNOWN IMPORTED)\n"
        "    set_target_properties(z3::libz3 PROPERTIES\n"
        "        IMPORTED_LOCATION \"${Z3_LIBRARIES}\"\n"
        "        INTERFACE_INCLUDE_DIRECTORIES \"${Z3_INCLUDE_DIRS}\"\n"
        "    )\n"
        "endif()\n"
    )

    cobra_mark_satisfied(z3)
else()
    message(STATUS "Building Z3 from source (z3-4.13.4)")
    cobra_add_dependency(z3
        GIT_REPOSITORY https://github.com/Z3Prover/z3.git
        GIT_TAG z3-4.13.4
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DZ3_BUILD_LIBZ3_SHARED=OFF
            -DZ3_BUILD_TEST_EXECUTABLES=OFF
            -DZ3_BUILD_PYTHON_BINDINGS=OFF
            -DZ3_ENABLE_EXAMPLE_TARGETS=OFF
    )
endif()
