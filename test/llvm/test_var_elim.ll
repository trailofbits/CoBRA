; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: variable elimination and remapping.
; (x ^ y) + 2*(x & y) + (z - z) should simplify to x + y.
; Variable z does not affect the output at {0,1} inputs (since z - z = 0),
; so EliminateAuxVars drops it. The pass must remap the simplified
; variable indices back to the correct leaf values (%x, %y).
; CHECK-LABEL: @test_var_elim
; CHECK: %[[ADD:cobra\.add[0-9]*]] = add i64 %x, %y
; CHECK-NEXT: ret i64 %[[ADD]]
define i64 @test_var_elim(i64 %x, i64 %y, i64 %z) {
entry:
  %xor = xor i64 %x, %y
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %sub = sub i64 %z, %z
  %add1 = add i64 %xor, %mul
  %add2 = add i64 %add1, %sub
  ret i64 %add2
}
