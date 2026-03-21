find_package(GTest REQUIRED)
include(GoogleTest)

function(cobra_add_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE cobra-core GTest::gtest_main)
    gtest_discover_tests(${TEST_NAME})
endfunction()

# Core tests
cobra_add_test(test_result core/test_result.cpp)
cobra_add_test(test_bitwidth core/test_bitwidth.cpp)
cobra_add_test(test_expr core/test_expr.cpp)
cobra_add_test(test_signature_vector core/test_signature_vector.cpp)
cobra_add_test(test_aux_var_eliminator core/test_aux_var_eliminator.cpp)
cobra_add_test(test_anf_transform core/test_anf_transform.cpp)
cobra_add_test(test_anf_cleanup core/test_anf_cleanup.cpp)
cobra_add_test(test_pattern_matcher core/test_pattern_matcher.cpp)
cobra_add_test(test_coeff_interpolator core/test_coeff_interpolator.cpp)
cobra_add_test(test_simplifier core/test_simplifier.cpp)
cobra_add_test(test_cob_expr_builder core/test_cob_expr_builder.cpp)
cobra_add_test(test_signature_checker core/test_signature_checker.cpp)
cobra_add_test(test_semilinear_ir core/test_semilinear_ir.cpp)
cobra_add_test(test_semilinear_normalizer core/test_semilinear_normalizer.cpp)
cobra_add_test(test_classifier core/test_classifier.cpp)
cobra_add_test(test_atom_simplifier core/test_atom_simplifier.cpp)
cobra_add_test(test_bit_partitioner core/test_bit_partitioner.cpp)
cobra_add_test(test_masked_atom_reconstructor core/test_masked_atom_reconstructor.cpp)
cobra_add_test(test_self_check core/test_self_check.cpp)
cobra_add_test(test_coefficient_splitter core/test_coefficient_splitter.cpp)
cobra_add_test(test_monomial_key core/test_monomial_key.cpp)
cobra_add_test(test_arithmetic_lowering core/test_arithmetic_lowering.cpp)
cobra_add_test(test_poly_normalizer core/test_poly_normalizer.cpp)
cobra_add_test(test_poly_expr_builder core/test_poly_expr_builder.cpp)
cobra_add_test(test_basis_transform core/test_basis_transform.cpp)
cobra_add_test(test_null_poly_generator core/test_null_poly_generator.cpp)
cobra_add_test(test_math_utils core/test_math_utils.cpp)
cobra_add_test(test_singleton_power_recovery core/test_singleton_power_recovery.cpp)
cobra_add_test(test_singleton_power_expr_builder core/test_singleton_power_expr_builder.cpp)
cobra_add_test(test_signature_eval core/test_signature_eval.cpp)
cobra_add_test(test_mixed_product_rewriter core/test_mixed_product_rewriter.cpp)
cobra_add_test(test_expr_cost core/test_expr_cost.cpp)
cobra_add_test(test_signature_simplifier core/test_signature_simplifier.cpp)
cobra_add_test(test_multivar_poly_recovery core/test_multivar_poly_recovery.cpp)
cobra_add_test(test_bitwise_decomposer core/test_bitwise_decomposer.cpp)
cobra_add_test(test_hybrid_decomposer core/test_hybrid_decomposer.cpp)
cobra_add_test(test_operand_simplifier core/test_operand_simplifier.cpp)
cobra_add_test(test_product_identity_recoverer core/test_product_identity_recoverer.cpp)
cobra_add_test(test_template_decomposer core/test_template_decomposer.cpp)
cobra_add_test(test_decomposition_engine core/test_decomposition_engine.cpp)
cobra_add_test(test_ghost_basis core/test_ghost_basis.cpp)
cobra_add_test(test_ghost_residual_solver core/test_ghost_residual_solver.cpp)
cobra_add_test(test_weighted_poly_fit core/test_weighted_poly_fit.cpp)
cobra_add_test(test_term_refiner core/test_term_refiner.cpp)
cobra_add_test(test_semilinear_signature core/test_semilinear_signature.cpp)
cobra_add_test(test_structure_recovery core/test_structure_recovery.cpp)

# ProductIdentityRecoverer diagnostic against real datasets.
# Not registered with gtest_discover_tests — run manually:
#   ./test/test_product_identity_diagnostic
add_executable(test_product_identity_diagnostic
    core/test_product_identity_diagnostic.cpp
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_product_identity_diagnostic PRIVATE
    cobra-core GTest::gtest_main)
target_include_directories(test_product_identity_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_product_identity_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")
cobra_add_test(test_semilinear_integration core/test_semilinear_integration.cpp)
if(Z3_FOUND)
    target_link_libraries(test_semilinear_integration PRIVATE cobra-verify)
    target_compile_definitions(test_semilinear_integration PRIVATE COBRA_HAS_Z3=1)
endif()

# Z3 verification tests (only when Z3 is available)
if(Z3_FOUND)
    add_executable(test_z3_verifier verify/test_z3_verifier.cpp)
    target_link_libraries(test_z3_verifier PRIVATE cobra-verify GTest::gtest_main)
    gtest_discover_tests(test_z3_verifier)

    # End-to-end integration: parse → simplify → Z3 verify
    # Uses MBA expressions from SiMBA (DenuvoSoftwareSolutions/SiMBA)
    add_executable(test_z3_integration verify/test_z3_integration.cpp
                   ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
    target_link_libraries(test_z3_integration PRIVATE cobra-verify GTest::gtest_main)
    target_include_directories(test_z3_integration PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
    gtest_discover_tests(test_z3_integration)
endif()

# Dataset benchmark tests: MSiMBA/MBASimplifier datasets
add_executable(test_dataset_benchmarks verify/test_dataset_benchmarks.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_dataset_benchmarks PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_dataset_benchmarks PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_dataset_benchmarks PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")
gtest_discover_tests(test_dataset_benchmarks)

# PLDIPoly diagnostic (manual, not auto-discovered)
add_executable(test_pldi_poly_diagnostic verify/test_pldi_poly_diagnostic.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_pldi_poly_diagnostic PRIVATE
    cobra-core GTest::gtest_main)
target_include_directories(test_pldi_poly_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_pldi_poly_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# QSynth diagnostic (manual, not auto-discovered)
add_executable(test_qsynth_diagnostic verify/test_qsynth_diagnostic.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_qsynth_diagnostic PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_qsynth_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_qsynth_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# ArithOverBitwise diagnostic (manual, not auto-discovered)
add_executable(test_arith_over_bitwise_diagnostic
               verify/test_arith_over_bitwise_diagnostic.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_arith_over_bitwise_diagnostic PRIVATE
    cobra-core GTest::gtest_main)
target_include_directories(test_arith_over_bitwise_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_arith_over_bitwise_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# Semilinear self-check diagnostic (manual, not auto-discovered)
add_executable(test_semilinear_selfcheck_diagnostic
               verify/test_semilinear_selfcheck_diagnostic.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_semilinear_selfcheck_diagnostic PRIVATE
    cobra-core GTest::gtest_main)
target_include_directories(test_semilinear_selfcheck_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_semilinear_selfcheck_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# QSynth trace dive (manual, not auto-discovered)
add_executable(test_qsynth_trace_dive verify/test_qsynth_trace_dive.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_qsynth_trace_dive PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_qsynth_trace_dive PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_qsynth_trace_dive PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# ExprParser test needs access to tools/cobra-cli headers
add_executable(test_expr_parser core/test_expr_parser.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_expr_parser PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_expr_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
gtest_discover_tests(test_expr_parser)
