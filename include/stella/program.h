#ifndef STELLA_STRICT_C_TRANSLATION_V1_PROGRAM_H
#define STELLA_STRICT_C_TRANSLATION_V1_PROGRAM_H

#include <stella/runtime.h>

#define STELLA_STRICT_C_TRANSLATION_VERSION UINT32_C(1)

void stella_program_entry(
    StellaValue *result_root,
    StellaValue *argument_root);

#endif
