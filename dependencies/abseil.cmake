# dependencies/abseil.cmake
# Abseil dependency: forwarding config (external) or source build.

if(USE_EXTERNAL_ABSEIL)
    find_package(absl REQUIRED)
    message(STATUS "Using external abseil")

    set(_absl_config_dir "${COBRA_INSTALL_PREFIX}/lib/cmake/absl")
    file(MAKE_DIRECTORY "${_absl_config_dir}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/config/abslConfig.cmake.in"
        "${_absl_config_dir}/abslConfig.cmake" @ONLY)

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
            -DABSL_ENABLE_INSTALL=ON
            -DABSL_PROPAGATE_CXX_STD=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    )
endif()
