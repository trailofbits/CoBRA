# dependencies/nanobind.cmake
# nanobind dependency: forwarding config (external) or source build.

set(_nanobind_config_dir "${COBRA_INSTALL_PREFIX}/lib/cmake/nanobind")
file(MAKE_DIRECTORY "${_nanobind_config_dir}")

if(USE_EXTERNAL_NANOBIND)
    find_package(nanobind CONFIG REQUIRED)
    message(STATUS "Using external nanobind")

    file(WRITE "${_nanobind_config_dir}/nanobind-config.cmake"
        "# Forwarding config - delegates to system nanobind\n"
        "include(\"${nanobind_DIR}/nanobind-config.cmake\")\n"
    )

    cobra_mark_satisfied(nanobind)
else()
    message(STATUS "Building nanobind from source (v2.12.0)")
    cobra_add_dependency(nanobind
        GIT_REPOSITORY https://github.com/wjakob/nanobind.git
        GIT_TAG 2a61ad2494d09fecb2e13322c1383342c299900d # v2.12.0
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        GIT_SUBMODULES_RECURSE ON
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DNB_TEST=OFF
            -DNB_CREATE_INSTALL_RULES=ON
            -DNB_USE_SUBMODULE_DEPS=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    )

    file(WRITE "${_nanobind_config_dir}/nanobind-config.cmake"
        "# Forwarding config - delegates to installed nanobind\n"
        "include(\"${COBRA_INSTALL_PREFIX}/nanobind/cmake/nanobind-config.cmake\")\n"
    )
endif()
