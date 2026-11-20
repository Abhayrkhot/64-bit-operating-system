// 128-bit division helpers the compiler calls for unsigned __int128 '/' and
// '%' (normally from compiler-rt, which the kernel and user programs don't
// link). Plain shift-and-subtract: only used off hot paths (time math,
// benchmark reporting).

namespace {
using u128 = unsigned __int128;

u128 divmod(u128 n, u128 d, u128* rem) {
  if (d == 0) __builtin_trap();
  if (d > n) {
    *rem = n;
    return 0;
  }
  u128 q = 0, r = 0;
  for (int i = 127; i >= 0; i--) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= static_cast<u128>(1) << i;
    }
  }
  *rem = r;
  return q;
}
}  // namespace

extern "C" u128 __udivti3(u128 n, u128 d) {
  u128 r;
  return divmod(n, d, &r);
}

extern "C" u128 __umodti3(u128 n, u128 d) {
  u128 r;
  divmod(n, d, &r);
  return r;
}
