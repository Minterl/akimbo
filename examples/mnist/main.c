#include "akimbo/internal/expr.h"
#define AKIMBO_IMPLEMENTATION
#include <akimbo/akimbo.h>

#include <stdio.h>

void*
alloc_libc(void *_, size_t bytes) {
    (void)_;
    return calloc(bytes, 1);
}

void 
free_libc(void* _, void *ptr) {
    (void)_;
    return free(ptr);
}

size_t
write_stdout(void *ctx, ak_str8 msg) {
    (void)ctx;
    return printf("%.*s", (int32_t)msg.len, msg.p);
}

AK_Allocator
libc_allocator = {
    .alloc = alloc_libc,
    .free = free_libc,
    .default_slab_size_bytes = 1024 * 1024 * 1024,
};

AK_Writer 
libc_writer = {
    .write = write_stdout,
};

int32_t 
main(void) {
    AK_StatusKind status = AK_StatusKind_OK;

    AK_Context ctx = {0};
    ak_context_init(&ctx, &libc_allocator, &libc_writer);

    AK_LogicalBuilder builder = {0};

    AK_LogicalSymbol a = ak_param(&ctx, &builder, (AK_LogicalShape){ .rank = 2, .dim = {2, 2} });
    AK_LogicalSymbol b = ak_param(&ctx, &builder, (AK_LogicalShape){ .rank = 1, .dim = {2} });
    AK_LogicalSymbol c = ak_add(&ctx, &builder, a, b);
    ak_pin(&ctx, &builder, c);
    
    AK_LogicalExpr lexpr = {0};
    status = ak_lbuilder_finish(&ctx, &builder, &libc_allocator, &lexpr);
    if (status != AK_StatusKind_OK) {
        goto out;
    }

    status = ak_lower_lexpr(&ctx, &libc_allocator, &lexpr, 0);
    if (status != AK_StatusKind_OK) {
        goto out;
    }

out:
    ak_lexpr_destroy(&lexpr, &libc_allocator);
    ak_arena_free_all(&ctx.arena);

    return status;
}
