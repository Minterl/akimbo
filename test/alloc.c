#include <akimbo/internal/alloc.h>

#ifndef TEST_IMPLEMENTATION
#define TEST_IMPLEMENTATION
#endif // TEST_IMPLEMENTATION
#include "testing.h"

#include <stdio.h>
#include <stdlib.h>

void *alloc_libc(void *_, size_t bytes) {
    (void)_;
    return calloc(bytes, 1);
}

void free_libc(void* _, void *ptr) {
    (void)_;
    return free(ptr);
}

static AK_Allocator libc_alloc = {
    .alloc = alloc_libc,
    .free = free_libc,
    .default_slab_size_bytes = 128,
};

test_status test_alloc() {
    AK_Arena arena;
    ak_arena_init(&arena, &libc_alloc);

    AK_Scope scope = ak_push_scope(&arena);
    ak_arena_alloc(&arena, 32, 16);
    ak_arena_alloc(&arena, 12, 16);
    ak_pop_scope(&arena, scope);
    ak_arena_alloc(&arena, 2, 16);
    ak_arena_alloc(&arena, 256, 16);
    ak_arena_free_all(&arena);
    ak_arena_free_recycled(&arena);

    test_assert(arena.current_offset == 0, "current offset must have been reset");
    test_assert(arena.recycled_slabs_head == NULL, "there must be no recycled slabs left");
    test_assert(arena.current_slab == NULL, "there must be no active slabs left");

    return TEST_STATUS_OK;
}

int main() {
    test_run(alloc);
}

#include <akimbo/internal/alloc.c>
#include <akimbo/internal/debug.c>
