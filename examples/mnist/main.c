#include "libgrad/internal/expr.h"
#define LIBGRAD_IMPLEMENTATION
#include <libgrad/libgrad.h>

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
write_stdout(void *ctx, lg_str8 msg) {
    (void)ctx;
    return printf("%.*s", (int32_t)msg.len, msg.p);
}

LG_Allocator
libc_allocator = {
    .alloc = alloc_libc,
    .free = free_libc,
    .default_slab_size_bytes = 1024 * 1024 * 1024,
};

LG_Writer 
libc_writer = {
    .write = write_stdout,
};

int32_t 
main(void) {
    LG_StatusKind status = LG_StatusKind_OK;

    LG_Context ctx = {0};
    lg_context_init(&ctx, &libc_allocator, &libc_writer);

    LG_LogicalBuilder builder = {0};

    LG_LogicalSymbol a = lg_param(&ctx, &builder, (LG_LogicalShape){0});
    LG_LogicalSymbol b = lg_param(&ctx, &builder, (LG_LogicalShape){0});
    LG_LogicalSymbol c = lg_add(&ctx, &builder, a, b);
    lg_pin(&ctx, &builder, c);
    
    LG_LogicalExpr expr = {0};
    status = lg_lbuilder_finish(&ctx, &builder, &libc_allocator, &expr);
    if (status != LG_StatusKind_OK) {
        return -1;
    }

    lg_lexpr_destroy(&expr, &libc_allocator);
    lg_arena_free_all(&ctx.arena);

    return 0;
}
