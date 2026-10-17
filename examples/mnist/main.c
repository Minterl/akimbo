#include "akimbo/internal/expr.h"
#define AKIMBO_IMPLEMENTATION
#include <akimbo/akimbo.h>

#include <stdio.h>

AK_AllocatorReturn
libc_allocator_f(void *ctx, AK_AllocatorMode mode_kind, AK_AllocatorParams params) {
    (void)ctx;
    switch (mode_kind) {
        case AK_AllocatorMode_Alloc:
            return (AK_AllocatorReturn){ .alloc.ptr = malloc(params.alloc.size_bytes) };
        case AK_AllocatorMode_Free:
            free(params.free.ptr);
            return (AK_AllocatorReturn){0};
        case AK_AllocatorMode_GetDefaultPreAllocation:
            return (AK_AllocatorReturn){ .get_default_pre_allocation.size_bytes = 1024 * 1024 };
        case AK_AllocatorMode_GetFlags:
            return (AK_AllocatorReturn){ .get_flags.flags = 0};
        default:
            ak_unreachable();
    }
}

size_t
write_stdout(void *ctx, ak_str8 msg) {
    (void)ctx;
    return fwrite(msg.p, 1, msg.len, stdout);
}

size_t
write_stderr(void *ctx, ak_str8 msg) {
    (void)ctx;
    return fwrite(msg.p, 1, msg.len, stderr);
}

static AK_Allocator 
libc_allocator = {
    .f = libc_allocator_f,
};

static AK_Writer 
libc_writer = {
    .write = write_stdout,
};

ak_maybe_unused static AK_Writer 
libc_err_writer = {
    .write = write_stderr,
};

int32_t 
main(void) {
    AK_StatusKind status = AK_StatusKind_OK;

    AK_Context ctx = {0};
    ak_context_init(&ctx, libc_allocator, &libc_writer);

    AK_LogicalBuilder builder = {0};

    AK_LogicalSymbol a = ak_param(&ctx, &builder, (AK_LogicalShape){ .rank = 2, .dim = {2, 2} });
    AK_LogicalSymbol b = ak_param(&ctx, &builder, (AK_LogicalShape){ .rank = 1, .dim = {2} });
    AK_LogicalSymbol c = ak_add(&ctx, &builder, a, b);
    ak_pin(&ctx, &builder, c);
    
    AK_LogicalExpr lexpr = {0};
    status = ak_lbuilder_finish(&ctx, &builder, libc_allocator, &lexpr);
    if (status != AK_StatusKind_OK) {
        goto out;
    }

    status = ak_lower_lexpr(&ctx, &lexpr, &(AK_Target){
        .kind = AK_TargetKind_C,
        .as.c = {
            .source_writer = AK_DBG_WRITER,
            .ident_prefix_lowercase = ak_str8_lit("ak_"),
            .scalar_ptr_type = ak_str8_lit("float*"),
        },
    }, 0);
    if (status != AK_StatusKind_OK) {
        goto out;
    }

out:
    ak_lexpr_destroy(&lexpr, libc_allocator);
    ak_arena_free_all(&ctx.arena);

    return status;
}
