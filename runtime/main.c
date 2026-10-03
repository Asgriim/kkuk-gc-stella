/* Запуск программы: ПРОГРАММА ЗНАЧЕНИЕ [MAX_HEAP_BYTES].
 * Переменные окружения STELLA_GC_STATS и STELLA_GC_STATE включают печать статистики
 * и состояния сборщика в стандартный поток ошибок. */
#include <stella/gc.h>
#include <stella/host.h>
#include <stella/program.h>
#include <stella/scalar.h>

#include <stdlib.h>
#include <string.h>

static size_t decimal(const char *text) {
  if (*text == '\0') stella_abi_violation("empty decimal input");
  size_t result = 0;
  for (; *text != '\0'; ++text) {
    if (*text < '0' || *text > '9') stella_abi_violation("expected an unsigned decimal integer");
    size_t digit = (size_t)(*text - '0');
    if (result > (SIZE_MAX - digit) / 10) stella_abi_violation("decimal input exceeds SIZE_MAX");
    result = result * 10 + digit;
  }
  return result;
}

static void encode(StellaValue *root, const char *text) {
  switch (stella_program_argument_kind) {
    case STELLA_SCALAR_BOOL:
      if (strcmp(text, "true") == 0) *root = &stella_bool_true;
      else if (strcmp(text, "false") == 0) *root = &stella_bool_false;
      else stella_abi_violation("expected true or false");
      return;
    case STELLA_SCALAR_UNIT:
      if (strcmp(text, "unit") != 0) stella_abi_violation("expected unit");
      *root = &stella_unit;
      return;
    case STELLA_SCALAR_NAT: {
      size_t count = decimal(text);
      *root = &stella_nat_zero;
      StellaValue next = NULL;
      stella_gc_push_root(&next);
      for (size_t i = 0; i < count; ++i) {
        next = stella_gc_alloc(&stella_nat_succ_descriptor);
        stella_object_init_managed(next, 0, *root);
        *root = next;
      }
      stella_gc_pop_root(&next);
      return;
    }
  }
  stella_abi_violation("invalid program argument kind");
}

static void print_result(StellaValue *root) {
  switch (stella_program_result_kind) {
    case STELLA_SCALAR_BOOL:
      if (*root == &stella_bool_true) puts("true");
      else if (*root == &stella_bool_false) puts("false");
      else stella_abi_violation("invalid Bool result");
      return;
    case STELLA_SCALAR_UNIT:
      if (*root != &stella_unit) stella_abi_violation("invalid Unit result");
      puts("unit");
      return;
    case STELLA_SCALAR_NAT: {
      size_t count = 0;
      StellaValue current = *root;
      stella_gc_push_root(&current);
      while (current != &stella_nat_zero) {
        if (current == NULL || current->descriptor != &stella_nat_succ_descriptor || count == SIZE_MAX)
          stella_abi_violation("invalid or oversized Nat result");
        current = stella_gc_read_managed(&current, 0);
        ++count;
      }
      printf("%zu\n", count);
      stella_gc_pop_root(&current);
      return;
    }
  }
  stella_abi_violation("invalid program result kind");
}

static int requested(const char *name) {
  const char *value = getenv(name);
  return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

int main(int argc, char *argv[]) {
  if (argc != 2 && argc != 3) {
    fputs("usage: PROGRAM VALUE [MAX_HEAP_BYTES]\n", stderr);
    return EXIT_FAILURE;
  }
  StellaGcConfig config = {STELLA_C_GC_ABI_VERSION, argc == 3 ? decimal(argv[2]) : 64 * 1024 * 1024};
  stella_gc_init(&config);
  StellaValue argument = NULL;
  StellaValue result = NULL;
  stella_gc_push_root(&argument);
  stella_gc_push_root(&result);
  encode(&argument, argv[1]);
  stella_program_entry(&result, &argument);
  print_result(&result);
  if (fflush(stdout) == EOF) return EXIT_FAILURE;
  /* Печать происходит, пока аргумент и результат ещё остаются корнями. */
  if (requested("STELLA_GC_STATS")) stella_gc_print_statistics(stderr);
  if (requested("STELLA_GC_STATE")) stella_gc_print_state(stderr);
  stella_gc_pop_root(&result);
  stella_gc_pop_root(&argument);
  stella_gc_shutdown();
  if (fflush(stdout) == EOF || ferror(stdout)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
