#include "base.h"
#include <akimbo/internal/context.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// error reporting shennanigans
///
/// TODO: move this with the context stuff to a context.(h|c)
///
////////////////////////////////////////////////////////////////////////////////

void
ak_context_init(
    AK_Context *ctx,
    AK_Allocator scratch_allocator,
    AK_Writer *ak_nullable error_writer
) {
    ak_memzero(ctx, sizeof(AK_Context));
    ak_arena_init(&ctx->arena, scratch_allocator);
    ctx->err.writer = error_writer;
}

void
ak_report_error(AK_Error *err, AK_StatusKind relevant_status, ak_str8 fmt, ...) {
    if (err->relevant_status != AK_StatusKind_OK) {
        return;
    }

    ak_assert(relevant_status != AK_StatusKind_OK);

    err->relevant_status = relevant_status;

    va_list ap;
    va_start(ap, fmt);
    size_t vprintf_status = ak_vprintf(err->writer, fmt, ap);
    (void)vprintf_status;
    va_end(ap);

    ak_write(err->writer, ak_str8_lit("\n"));
}
