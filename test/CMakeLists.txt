find_package(GTest REQUIRED)
include(GoogleTest)

function(cobra_add_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE cobra-core GTest::gtest_main)
    gtest_discover_tests(${TEST_NAME})
endfunction()

# Core tests
cobra_add_test(test_result core/test_result.cpp)
cobra_add_test(test_pass_contract core/test_pass_contract.cpp)
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
cobra_add_test(test_multivar_poly_recovery core/test_multivar_poly_recovery.cpp)
cobra_add_test(test_template_decomposer core/test_template_decomposer.cpp)
cobra_add_test(test_decomposition_engine core/test_decomposition_engine.cpp)
cobra_add_test(test_ghost_basis core/test_ghost_basis.cpp)
cobra_add_test(test_ghost_residual_solver core/test_ghost_residual_solver.cpp)
cobra_add_test(test_weighted_poly_fit core/test_weighted_poly_fit.cpp)
cobra_add_test(test_term_refiner core/test_term_refiner.cpp)
cobra_add_test(test_semilinear_signature core/test_semilinear_signature.cpp)
cobra_add_test(test_structure_recovery core/test_structure_recovery.cpp)
cobra_add_test(test_competition_group core/test_competition_group.cpp)
target_include_directories(test_competition_group PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_orchestrator_types core/test_orchestrator_types.cpp)
target_include_directories(test_orchestrator_types PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_signature_passes core/test_signature_passes.cpp)
target_include_directories(test_signature_passes PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_decomposition_types core/test_decomposition_types.cpp)
target_include_directories(test_decomposition_types PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_decomposition_helpers core/test_decomposition_helpers.cpp)
target_include_directories(test_decomposition_helpers PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_extractor_passes core/test_extractor_passes.cpp)
target_include_directories(test_extractor_passes PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_residual_prep_passes core/test_residual_prep_passes.cpp)
target_include_directories(test_residual_prep_passes PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_residual_solver_passes core/test_residual_solver_passes.cpp)
target_include_directories(test_residual_solver_passes PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_orchestrator_telemetry core/test_orchestrator_telemetry.cpp)

cobra_add_test(test_lifting_passes core/test_lifting_passes.cpp)
target_include_directories(test_lifting_passes PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)

cobra_add_test(test_semilinear_integration core/test_semilinear_integration.cpp)
target_include_directories(test_semilinear_integration PRIVATE ${PROJECT_SOURCE_DIR}/lib/core)
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

# Z3 formal verification of dataset results (manual, not auto-discovered).
# Compares original MBA AST against simplified AST via Z3 bitvector solver.
# Slow (~minutes per dataset) — run locally, not in CI:
#   ./test/test_dataset_z3
if(Z3_FOUND)
    add_executable(test_dataset_z3 verify/test_dataset_z3.cpp
                   ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
    target_link_libraries(test_dataset_z3 PRIVATE
        cobra-core cobra-verify GTest::gtest_main)
    target_include_directories(test_dataset_z3 PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
    target_compile_definitions(test_dataset_z3 PRIVATE
        DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")
endif()

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

# OSES unsupported-family diagnostic (manual, not auto-discovered)
add_executable(test_oses_diagnostic verify/test_oses_diagnostic.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_oses_diagnostic PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_oses_diagnostic PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_oses_diagnostic PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# MSiMBA ground truth diagnostic (manual, not auto-discovered)
add_executable(test_msimba_groundtruth verify/test_msimba_groundtruth.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_msimba_groundtruth PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_msimba_groundtruth PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
target_compile_definitions(test_msimba_groundtruth PRIVATE
    DATASET_DIR="${PROJECT_SOURCE_DIR}/test/datasets")

# LLVM pass FileCheck tests (only when pass is built)
if(COBRA_BUILD_LLVM_PASS)
    add_subdirectory(llvm)
endif()

# ExprParser test needs access to tools/cobra-cli headers
add_executable(test_expr_parser core/test_expr_parser.cpp
               ${PROJECT_SOURCE_DIR}/tools/cobra-cli/ExprParser.cpp)
target_link_libraries(test_expr_parser PRIVATE cobra-core GTest::gtest_main)
target_include_directories(test_expr_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/cobra-cli)
gtest_discover_tests(test_expr_parser)
