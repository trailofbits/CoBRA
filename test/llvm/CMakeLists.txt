# LLVM pass plugin FileCheck tests.
# Each .ll file uses CHECK directives validated by FileCheck.

find_program(FILECHECK_BIN FileCheck
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_INSTALL_PREFIX}/bin)
find_program(OPT_BIN opt
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_INSTALL_PREFIX}/bin)

if(NOT FILECHECK_BIN OR NOT OPT_BIN)
    message(STATUS "opt or FileCheck not found — skipping LLVM pass tests")
    return()
endif()

message(STATUS "LLVM pass tests: opt=${OPT_BIN} FileCheck=${FILECHECK_BIN}")

function(cobra_add_ll_test TEST_FILE)
    get_filename_component(TEST_NAME "${TEST_FILE}" NAME_WE)
    set(FULL_NAME "LLVMPass.${TEST_NAME}")
    set(FULL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE}")

    add_test(
        NAME "${FULL_NAME}"
        COMMAND sh -c "${OPT_BIN} -load-pass-plugin=$<TARGET_FILE:CobraPass> -passes=cobra-simplify -S '${FULL_PATH}' | ${FILECHECK_BIN} '${FULL_PATH}'"
    )
    set_tests_properties("${FULL_NAME}" PROPERTIES LABELS "llvm")
endfunction()

file(GLOB LL_TEST_FILES "*.ll")
foreach(LL_FILE ${LL_TEST_FILES})
    get_filename_component(LL_NAME "${LL_FILE}" NAME)
    cobra_add_ll_test("${LL_NAME}")
endforeach()
