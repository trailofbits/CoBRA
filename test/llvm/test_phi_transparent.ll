; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: MBA with a transparent PHI node.  Both branches compute the
; same xor, merging through a phi.  The detector should recognize
; that both arms are equivalent, follow through the phi, and
; simplify (x ^ y) + 2*(x & y) = x + y.
; CHECK-LABEL: @test_phi_transparent
; CHECK: %[[ADD:cobra\.add[0-9]*]] = add i64 %x, %y
; CHECK: ret i64 %[[ADD]]
define i64 @test_phi_transparent(i64 %x, i64 %y, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right

left:
  %xor_l = xor i64 %x, %y
  br label %merge

right:
  %xor_r = xor i64 %x, %y
  br label %merge

merge:
  %p = phi i64 [ %xor_l, %left ], [ %xor_r, %right ]
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %p, %mul
  ret i64 %add
}
