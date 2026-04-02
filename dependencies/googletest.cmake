# dependencies/googletest.cmake
# GoogleTest dependency: forwarding config (external) or source build.

if(USE_EXTERNAL_GOOGLETEST)
    find_package(GTest REQUIRED)
    message(STATUS "Using external GoogleTest")

    set(_gtest_config_dir "${COBRA_INSTALL_PREFIX}/lib/cmake/GTest")
    file(MAKE_DIRECTORY "${_gtest_config_dir}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/config/GTestConfig.cmake.in"
        "${_gtest_config_dir}/GTestConfig.cmake" @ONLY)

    cobra_mark_satisfied(googletest)
else()
    message(STATUS "Building GoogleTest from source (v1.16.0)")
    cobra_add_dependency(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5 # v1.16.0
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DBUILD_GMOCK=OFF
            -DINSTALL_GTEST=ON
    )
endif()
