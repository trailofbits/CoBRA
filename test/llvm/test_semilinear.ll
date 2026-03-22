; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: (x ^ 0x10) + 2*(x & 0x10) should simplify to x + 16
; This is a semilinear expression (constants inside bitwise ops) —
; only reachable when the AST is passed to Simplify(), enabling
; the semilinear pipeline with XOR constant lowering.
; CHECK-LABEL: @test_semilinear_xor_cancel
; CHECK: %cobra.add = add i64 {{(%x, 16|16, %x)}}
; CHECK-NEXT: ret i64 %cobra.add
define i64 @test_semilinear_xor_cancel(i64 %x) {
entry:
  %xor = xor i64 %x, 16
  %and = and i64 %x, 16
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
