#ifndef AKIMBO_IMPLEMENTATION
#define AKIMBO_IMPLEMENTATION
#include "akimbo/internal/core.h"
#endif // AKIMBO_IMPLEMENTATION
#ifndef AK_CPU_IMPLEMENTATION
#define AK_CPU_IMPLEMENTATION
#endif // AK_CPU_IMPLEMENTATION
#ifndef TEST_IMPLEMENTATION
#define TEST_IMPLEMENTATION
#endif // TEST_IMPLEMENTATION
 
#include <stdbool.h>
#include <stdlib.h>
#include <akimbo/akimbo.h>
#include <akimbo/cpu.h>
#include "testing.h"

// Using unusual memory alignment of 5 for testing purposes
#define ALLOC_ADDR (void*)(5 * sizeof(ak_scalar))

typedef struct mock_allocator_context {
    size_t bytes_allocated;
} mock_allocator_context;

void* mock_alloc(void *_ctx, size_t size_bytes) {
    mock_allocator_context *ctx = _ctx;
    ctx->bytes_allocated += size_bytes;
    return ALLOC_ADDR;
}

static inline bool increment_coords_rtl(size_t *coords, const size_t *dim, size_t rank) {
    if (rank == 0) return false;

    size_t axis = rank;
    
    while (axis > 0) {
        axis--;
        coords[axis]++;
        if (coords[axis] < dim[axis]) {
            return true; 
        }
        coords[axis] = 0;
    }

    return false;
}

