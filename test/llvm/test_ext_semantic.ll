; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Regression test: extension semantics must not be transparent.
; sext(i1 x) + zext(i1 x) + 1 = 1 for all x when extensions
; are lowered correctly:
;   x=0: sext(0)=0,  zext(0)=0  → 0+0+1 = 1
;   x=1: sext(1)=-1, zext(1)=1  → -1+1+1 = 1
; With transparent (wrong) semantics both sext and zext would
; produce the identity, giving 2x+1 — not constant.
; CHECK-LABEL: @test_sext_zext_constant
; CHECK-NOT: sext
; CHECK-NOT: zext
; CHECK: ret i32 1
define i32 @test_sext_zext_constant(i1 %x) {
entry:
  %s = sext i1 %x to i32
  %z = zext i1 %x to i32
  %add1 = add i32 %s, %z
  %add2 = add i32 %add1, 1
  ret i32 %add2
}
