#ifndef STELLA_CPP_ABI_HPP
#define STELLA_CPP_ABI_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define _Static_assert static_assert
#define _Noreturn [[noreturn]]
#define _Alignof alignof

extern "C" {
#include <stella/gc.h>
}

#undef _Alignof
#undef _Noreturn
#undef _Static_assert

#endif
