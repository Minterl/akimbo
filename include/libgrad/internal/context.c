#include <libgrad/internal/context.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// error reporting shennanigans
///
/// TODO: move this with the context stuff to a context.(h|c)
///
////////////////////////////////////////////////////////////////////////////////

void
lg_context_init(
    LG_Context *ctx,
    LG_Allocator *scratch_allocator,
    LG_Writer *lg_nullable error_writer
) {
    lg_memzero(ctx, sizeof(LG_Context));
    lg_arena_init(&ctx->arena, scratch_allocator);
    ctx->error.writer = error_writer;
}

LG_StatusKind
lg_check_error(LG_Context *ctx) {
    return ctx->error.relevant_status;
}

void
lg_report_error(LG_Context *ctx, LG_StatusKind relevant_status, lg_str8 fmt, ...) {
    if (ctx->error.relevant_status != LG_StatusKind_OK) {
        return;
    }

    lg_assert(relevant_status != LG_StatusKind_OK);

    ctx->error.relevant_status = relevant_status;

    va_list ap;
    va_start(ap, fmt);
    LG_StatusKind vprintf_status = lg_vprintf(ctx->error.writer, fmt, ap);
    (void)vprintf_status;
    va_end(ap);

    lg_write(ctx->error.writer, lg_str8_lit("\n"));
}
