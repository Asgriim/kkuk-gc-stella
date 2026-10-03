#include <stella/runtime.h>

#include <stdlib.h>

_Noreturn void stella_gc_out_of_memory(size_t requested_bytes) {
  fprintf(
      stderr,
      "stella-c-gc-abi-v1: cannot allocate %zu bytes\n",
      requested_bytes);
  exit(EXIT_FAILURE);
}

_Noreturn void stella_abi_violation(const char *message) {
  fprintf(
      stderr,
      "stella-c-gc-abi-v1: ABI violation: %s\n",
      message == NULL ? "unspecified violation" : message);
  exit(EXIT_FAILURE);
}
