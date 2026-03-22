; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: PHI with divergent arms should be treated as a leaf.
; The left branch computes xor, the right computes or — different
; functions.  The phi verification should reject transparency,
; and the post-phi add is too small to be a standalone candidate.
; The per-branch MBAs (xor and or alone) are also below min_ast_size.
; The expression should pass through unchanged.
; CHECK-LABEL: @test_phi_divergent
; CHECK: phi
; CHECK: ret i64
define i64 @test_phi_divergent(i64 %x, i64 %y, i1 %cond) {
entry:
  br i1 %cond, label %left, label %right

left:
  %xor_l = xor i64 %x, %y
  br label %merge

right:
  %or_r = or i64 %x, %y
  br label %merge

merge:
  %p = phi i64 [ %xor_l, %left ], [ %or_r, %right ]
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %p, %mul
  ret i64 %add
}
