// In-place heapsort (no allocation, O(n log n) worst case) for benchmark samples.
#pragma once

#include <stddef.h>

namespace shale {

template <typename T>
void heap_sort(T* a, size_t n) {
  auto sift = [a](size_t root, size_t end) {
    while (2 * root + 1 < end) {
      size_t child = 2 * root + 1;
      if (child + 1 < end && a[child] < a[child + 1]) child++;
      if (!(a[root] < a[child])) return;
      T t = a[root];
      a[root] = a[child];
      a[child] = t;
      root = child;
    }
  };
  if (n < 2) return;
  for (size_t i = n / 2; i-- > 0;) sift(i, n);
  for (size_t end = n - 1; end > 0; end--) {
    T t = a[0];
    a[0] = a[end];
    a[end] = t;
    sift(0, end);
  }
}

}  // namespace shale
