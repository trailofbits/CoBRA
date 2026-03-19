add_executable(cobra-cli
    main.cpp
    ExprParser.cpp
)

target_link_libraries(cobra-cli PRIVATE cobra-core)
target_include_directories(cobra-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(Z3_FOUND)
    target_link_libraries(cobra-cli PRIVATE cobra-verify)
    target_compile_definitions(cobra-cli PRIVATE COBRA_HAS_Z3=1)
endif()
