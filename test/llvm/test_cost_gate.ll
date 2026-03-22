; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: cost gate skips replacement when simplified form is not smaller.
; 3*x + 5*y + 7*z + 9*w arranged as a balanced tree. The simplifier
; reconstructs the same linear combination with identical cost
; (weighted_size, nonlinear_mul_count, max_depth), so IsBetter returns
; false and the pass leaves the expression unchanged.
; CHECK-LABEL: @test_cost_gate_skip
; CHECK: %mx = mul i64 %x, 3
; CHECK: %my = mul i64 %y, 5
; CHECK: %mz = mul i64 %z, 7
; CHECK: %mw = mul i64 %w, 9
; CHECK: ret i64
define i64 @test_cost_gate_skip(i64 %x, i64 %y, i64 %z, i64 %w) {
entry:
  %mx = mul i64 %x, 3
  %my = mul i64 %y, 5
  %mz = mul i64 %z, 7
  %mw = mul i64 %w, 9
  %a1 = add i64 %mx, %my
  %a2 = add i64 %mz, %mw
  %a3 = add i64 %a1, %a2
  ret i64 %a3
}
