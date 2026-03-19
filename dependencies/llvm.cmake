# dependencies/llvm.cmake
# LLVM dependency: forwarding config (external) or source build.

set(LLVM_VERSION "llvmorg-21.0.0" CACHE STRING
    "LLVM Git tag for source builds (only used when USE_EXTERNAL_LLVM=OFF)")

if(USE_EXTERNAL_LLVM)
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Using external LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "  Config: ${LLVM_DIR}/LLVMConfig.cmake")

    # Write a forwarding LLVMConfig.cmake so the main project's
    # find_package(LLVM) transparently finds the system install.
    set(_llvm_config_dir
        "${COBRA_INSTALL_PREFIX}/lib/cmake/llvm")
    file(MAKE_DIRECTORY "${_llvm_config_dir}")
    file(WRITE "${_llvm_config_dir}/LLVMConfig.cmake"
        "# Forwarding config — delegates to system LLVM\n"
        "include(\"${LLVM_DIR}/LLVMConfig.cmake\")\n"
    )

    cobra_mark_satisfied(llvm)
else()
    message(STATUS "Building LLVM from source (${LLVM_VERSION})")
    cobra_add_dependency(llvm
        GIT_REPOSITORY https://github.com/llvm/llvm-project.git
        GIT_TAG "${LLVM_VERSION}"
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        SOURCE_SUBDIR llvm
        CMAKE_ARGS
            ${COBRA_COMMON_CMAKE_ARGS}
            -DLLVM_TARGETS_TO_BUILD=Native
            -DLLVM_ENABLE_PROJECTS=
            -DLLVM_BUILD_TOOLS=OFF
            -DLLVM_INCLUDE_TESTS=OFF
            -DLLVM_INCLUDE_EXAMPLES=OFF
            -DLLVM_INCLUDE_BENCHMARKS=OFF
    )
endif()
