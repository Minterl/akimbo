#ifndef LG_CONTEXT_H_
#define LG_CONTEXT_H_

#include <libgrad/internal/base.h>

typedef struct
LG_Error {
    LG_StatusKind relevant_status;
    LG_Writer *writer;
} LG_Error;

typedef struct
LG_Context {
    LG_Arena  arena;
    LG_Error  error;
} LG_Context;

void
lg_context_init(
    LG_Context *ctx,
    LG_Allocator scratch_allocator,
    LG_Writer *lg_nullable error_writer
);

LG_StatusKind
lg_check_error(LG_Context *ctx);

/// Reports error on a best-effort basis, filling the buffer as much as possible.
/// Does nothing if the error has already been set
void 
lg_report_error(LG_Context *ctx, LG_StatusKind status, lg_str8 fmt, ...);

#endif // LG_CONTEXT_H_
