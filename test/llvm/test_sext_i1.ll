; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: sext i1 in an MBA tree.
; sext(i1 (x & y)) produces all-ones or zero — this must not
; be treated as identity.
; CHECK-LABEL: @test_sext_i1
; CHECK-NOT: sext
; CHECK: ret i32
define i32 @test_sext_i1(i1 %x, i1 %y) {
entry:
  %and = and i1 %x, %y
  %s = sext i1 %and to i32
  %xor = xor i32 %s, -1
  %add = add i32 %s, %xor
  ret i32 %add
}
