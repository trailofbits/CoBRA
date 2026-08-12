; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Smoke test: nested zext(sext(i1 -> i8) -> i32) inside MBA.
; Verifies traversal through stacked extensions doesn't crash.
; The double extension (i1 -> i8 sext then i8 -> i32 zext)
; produces masking that the simplifier cannot see through.
; CHECK-LABEL: @test_ext_nested
; CHECK: sext
; CHECK: zext
; CHECK: ret i32
define i32 @test_ext_nested(i1 %x, i1 %y) {
entry:
  %xor1 = xor i1 %x, %y
  %s = sext i1 %xor1 to i8
  %z = zext i8 %s to i32
  %and1 = and i1 %x, %y
  %s2 = sext i1 %and1 to i8
  %z2 = zext i8 %s2 to i32
  %mul = mul i32 %z2, 2
  %add = add i32 %z, %mul
  ret i32 %add
}
