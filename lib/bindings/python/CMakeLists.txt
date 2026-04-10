find_package(Python 3.10 COMPONENTS Interpreter Development REQUIRED)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

if (NOT nanobind_DIR AND NOT nanobind_ROOT)
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT
  )
endif()
if (nanobind_ROOT AND NOT nanobind_DIR)
  set(nanobind_DIR "${nanobind_ROOT}")
endif()
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(
  cobra_mba
  PythonInterface.cpp
  PythonInterface.hpp
  CobraPython.cpp
  CobraPython.hpp
)
target_link_libraries(cobra_mba PRIVATE cobra-core)


