; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: MBA split across two basic blocks connected by unconditional
; branch.  (x ^ y) + 2*(x & y) = x + y.  The xor lives in entry,
; the rest in cont.  CollectTree should follow operands across the
; block boundary and build the complete tree.
; CHECK-LABEL: @test_cross_block
; CHECK: %[[ADD:cobra\.add[0-9]*]] = add i64 %x, %y
; CHECK: ret i64 %[[ADD]]
define i64 @test_cross_block(i64 %x, i64 %y) {
entry:
  %xor = xor i64 %x, %y
  br label %cont

cont:
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
