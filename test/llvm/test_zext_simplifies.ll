; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Positive test: extension lowering produces correct semantics.
; zext(x^y) - zext(x^y) + zext(x&y) - zext(x&y) = 0
; Both zext arms cancel, so the full-width evaluator confirms
; constant 0 regardless of input width mismatch.
; CHECK-LABEL: @test_zext_cancel
; CHECK-NOT: zext
; CHECK-NOT: xor
; CHECK-NOT: and
; CHECK: ret i64 0
define i64 @test_zext_cancel(i8 %x, i8 %y) {
entry:
  %xor = xor i8 %x, %y
  %and = and i8 %x, %y
  %z1 = zext i8 %xor to i64
  %z2 = zext i8 %xor to i64
  %z3 = zext i8 %and to i64
  %z4 = zext i8 %and to i64
  %sub1 = sub i64 %z1, %z2
  %sub2 = sub i64 %z3, %z4
  %add = add i64 %sub1, %sub2
  ret i64 %add
}
