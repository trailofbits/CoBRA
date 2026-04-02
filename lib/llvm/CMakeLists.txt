if(NOT LLVM_FOUND)
    message(FATAL_ERROR "CobraPass requires LLVM — set COBRA_BUILD_LLVM_PASS=OFF to skip")
endif()

add_library(CobraPass MODULE
    CobraPass.cpp
    MBADetector.cpp
    IRReconstructor.cpp
)

target_link_libraries(CobraPass PRIVATE cobra-core)

target_include_directories(CobraPass PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(CobraPass PRIVATE ${LLVM_DEFINITIONS})

# Don't link LLVM libraries — loaded into opt/clang which provides them
set_target_properties(CobraPass PROPERTIES
    PREFIX ""
    SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}"
)

if(APPLE)
    target_link_options(CobraPass PRIVATE -undefined dynamic_lookup)
endif()

if(Z3_FOUND)
    target_link_libraries(CobraPass PRIVATE cobra-verify)
    target_compile_definitions(CobraPass PRIVATE COBRA_HAS_Z3=1)
endif()
