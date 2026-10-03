#ifndef STELLA_SCALAR_HOST_H
#define STELLA_SCALAR_HOST_H

/* Интерфейс между порождённой программой и runtime/main.c; в интерфейс сборщика (ABI v1) не входит. */
enum StellaScalarKind { STELLA_SCALAR_BOOL, STELLA_SCALAR_NAT, STELLA_SCALAR_UNIT };
extern const enum StellaScalarKind stella_program_argument_kind;
extern const enum StellaScalarKind stella_program_result_kind;

#endif
