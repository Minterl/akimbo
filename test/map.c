#ifndef AKIMBO_IMPLEMENTATION
#define AKIMBO_IMPLEMENTATION
#endif // AKIMBO_IMPLEMENTATION
#include <akimbo/akimbo.h>
#include <akimbo/internal/map.h>

#ifndef TEST_IMPLEMENTATION
#define TEST_IMPLEMENTATION
#endif // TEST_IMPLEMENTATION
#include "testing.h"

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
};

test_status test_map() {
    AK_StatusKind status;

    AK_Map map = {0};
    status = ak_map_init(&map, &libc_alloc, 32);
    test_assert(status == AK_StatusKind_OK, "failed to init map");

    bool was_occupied;

    size_t first_idx;
    {
        status = ak_map_ensure(&map, 1234, &first_idx, &was_occupied);
        test_assert(status == AK_StatusKind_OK, "failed to insert into map");
        test_assert(!was_occupied, "incorrectly indicated the slot was occpuied");

        size_t idx_1;
        status = ak_map_ensure(&map, 1234, &idx_1, &was_occupied);
        test_assert(status == AK_StatusKind_OK, "failed to upsert into/get from map");
        test_assert(was_occupied, "incorrectly indicated the slot not was occpuied");
        test_assert(first_idx == idx_1, "gave wrong index");

        size_t idx_2 = ak_map_get(&map, 1234, &was_occupied);
        test_assert(was_occupied, "incorrectly indicated the slot not was occpuied");
        test_assert(first_idx == idx_2, "gave wrong index");
    }

    {
        size_t idx_0;
        status = ak_map_ensure(&map, 4321, &idx_0, &was_occupied);
        test_assert(status == AK_StatusKind_OK, "failed to insert into map");
        test_assert(!was_occupied, "incorrectly indicated the slot was occpuied");
        test_assert(idx_0 != first_idx, "clobbered original slot; idx: %lu", idx_0);

        size_t idx_1;
        status = ak_map_ensure(&map, 4321, &idx_1, &was_occupied);
        test_assert(status == AK_StatusKind_OK, "failed to upsert into/get from map");
        test_assert(was_occupied, "incorrectly indicated the slot not was occpuied");
        test_assert(idx_0 == idx_1, "gave wrong index");

        size_t idx_2 = ak_map_get(&map, 4321, &was_occupied);
        test_assert(was_occupied, "incorrectly indicated the slot not was occpuied");
        test_assert(idx_0 == idx_2, "gave wrong index");
    }

    ak_map_deinit(&map, &libc_alloc);

    return TEST_STATUS_OK;
}

test_status test_iter() {
    AK_StatusKind status;

    int32_t keys[32] = {0};
    for (size_t i = 0; i < 32; i++) {
        keys[i] = ((1208 % (i + 1) * 13) ^ (i * 7)) * i + 1;
        test_assert(keys[i] != 0, "they're supposed to be random");
    }

    AK_Map map = {0};
    status = ak_map_init(&map, &libc_alloc, 32);
    test_assert(status == AK_StatusKind_OK, "failed to init map");

    int32_t values[32] = {0};

    for (size_t i = 0; i < 32; i++) {
        size_t idx;
        bool was_occupied;
        status = ak_map_ensure(&map, keys[i], &idx, &was_occupied);
        test_assert(status == AK_StatusKind_OK, "failed to insert into map");
        test_assert(!was_occupied, "incorrectly indicated the slot was occpuied");
        values[idx] = -keys[i];
    }

    AK_MapIter iter;
    ak_map_iter_init(&iter, &map);
    while(ak_map_iter_advance(&iter)) {
        bool found = false;
        for (size_t i = 0; i < 32; i++) {
            if (keys[i] == (int32_t)iter.key) {
                found = true;
                break;
            }
        }
        test_assert(found, "iter key not in keys");
        
        int32_t got_value = values[ak_map_get(&map, iter.key, &found)];
        test_assert(found, "value not found");
        test_assert(values[iter.idx] == got_value, "wanted: %d, got %d", values[iter.idx], got_value);

        test_assert(
            values[iter.idx] == -1 * (int32_t)iter.key,
            "incorrect value; wanted: %d, got: %d", -1 * (int32_t)iter.idx, values[iter.idx]
        );
    }

    ak_map_deinit(&map, &libc_alloc);

    return TEST_STATUS_OK;
}

int main() {
    test_run(map);
    test_run(iter);
    return 0;
}
