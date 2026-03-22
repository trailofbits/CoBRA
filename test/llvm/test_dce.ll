; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: dead instructions from replaced MBA trees are erased.
; After simplifying (x ^ y) + 2*(x & y) → x + y, the intermediate
; xor, and, mul instructions should be removed.
; CHECK-LABEL: @test_dce
; CHECK-NOT: xor
; CHECK-NOT: mul
; CHECK: %cobra.add = add i64 %x, %y
; CHECK-NEXT: ret i64 %cobra.add
define i64 @test_dce(i64 %x, i64 %y) {
entry:
  %xor = xor i64 %x, %y
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
