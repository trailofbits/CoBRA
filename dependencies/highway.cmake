# dependencies/highway.cmake
# Highway SIMD library: portable vector intrinsics.

message(STATUS "Building highway from source (1.3.0)")
cobra_add_dependency(highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG ac0d5d297b13ab1b89f48484fc7911082d76a93f # 1.3.0
    GIT_SHALLOW ON
    GIT_PROGRESS ON
    CMAKE_ARGS
        ${COBRA_COMMON_CMAKE_ARGS}
        -DHWY_ENABLE_TESTS=OFF
        -DHWY_ENABLE_EXAMPLES=OFF
        -DHWY_ENABLE_CONTRIB=OFF
        -DBUILD_TESTING=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)
