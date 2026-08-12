if(NOT DEFINED IDA_SDK_DIR)
    message(FATAL_ERROR
        "IDA_SDK_DIR must be set when COBRA_BUILD_IDA_PLUGIN is ON.\n"
        "  cmake -DIDA_SDK_DIR=/path/to/ida-sdk/src ...")
endif()

# Detect platform-specific IDA stub library directory
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_ida_lib_suffix "arm64_mac_64")
    else()
        set(_ida_lib_suffix "x64_mac_64")
    endif()
    set(_ida_lib_name "libida.dylib")
elseif(WIN32)
    set(_ida_lib_suffix "x64_win_64")
    set(_ida_lib_name "ida.lib")
else()
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(_ida_lib_suffix "arm64_linux_64")
    else()
        set(_ida_lib_suffix "x64_linux_64")
    endif()
    set(_ida_lib_name "libida.so")
endif()

set(_ida_lib_path "${IDA_SDK_DIR}/lib/${_ida_lib_suffix}/${_ida_lib_name}")
if(NOT EXISTS "${_ida_lib_path}")
    message(FATAL_ERROR "IDA stub library not found: ${_ida_lib_path}")
endif()

add_library(ida-cobra MODULE
    ida-cobra.cpp
    MicrocodeDetector.cpp
    MicrocodeConverter.cpp
    Verifier.cpp
)

target_link_libraries(ida-cobra PRIVATE cobra-core "${_ida_lib_path}")

target_include_directories(ida-cobra PRIVATE
    ${IDA_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# IDA SDK platform defines
target_compile_definitions(ida-cobra PRIVATE
    __EA64__=1
    $<$<PLATFORM_ID:Windows>:__NT__>
    $<$<PLATFORM_ID:Linux>:__LINUX__>
    $<$<PLATFORM_ID:Darwin>:__MAC__>
)

set_target_properties(ida-cobra PROPERTIES
    PREFIX ""
    SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
)

install(TARGETS ida-cobra
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cobra
)
install(FILES ida-cobra.cfg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cobra
)
