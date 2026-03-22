; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: ~(x & y) + (x & y) + 1 should simplify to 0 (constant)
; The ~(x & y) = xor (x & y), -1 is detected as NOT in the Expr bridge.
; CHECK-LABEL: @test_not_pattern
; CHECK: ret i32 0
define i32 @test_not_pattern(i32 %x, i32 %y) {
entry:
  %and = and i32 %x, %y
  %not = xor i32 %and, -1
  %add1 = add i32 %not, %and
  %add2 = add i32 %add1, 1
  ret i32 %add2
}
