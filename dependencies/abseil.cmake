# dependencies/abseil.cmake
# Abseil dependency: forwarding config (external) or source build.

if(USE_EXTERNAL_ABSEIL)
    find_package(absl REQUIRED)
    message(STATUS "Using external abseil")

    # Write a forwarding abslConfig.cmake.
    set(_absl_config_dir
        "${COBRA_INSTALL_PREFIX}/lib/cmake/absl")
    file(MAKE_DIRECTORY "${_absl_config_dir}")
    file(WRITE "${_absl_config_dir}/abslConfig.cmake"
        "# Forwarding config — delegates to system abseil\n"
        "include(\"${absl_DIR}/abslConfig.cmake\")\n"
    )

    cobra_mark_satisfied(abseil)
else()
    message(STATUS "Building abseil from source (20260107.1)")
    cobra_add_dependency(abseil
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 255c84dadd029fd8ad25c5efb5933e47beaa00c7 # 20260107.1
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DABSL_BUILD_TESTING=OFF
            -DABSL_PROPAGATE_CXX_STD=ON
    )
endif()
