; Test: (x ^ y) + 2 * (x & y) should simplify to x + y
define i64 @test_xor_and(i64 %x, i64 %y) {
entry:
  %xor = xor i64 %x, %y
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
