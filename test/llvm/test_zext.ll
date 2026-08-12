; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Smoke test: zext in MBA trees doesn't crash.
; The extension lowering masks the inner 8-bit MBA to 32 bits,
; so the simplifier cannot currently recognize the cross-width
; identity. The expression should pass through unchanged.
; CHECK-LABEL: @test_zext_xor_and
; CHECK: zext
; CHECK: ret i32
define i32 @test_zext_xor_and(i8 %x, i8 %y) {
entry:
  %xor = xor i8 %x, %y
  %and = and i8 %x, %y
  %zx = zext i8 %xor to i32
  %za = zext i8 %and to i32
  %mul = mul i32 %za, 2
  %add = add i32 %zx, %mul
  ret i32 %add
}
