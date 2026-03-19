# dependencies/googletest.cmake
# GoogleTest dependency: forwarding config (external) or source build.

if(USE_EXTERNAL_GOOGLETEST)
    find_package(GTest REQUIRED)
    message(STATUS "Using external GoogleTest")

    # Write a forwarding GTestConfig.cmake.
    set(_gtest_config_dir
        "${COBRA_INSTALL_PREFIX}/lib/cmake/GTest")
    file(MAKE_DIRECTORY "${_gtest_config_dir}")
    file(WRITE "${_gtest_config_dir}/GTestConfig.cmake"
        "# Forwarding config — delegates to system GoogleTest\n"
        "include(\"${GTest_DIR}/GTestConfig.cmake\")\n"
    )

    cobra_mark_satisfied(googletest)
else()
    message(STATUS "Building GoogleTest from source (v1.16.0)")
    cobra_add_dependency(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.16.0
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DBUILD_GMOCK=OFF
            -DINSTALL_GTEST=ON
    )
endif()
