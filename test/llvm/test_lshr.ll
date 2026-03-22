; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Smoke test: LShr in MBA trees doesn't crash.
; The boolean signature at {0,1} is degenerate for shifts (x>>2 = 0
; for x in {0,1}), so the full-width evaluator correctly rejects
; the simplified result. The expression should pass through unchanged.
; CHECK-LABEL: @test_lshr_passthrough
; CHECK: lshr
; CHECK: ret i64
define i64 @test_lshr_passthrough(i64 %x, i64 %y) {
entry:
  %shr_x = lshr i64 %x, 2
  %shr_y = lshr i64 %y, 2
  %xor = xor i64 %shr_x, %shr_y
  %and = and i64 %shr_x, %shr_y
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
