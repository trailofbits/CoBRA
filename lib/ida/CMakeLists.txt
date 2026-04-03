if(NOT DEFINED IDA_SDK_DIR)
    message(FATAL_ERROR
        "IDA_SDK_DIR must be set when COBRA_BUILD_IDA_PLUGIN is ON.\n"
        "  cmake -DIDA_SDK_DIR=/path/to/ida-sdk/src ...")
endif()

add_library(ida-cobra MODULE
    ida-cobra.cpp
    MicrocodeDetector.cpp
    MicrocodeConverter.cpp
    Verifier.cpp
)

target_link_libraries(ida-cobra PRIVATE cobra-core)

target_include_directories(ida-cobra PRIVATE
    ${IDA_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# IDA SDK requires __EA64__ for 64-bit address support
target_compile_definitions(ida-cobra PRIVATE __EA64__=1)

set_target_properties(ida-cobra PROPERTIES
    PREFIX ""
    SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}"
)

# IDA provides symbols at runtime — don't link them
if(APPLE)
    target_link_options(ida-cobra PRIVATE -undefined dynamic_lookup)
elseif(UNIX)
    # IDA resolves SDK symbols at plugin load time; allow unresolved.
    target_link_options(ida-cobra PRIVATE -Wl,--unresolved-symbols=ignore-in-shared-libs)
endif()

install(TARGETS ida-cobra
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cobra
)
install(FILES ida-cobra.cfg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cobra
)