test_status test_tensor_layout() {
    // --- w/o padding ---
    {
        size_t expected_strides[] = {8, 4, 1};
        AK_StridedDesc desc = { .dim = {3, 2, 4}, .rank = 3 };
        test_assert(ak_desc_compute_strides(&desc, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
        test_assert(desc.rank == 3, "got tensor rank %lu", desc.rank);
        test_assert_array_eq(expected_strides, desc.strides, 3, "%lu");
    }

    // --- w/ padding ---
    {
        size_t expected_strides[4] = {224, 32, 8, 1};
        AK_StridedDesc desc = { .dim = {2, 7, 4, 3}, .rank = 4 };
        test_assert(ak_desc_compute_strides(&desc, AK_LayoutKind_RowMajor, 8) == AK_StatusKind_OK, "failed to initialize tensor");
        test_assert(desc.rank == 4, "got tensor rank %lu", desc.rank);
        test_assert_array_eq(expected_strides, desc.strides, 3, "%lu");
    }
    
    // --- w/o padding ---
    {
        size_t expected_strides[] = {1, 4, 8};
        AK_StridedDesc desc = { .dim = {3, 2, 4}, .rank = 3 };
        test_assert(ak_desc_compute_strides(&desc, AK_LayoutKind_ColumnMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
        test_assert(desc.rank == 3, "got tensor rank %lu", desc.rank);
        test_assert_array_eq(expected_strides, desc.strides, 3, "%lu");
    }

    // --- w/ padding ---
    {
        size_t expected_strides[] = {1, 8, 32, 224};
        AK_StridedDesc desc = { .dim = {2, 7, 4, 3}, .rank = 4 };
        test_assert(ak_desc_compute_strides(&desc, AK_LayoutKind_ColumnMajor, 8) == AK_StatusKind_OK, "failed to initialize tensor");
        test_assert(desc.rank == 4, "got tensor rank %lu", desc.rank);
        test_assert_array_eq(expected_strides, desc.strides, 3, "%lu");
    }

    return TEST_STATUS_OK;
}

test_status test_tensor_size() {
    AK_StridedDesc _36 = {
        .rank = 2, 
        .dim = {6, 6},
        .strides = {6, 1},
    };
    size_t _36_size = ak_desc_size_in_bytes(_36);
    test_assert(_36_size == 36 * sizeof(ak_scalar), "tensor size was %lu", _36_size);

    AK_StridedDesc also_36 = {
        .rank = 3,
        .dim = {6, 2, 3},
        .strides = {6, 3, 1},
    };
    size_t also_36_size = ak_desc_size_in_bytes(also_36);
    test_assert(also_36_size == 36 * sizeof(ak_scalar), "tensor size was %lu", also_36_size);

    AK_StridedDesc padded = { .dim = {3, 3, 3}, .rank = 3 };
    test_assert(ak_desc_compute_strides(&padded, AK_LayoutKind_RowMajor, 4) == AK_StatusKind_OK, "failed to initialize tensor");
    size_t calculated_bytes = ak_desc_size_in_bytes(padded);
    // Strides should be (12, 4, 1), meaning the maximum offset at (2, 2, 2) is
    // (12 + 4 + 1) * 2, so the max size is one more than that.
    size_t expected_bytes = ((12 + 4 + 1) * 2 + 1) * sizeof(ak_scalar);
    test_assert(
        calculated_bytes == expected_bytes,
        "tensor size calculated to be %lu bytes, wanted %lu",
        calculated_bytes,
        expected_bytes
    );

    AK_StridedDesc zero_ten = {0};
    test_assert(ak_desc_compute_strides(&zero_ten, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
    calculated_bytes = ak_desc_size_in_bytes(zero_ten);
    test_assert(calculated_bytes == 0, "tensor size calculated to be %lu bytes", calculated_bytes);

    AK_StridedDesc scalar = { .dim = {1}, .rank = 1 };
    test_assert(ak_desc_compute_strides(&scalar, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
    calculated_bytes = ak_desc_size_in_bytes(scalar);
    test_assert(calculated_bytes == sizeof(ak_scalar), "tensor size calculated to be %lu bytes", calculated_bytes);

    return TEST_STATUS_OK;
}

test_status test_tensor_aligned_views_not_compatible() {
    // 4 != 5, so these should clash and not be compatible
    AK_StridedDesc x0 = { .dim =  {4, 4}, .rank = 2 };
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
    AK_StridedDesc x1 = { .dim =  {6, 5, 4}, .rank = 3 };
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");

    AK_StatusKind status = ak_create_broadcast_space((AK_StridedDesc*[]){&x0, &x1}, 2);
    test_assert(status == AK_StatusKind_ShapeMismatch, "failed to detect shape mismatch");

    return TEST_STATUS_OK;
}

test_status test_tensor_aligned_views() {
    // This is a 6x4x4 tensor.
    // In memory, this looks like this:
    // { 
    //     (0, 0, 0), (0, 0, 1) ...
    //     (0, 1, 0), (0, 1, 1) ...
    //     (1, 0, 0), (1, 0, 1) ...
    //     (m-1, n-1, k-1)
    // }
    AK_StridedDesc x0 = { .dim = {6, 4, 4}, .rank = 3 };
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");
    // This is a mat44.
    // In memory, with no alignment, this should be a contiguous
    // row-major 2d array (these are (x, y) pairs, not matrix coords):
    // { (0, 0), (0, 1) ... (1, 0), (1, 1) ... (m-1, n-1) }
    AK_StridedDesc x1 = { .dim = {4 ,4}, .rank = 2 };
    test_assert(ak_desc_compute_strides(&x1, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to initialize tensor");

    test_assert(x1.strides[0] == 4, "got first stride of %lu", x0.strides[0]);
    test_assert(x1.strides[1] == 1, "got second stride of %lu", x0.strides[1]);

    // Coalesced dimensions
    size_t expected_strides_a[] = {16, 4, 1};
    size_t expected_strides_b[] = {0, 4, 1};

    test_assert(ak_create_broadcast_space((AK_StridedDesc*[]){&x0, &x1}, 2) == AK_StatusKind_OK, "failed to broadcast tensors");
    test_assert(ak_sort_axes((AK_StridedDesc*[]){&x0, &x1}, 2) == AK_StatusKind_OK, "failed to sort dmis");
    test_assert_array_eq(expected_strides_a, x0.strides, 3, "%llu");
    test_assert_array_eq(expected_strides_b, x1.strides, 3, "%llu");
    test_assert(ak_coalesce_axes((AK_StridedDesc*[]){&x0, &x1}, 2) == AK_StatusKind_OK, "failed to coalesce dims");
    test_assert(x0.strides[0] == 1, "%lu");
    
    return TEST_STATUS_OK;
}

void *alloc_libc(void *_, size_t bytes) {
    (void)_;
    return calloc(bytes, 1);
}

void free_libc(void* _, void *ptr) {
    (void)_;
    return free(ptr);
}

test_status test_cpu_add_basic() {
    AK_StridedDesc y = { .dim = {4, 4, 12}, .rank = 3 },
              x0 = { .dim = {4, 1, 12}, .rank = 3 },
              x1 = { .dim = {1, 4, 12}, .rank = 3 };

    test_assert(ak_desc_compute_strides(&y, AK_LayoutKind_RowMajor, 7) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x1, AK_LayoutKind_ColumnMajor, 2) == AK_StatusKind_OK, "failed to lay out tensor");

    size_t y_size = ak_desc_size_in_bytes(y);
    size_t x0_size = ak_desc_size_in_bytes(x0);
    size_t x1_size = ak_desc_size_in_bytes(x1);
    ak_scalar *y_data = calloc(y_size, 1);
    ak_scalar *x0_data = calloc(x0_size, 1);
    ak_scalar *x1_data = calloc(x1_size, 1);

    test_assert(ak_create_broadcast_space(((AK_StridedDesc*[]){&y, &x0, &x1}), 3) == AK_StatusKind_OK, "failed to broadcast tensors");
    test_assert(ak_sort_axes(((AK_StridedDesc*[]){&y, &x0, &x1}), 3) == AK_StatusKind_OK, "failed to sort dims");

    size_t coords[AK_MAX_RANK] = {0};
    do {
        size_t x0_idx = 0;
        size_t x1_idx = 0;
        for (size_t i = 0; i < y.rank; i++) {
            x0_idx += x0.strides[i] * coords[i];
            x1_idx += x1.strides[i] * coords[i];
        }
        x0_data[x0_idx] = 1.0f;
        x1_data[x1_idx] = 2.0f;
    } while (increment_coords_rtl(coords, y.dim, y.rank));

    AK_RT_CPU_Add(y, y_data, x0, x0_data, x1, x1_data);

    for (size_t i = 0; i < AK_MAX_RANK; i++) {
        coords[i] = 0;
    }

    do {
        size_t idx = 0;
        for (size_t i = 0; i < y.rank; i++) {
            idx += y.strides[i] * coords[i];
        }
        test_assert(y_data[idx] == 3.0f, "wanted 3, got %f", y_data[idx]);
    } while (increment_coords_rtl(coords, y.dim, y.rank));

    free(y_data);
    free(x0_data);
    free(x1_data);

    return TEST_STATUS_OK;
}

test_status test_cpu_add_vec() {
    AK_StridedDesc y = { .dim = {3}, .rank = 1 },
              x0 = { .dim = {3}, .rank = 1 },
              x1 = { .dim = {3}, .rank = 1 };

    test_assert(ak_desc_compute_strides(&y, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x1, AK_LayoutKind_ColumnMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");

    size_t y_size = ak_desc_size_in_bytes(y);
    size_t x0_size = ak_desc_size_in_bytes(x0);
    size_t x1_size = ak_desc_size_in_bytes(x1);
    ak_scalar *y_data = calloc(y_size, 1);
    ak_scalar *x0_data = calloc(x0_size, 1);
    ak_scalar *x1_data = calloc(x1_size, 1);

    test_assert(ak_create_broadcast_space(((AK_StridedDesc*[]){&y, &x0, &x1}), 3) == AK_StatusKind_OK, "failed to broadcast tensors");

    size_t coords[AK_MAX_RANK] = {0};
    do {
        size_t x0_idx = 0;
        size_t x1_idx = 0;
        for (size_t i = 0; i < y.rank; i++) {
            x0_idx += x0.strides[i] * coords[i];
            x1_idx += x1.strides[i] * coords[i];
        }
        x0_data[x0_idx] = 1.0f;
        x1_data[x1_idx] = 2.0f;
    } while (increment_coords_rtl(coords, y.dim, y.rank));

    AK_RT_CPU_Add(y, y_data, x0, x0_data, x1, x1_data);

    for (size_t i = 0; i < AK_MAX_RANK; i++) {
        coords[i] = 0;
    }

    do {
        size_t idx = 0;
        for (size_t i = 0; i < y.rank; i++) {
            idx += y.strides[i] * coords[i];
        }
        test_assert(y_data[idx] == 3.0f, "wanted 3, got %f", y_data[idx]);
    } while (increment_coords_rtl(coords, y.dim, y.rank));;;

    free(y_data);
    free(x0_data);
    free(x1_data);

    return TEST_STATUS_OK;
}

test_status test_cpu_matmul() {
    AK_StridedDesc y = { .dim = {2, 2}, .rank = 2 },
              x0 = { .dim = {2, 2}, .rank = 2 },
              x1T = { .dim = {2, 2}, .rank = 2 };

    test_assert(ak_desc_compute_strides(&y, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x1T, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");

    {
        size_t expected_dim[] = {2, 2};
        size_t expected_strides[] = {2, 1};
        
        test_assert_array_eq(expected_dim, y.dim, 2, "%lu");
        test_assert_array_eq(expected_strides, y.strides, 2, "%lu");
        test_assert_array_eq(expected_dim, x0.dim, 2, "%lu");
        test_assert_array_eq(expected_strides, x0.strides, 2, "%lu");
        test_assert_array_eq(expected_dim, x1T.dim, 2, "%lu");
        test_assert_array_eq(expected_strides, x1T.strides, 2, "%lu");
    }

    ak_scalar x0_data[4] = {1, 2, 3, 4};
    ak_scalar x1_data[4] = {5, 6, 7, 8};
    ak_scalar y_data[4] = {0};

    AK_StridedDesc y_cpy = y;

    test_assert(ak_create_contraction_space(&y_cpy, &x0, &x1T, 0) == AK_StatusKind_OK, "failed to contract output dims");

    {
        size_t expected_dim[] = {2, 2, 2};
        size_t y_expected_strides[] = {2, 1, 0};
        size_t x0_expected_strides[] = {2, 0, 1};
        size_t x1_expected_strides[] = {0, 1, 2};

        test_assert(y_cpy.rank == 3, "y rank: %lu", y_cpy.rank);
        test_assert(x0.rank == 3, "x0 rank: %lu", x0.rank);
        test_assert(x1T.rank == 3, "x1T rank: %lu", x1T.rank);

        test_assert_array_eq(expected_dim, y_cpy.dim, 3, "%lu");
        test_assert_array_eq(y_expected_strides, y_cpy.strides, 3, "%lu");
        test_assert_array_eq(expected_dim, x0.dim, 3, "%lu");
        test_assert_array_eq(x0_expected_strides, x0.strides, 3, "%lu");
        test_assert_array_eq(expected_dim, x1T.dim, 3, "%lu");
        test_assert_array_eq(x1_expected_strides, x1T.strides, 3, "%lu");
    }

    ak_scalar expected_out[] = {19, 22, 43, 50};

    AK_RT_CPU_Contract(y_cpy, y_data, x0, x0_data, x1T, x1_data);
    test_assert_array_eq(expected_out, y_data, 4, "%f");

    return TEST_STATUS_OK;
}

test_status test_cpu_matmul_batch() {
    AK_StridedDesc y = { .dim = {2, 2, 2}, .rank = 3 },
              x0 = { .dim = {2, 2, 2}, .rank = 3 },
              x1T = { .dim = {2, 2, 2}, .rank = 3 };

    test_assert(ak_desc_compute_strides(&y, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x0, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
    test_assert(ak_desc_compute_strides(&x1T, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");

    {
        size_t expected_dim[] = {2, 2, 2};
        size_t expected_strides[] = {4, 2, 1};
        
        test_assert_array_eq(expected_dim, y.dim, 3, "%lu");
        test_assert_array_eq(expected_strides, y.strides, 3, "%lu");
        test_assert_array_eq(expected_dim, x0.dim, 3, "%lu");
        test_assert_array_eq(expected_strides, x0.strides, 3, "%lu");
        test_assert_array_eq(expected_dim, x1T.dim, 3, "%lu");
        test_assert_array_eq(expected_strides, x1T.strides, 3, "%lu");
    }

    ak_scalar x0_data[8] = {
        1, 2, 3, 4,
        1, 2, 3, 4,
    };
    ak_scalar x1_data[8] = {
        5, 6, 7, 8,
        5, 6, 7, 8,
    };
    ak_scalar y_data[8] = {0};

    AK_StridedDesc y_cpy = y;

    test_assert(ak_create_contraction_space(&y_cpy, &x0, &x1T, 1) == AK_StatusKind_OK, "failed to contract output dims");

    {
        size_t expected_dim[] = {2, 2, 2, 2};
        size_t y_expected_strides[] = {4, 2, 1, 0};
        size_t x0_expected_strides[] = {4, 2, 0, 1};
        size_t x1_expected_strides[] = {4, 0, 1, 2};

        test_assert(y_cpy.rank == 4, "y rank: %lu", y_cpy.rank);
        test_assert(x0.rank == 4, "x0 rank: %lu", x0.rank);
        test_assert(x1T.rank == 4, "x1T rank: %lu", x1T.rank);

        test_assert_array_eq(expected_dim, y_cpy.dim, 4, "%lu");
        test_assert_array_eq(y_expected_strides, y_cpy.strides, 4, "%lu");
        test_assert_array_eq(expected_dim, x0.dim, 4, "%lu");
        test_assert_array_eq(x0_expected_strides, x0.strides, 4, "%lu");
        test_assert_array_eq(expected_dim, x1T.dim, 4, "%lu");
        test_assert_array_eq(x1_expected_strides, x1T.strides, 4, "%lu");
    }

    ak_scalar expected_out[] = {
        19, 22, 43, 50,
        19, 22, 43, 50,
    };

    AK_RT_CPU_Contract(y_cpy, y_data, x0, x0_data, x1T, x1_data);
    test_assert_array_eq(expected_out, y_data, 8, "%f");

    return TEST_STATUS_OK;
}

// TODO: reimplement after migration
// test_status test_expr_alloc() {
//     AK_Allocator allocator = {
//         .Alloc = alloc_libc,
//         .Free = free_libc,
//     };

//     AK_Expr expr = {0};
//     uint8_t *expr_buf;
//     size_t expr_fields_bytes_allocated;
//     test_assert(AK_AllocExpr(&allocator, &expr_buf, &expr_fields_bytes_allocated, &expr, 32) == AK_StatusKind_OK, "failed to allocate expr");
//     test_assert(0 < expr_fields_bytes_allocated, "failed to allocate expr");

//     struct ak_tensor x0 = { .desc.rank = 1, .desc.dim = {4} },
//                         x1 = { .desc.rank = 1, .desc.dim = {4} };
    
//     test_assert(ak_desc_compute_strides(&x0.desc, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");
//     test_assert(ak_desc_compute_strides(&x1.desc, AK_LayoutKind_RowMajor, 1) == AK_StatusKind_OK, "failed to lay out tensor");

//     ak_scalar x0_vals[4] = {1, 2, 3, 4},
//               x1_vals[4] = {3, 3, 1, 4};
//     test_assert(sizeof(x0_vals) == ak_desc_size_in_bytes(x0.desc), "wrong size");
//     test_assert(sizeof(x1_vals) == ak_desc_size_in_bytes(x1.desc), "wrong size");
//     x0.data = x0_vals;
//     x1.data = x1_vals;
    
//     struct ak_tensor y0;
//     test_assert(AK_AppendAdd(&expr, &y0, x0, x1) == AK_StatusKind_OK, "failed to append add node");

//     struct ak_tensor y1;
//     test_assert(AK_AppendAdd(&expr, &y1, y0, x1) == AK_StatusKind_OK, "failed to append add node");

//     test_assert(AK_CompileExpr(&expr) == AK_StatusKind_OK, "failed to compile expr");

//     ak_scalar *data_buf;
//     size_t expr_data_bytes_allocated = 0;
//     test_assert(AK_AllocExprData(&allocator, &allocator, &data_buf, &expr_data_bytes_allocated, &expr) == AK_StatusKind_OK, "failed to allocate expr");
//     test_assert(data_buf == expr.nodes[0].y.data, "wanted %p; got %p", data_buf, expr.nodes[0].y.data);
//     test_assert(8 * sizeof(ak_scalar) == expr_data_bytes_allocated, "did not alias buffers, got %lu", expr_data_bytes_allocated);
//     // test_assert(data_buf == expr.nodes[1].y.data, "wanted %p; got %p", data_buf, expr.y[1].data); 
//     // fuck you, gcc ubsan. corrupts the fucking pointer
//     test_assert(AK_RT_CPU_ExecExpr(expr) == AK_StatusKind_OK, "failed to exectute expr");

//     // The buffers should not alias, so . . .
//     // x0 + x1 = y0 = {4, 5, 4, 8};
//     ak_scalar expected_data[4] = {4, 5, 4, 8};
//     test_assert_array_eq(expected_data, expr.nodes[0].y.data, 4, "%.2f");
//     // test_assert_array_eq(expected_data, expr.nodes[1].data, 4, "%f");
//     // still corrupts the fucking pointer

//     free(expr_buf);
//     free(data_buf);

//     return TEST_STATUS_OK;
// }

int main(void) {
    test_run(tensor_layout);
    test_run(tensor_size);
    test_run(tensor_aligned_views_not_compatible);
    test_run(tensor_aligned_views);
    test_run(cpu_add_basic);
    test_run(cpu_add_vec);
    test_run(cpu_matmul);
    test_run(cpu_matmul_batch);
    // test_run(expr_alloc);
    return 0;
}
