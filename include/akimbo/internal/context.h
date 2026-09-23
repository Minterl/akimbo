#ifndef AK_CONTEXT_H_
#define AK_CONTEXT_H_

#include <akimbo/internal/base.h>

typedef struct
AK_Error {
    AK_StatusKind relevant_status;
    AK_Writer *writer;
} AK_Error;

typedef struct
AK_Context {
    AK_Arena  arena;
    AK_Error  error;
} AK_Context;

void
ak_context_init(
    AK_Context *ctx,
    AK_Allocator scratch_allocator,
    AK_Writer *ak_nullable error_writer
);

AK_StatusKind
ak_check_error(AK_Context *ctx);

/// Reports error on a best-effort basis, filling the buffer as much as possible.
/// Does nothing if the error has already been set
void 
ak_report_error(AK_Context *ctx, AK_StatusKind status, ak_str8 fmt, ...);

#endif // AK_CONTEXT_H_
