#include "akimbo/internal/expr.h"
#define AKIMBO_IMPLEMENTATION
#include <akimbo/akimbo.h>

#include <stdio.h>

AK_AllocatorModeReturn
libc_allocator_f(void *ctx, AK_AllocatorModeKind mode_kind, AK_AllocatorModeParams params) {
    (void)ctx;
    switch (mode_kind) {
        case AK_AllocatorModeKind_Alloc:
            return (AK_AllocatorModeReturn){ .alloc.ptr = malloc(params.alloc.size_bytes) };
        case AK_AllocatorModeKind_Free:
            free(params.free.ptr);
            return (AK_AllocatorModeReturn){0};
        case AK_AllocatorModeKind_GetDefaultPreAllocation:
            return (AK_AllocatorModeReturn){ .get_default_pre_allocation.size_bytes = 1024 * 1024 };
        case AK_AllocatorModeKind_GetFlags:
            return (AK_AllocatorModeReturn){ .get_flags.flags = 0};
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

    status = ak_lower_lexpr(&ctx, libc_allocator, &lexpr, 0);
    if (status != AK_StatusKind_OK) {
        goto out;
    }

out:
    ak_lexpr_destroy(&lexpr, libc_allocator);
    ak_arena_free_all(&ctx.arena);

    return status;
}
