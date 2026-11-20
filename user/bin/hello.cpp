#include "shale.hpp"

int main(int argc, char** argv) {
  printf("hello from user space: pid %d, parent %d, %d arg(s):", getpid(), getppid(), argc);
  for (int i = 0; i < argc; i++) printf(" %s", argv[i]);
  printf("\n");
  return argc > 1 ? static_cast<int>(atol(argv[1])) : 0;
}
