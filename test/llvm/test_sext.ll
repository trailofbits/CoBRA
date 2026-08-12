; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Smoke test: sext in MBA trees doesn't crash.
; The sign-extension lowering introduces masking and offset
; arithmetic that obscures the inner 8-bit MBA identity.
; The expression should pass through unchanged.
; CHECK-LABEL: @test_sext_xor_and
; CHECK: sext
; CHECK: ret i32
define i32 @test_sext_xor_and(i8 %x, i8 %y) {
entry:
  %xor = xor i8 %x, %y
  %and = and i8 %x, %y
  %sx = sext i8 %xor to i32
  %sa = sext i8 %and to i32
  %mul = mul i32 %sa, 2
  %add = add i32 %sx, %mul
  ret i32 %add
}
