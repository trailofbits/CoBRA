; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Negative test: extension wrapping a non-MBA expression.
; zext of a single variable has no boolean+arithmetic mix,
; so it must NOT be detected as an MBA candidate.
; CHECK-LABEL: @test_ext_only_not_mba
; CHECK: zext
; CHECK: ret i32
define i32 @test_ext_only_not_mba(i8 %x) {
entry:
  %z = zext i8 %x to i32
  ret i32 %z
}

; Negative test: extension chain with no core MBA ops.
; CHECK-LABEL: @test_ext_chain_not_mba
; CHECK: sext
; CHECK: zext
; CHECK: ret i32
define i32 @test_ext_chain_not_mba(i1 %x) {
entry:
  %s = sext i1 %x to i8
  %z = zext i8 %s to i32
  ret i32 %z
}
