add_library(cobra-verify STATIC
    Z3Verifier.cpp
)

target_link_libraries(cobra-verify PUBLIC cobra-core)
target_link_libraries(cobra-verify PRIVATE z3::libz3)
target_include_directories(cobra-verify PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
