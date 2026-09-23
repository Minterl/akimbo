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
    ctx->error.writer = error_writer;
}

AK_StatusKind
ak_check_error(AK_Context *ctx) {
    return ctx->error.relevant_status;
}

void
ak_report_error(AK_Context *ctx, AK_StatusKind relevant_status, ak_str8 fmt, ...) {
    if (ctx->error.relevant_status != AK_StatusKind_OK) {
        return;
    }

    ak_assert(relevant_status != AK_StatusKind_OK);

    ctx->error.relevant_status = relevant_status;

    va_list ap;
    va_start(ap, fmt);
    AK_StatusKind vprintf_status = ak_vprintf(ctx->error.writer, fmt, ap);
    (void)vprintf_status;
    va_end(ap);

    ak_write(ctx->error.writer, ak_str8_lit("\n"));
}
