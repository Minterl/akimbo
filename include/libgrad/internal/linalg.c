#include <libgrad/internal/base.h>
#include <libgrad/internal/linalg.h>

size_t
lg_vfmt_lshape_ptr(va_list ap, LG_Writer *writer) {
    size_t written = 0;

    LG_LogicalShape *shape = va_arg(ap, LG_LogicalShape*);

    written += lg_write(writer, lg_str8_lit("{"));
    for (size_t i = 0; i < shape->rank; i++) {
        written += lg_printf(writer, lg_str8_lit("%{i64}"), shape->dim[i]);
        if (i != shape->rank - 1) {
            written += lg_write(writer, lg_str8_lit(" x "));
        }
    }
    written += lg_write(writer, lg_str8_lit("}\n"));

    return written;
}

LG_StatusKind
lg_atran_strided_projection_from_shape(
    LG_Arena *arena,
    const LG_LogicalShape *shape,
    LG_LayoutKind layout,
    uint32_t unit_align,
    LG_AffineTransform **out_atran
) {
    lg_assert(out_atran != NULL);

    LG_AffineTransform *atran = lg_arena_alloc_famstruct(arena, LG_AffineTransform, shape->rank * sizeof(uint64_t));
    if (atran == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    atran->n_rows = 1;
    atran->n_cols = shape->rank;

    int64_t *const A = lg_atran_get_A(atran);

    uint8_t last_stride = 1;
    for (uint8_t i = 1; i <= shape->rank; i++) {
        uint8_t axis = (layout == LG_LayoutKind_RowMajor) ? (uint8_t)(shape->rank - i) : (uint8_t)(i - 1);
        A[axis] = last_stride;

        last_stride *= shape->dim[shape->rank - i];

        // Conceptually, we only pad the rightmost dimension.
        // However, this affects the stride of the second-rightmost dimension first
        // (and then all subsequent dimensions).
        if (unit_align > 1 && i == 1) {
            last_stride = (last_stride + unit_align - 1) & ~(unit_align - 1);
        }
    }

    lg_assert(lg_atran_is_valid_address_operator(atran));

    *out_atran = atran;

    return LG_StatusKind_OK;
}

void
lg_atran_apply(
    const LG_AffineTransform *atran,
    const int64_t *x, // must be an array of length atran.n_cols
    int64_t *y // must be an array of length atran.n_rows
) {
    const int64_t *const restrict A = lg_atran_get_A(atran);
    const int64_t *const restrict b = lg_atran_get_b(atran);

    lg_memzero(y, atran->n_rows * sizeof(int64_t));

    for (uint8_t i_rows = 0; i_rows < atran->n_rows; i_rows++) {
        for (uint8_t i_cols = 0; i_cols < atran->n_cols; i_cols++) {
            y[i_rows] += A[atran->n_cols*i_rows + i_cols] * x[i_cols];
        }
    }

    for (uint8_t i_rows = 0; i_rows < atran->n_rows; i_rows++) {
        y[i_rows] += b[i_rows];
    }
}

LG_StatusKind
lg_poly_make_parallelotope(
    LG_Arena *arena,
    LG_Polyhedron **out_poly,
    uint8_t rank,
    int64_t *lower,
    int64_t *upper
) {
    bool is_canonical_aabb = true;
    for (uint8_t i = 0; i < rank; i++) {
        if (lower[i] != 0 || upper[i] < 0) {
            is_canonical_aabb = false;
            break;
        }
    }

    if (is_canonical_aabb) {
        LG_Polyhedron *poly = lg_arena_alloc_famstruct(arena, LG_Polyhedron, rank * sizeof(int64_t));
        if (poly == NULL) {
            return LG_StatusKind_OutOfMemory;
        }

        poly->repr_kind = LG_PolyhedronReprKind_CanonicalAABB;
        poly->as.canonical_aabb.rank = rank;

        int64_t *const extents = lg_canonical_aabb_get_extents(poly);
        lg_memcpy(extents, upper, rank * sizeof(int64_t));
    } else {
        const uint8_t n_cols = rank;
        const uint8_t n_rows = rank * 2;

        LG_Polyhedron *poly = lg_arena_alloc_famstruct(arena, LG_Polyhedron, rank * sizeof(int64_t));
        if (poly == NULL) {
            return LG_StatusKind_OutOfMemory;
        }

        lg_memzero(out_poly, sizeof(LG_Polyhedron));
        poly->repr_kind = LG_PolyhedronReprKind_Hyperplane;
        poly->as.hyperplane.n_cols = n_cols;
        poly->as.hyperplane.n_rows = n_rows;

        int64_t *A = lg_hpoly_get_A(poly);
        int64_t *b = lg_hpoly_get_b(poly);

        for (uint8_t i = 0; i < rank; i++) {
            A[2*i * poly->as.hyperplane.n_cols + i] = -1;
            A[(2*i + 1) * poly->as.hyperplane.n_cols + i] = 1;
            b[2*i] = -lower[i];
            b[2*i + 1] = upper[i];
        }
    }

    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_infer_broadcasted_dims(
    LG_LogicalShape *lg_nullable out,
    const LG_LogicalShape **shapes,
    size_t n_descs
) {
    size_t max_rank = 0;
    for (size_t i = 0; i < n_descs; i++) {
        if (shapes[i]->rank > max_rank) {
            max_rank = shapes[i]->rank;
        }
    }

    // Every tensor participating in the tracking must be broadcast-compatible
    // with every other tensor.
    // Naively, we would perform an O(n^2) check across a matrix of all of the participating tensors.
    // The shortcut below is equivalent.
    //
    // Tensors are broadcast-compatible if all of their dimensions are broadcast-compatible.
    // Two dimensions are broadcast-compatible if one of three things is true:
    // 1) The dimensions are the same.
    // 2) One of the dimensions is 1.
    // 3) One of the dimensions does not exist.
    
    size_t master_dim[LG_MAX_RANK];
    for (size_t i = 0; i < max_rank; i++) {
        master_dim[i] = 1;
    }

    for (size_t i_desc = 0; i_desc < n_descs; i_desc++) {
        for (size_t i_axis = 0; i_axis < shapes[i_desc]->rank; i_axis++) {
            const size_t dim_desc = shapes[i_desc]->dim[shapes[i_desc]->rank - i_axis - 1];
            size_t *const dim_master = &master_dim[max_rank - i_axis - 1];
            if (dim_desc == 1) {
                continue;
            }
            if (*dim_master == 1) {
                *dim_master = dim_desc;
            } else if (*dim_master != dim_desc) {
                return LG_StatusKind_ShapeMismatch;
            }
        }
    }

    if (out != NULL) {
        out->rank = max_rank;
        for (size_t i = 0; i < LG_MAX_RANK; i++) {
            out->dim[i] = master_dim[i];
        }
    }

    return LG_StatusKind_OK;
} 

LG_StatusKind 
lg_infer_contracted_dims(
    LG_LogicalShape *lg_nullable out_y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
) {
    if (x0->rank < n_contracted_axes || n_contracted_axes + n_batch_axes > x1->rank) {
        return LG_StatusKind_InvalidArgument;
    }

    // repeated below
    const size_t x0_first_contracted_axis = x0->rank - n_contracted_axes;
    const size_t x1_first_free_axis = n_contracted_axes + n_batch_axes;

    size_t rank = 0;

    size_t dim[LG_MAX_RANK] = {0};
    for (size_t i = n_batch_axes; i < x0_first_contracted_axis; i++, rank++) {
        dim[rank] = x0->dim[i];
    }
    for (size_t i = x1->rank; i > x1_first_free_axis; i--, rank++) {
        dim[rank] = x1->dim[i - 1];
    }

    if (out_y != NULL) {
        out_y->rank = rank;
        for (size_t i = 0; i < rank; i++) {
            out_y->dim[i] = dim[i];
        }
    }

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_create_broadcasted_iteration_space(
    LG_Arena *arena,
    const LG_LogicalShape *y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    LG_MappedSpace *out_space
) {
    // TODO: think of a way to do this function without duplicating the work

    lg_assert(out_space != NULL);

    LG_LogicalShape y_should;
    LG_StatusKind status = lg_infer_broadcasted_dims(&y_should, (const LG_LogicalShape*[]){x0, x1}, 2);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    if (y->rank != y_should.rank) {
        return LG_StatusKind_InvalidRank;
    }
    for (uint8_t i = 0; i < y_should.rank; i++) {
        if (y_should.dim[i] != y->dim[i]) {
            return LG_StatusKind_ShapeMismatch;
        }
    }

    const size_t iter_domain_rank = y->rank;
    LG_Polyhedron *iter_domain = lg_arena_alloc_famstruct(arena, LG_Polyhedron, iter_domain_rank * sizeof(uint64_t));
    if (iter_domain == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    iter_domain->repr_kind = LG_PolyhedronReprKind_CanonicalAABB,
    iter_domain->as.canonical_aabb.rank = iter_domain_rank;
    int64_t *iter_domain_extents = lg_canonical_aabb_get_extents(iter_domain);
    lg_memcpy(iter_domain_extents, y->dim, iter_domain_rank * sizeof(uint64_t));

    LG_AffineTransform *to_y_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * y->rank * sizeof(uint64_t));
    if (to_y_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }
    LG_AffineTransform *to_x0_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * x0->rank * sizeof(uint64_t));
    if (to_x0_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }
    LG_AffineTransform *to_x1_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * x1->rank * sizeof(uint64_t));
    if (to_x1_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    int64_t *const restrict A_y  = lg_atran_get_A(to_y_coords);
    int64_t *const restrict A_x0 = lg_atran_get_A(to_x0_coords);
    int64_t *const restrict A_x1 = lg_atran_get_A(to_x1_coords);

    for (uint8_t i_rev = 1; i_rev <= iter_domain_rank; i_rev++) {
        uint8_t r = iter_domain_rank - i_rev;
        if (x0->dim[r] != y->dim[r]) {
            lg_assert(x0->dim[r] == 1);
            A_x1[r*x1->rank + r] = 0;
        }
        if (x1->dim[r] != y->dim[r]) {
            lg_assert(x1->dim[r] == 1);
            A_x0[r*x1->rank + r] = 0;
        }
        A_y[r*y->rank + r] = 1;
    }

    // The resulting state of our tensor views looks like this:
    // - All tensors have the same logical rank
    // - All tensors have the exact same logical dims
    // - The only thing that changes between tensor views is
    //   striding.

    lg_memzero(out_space, sizeof(LG_MappedSpace));

    out_space->iteration_domain = iter_domain;
    out_space->to_y_coords      = to_y_coords;
    out_space->to_x0_coords     = to_x0_coords;
    out_space->to_x1_coords     = to_x1_coords;

    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_create_contracted_iteration_space(
    LG_Arena *arena,
    const LG_LogicalShape *y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    size_t n_batch_axes,
    LG_MappedSpace *out_space
) {
    lg_assert(out_space != NULL);

    LG_StatusKind status = LG_StatusKind_OK;

    if (
        n_batch_axes > y->rank ||
        n_batch_axes > x0->rank ||
        n_batch_axes > x1->rank
    ) {
        return LG_StatusKind_InvalidArgument;
    }

    ////////////////////////////////////////////////////////////////////
    /// ~~ Important terms & explanation ~~
    
    // The easiest way to picture how this works is to pretend that
    // these are just strided buffer descriptors.
    //
    // The strides over the iteration space will/would look like this:
    // { [batch], [x0_free], [x1_free], [contracted] }
    //    reg      reg        reg        0           | y strides
    //    reg      reg        0          reg         | x0 strides
    //    reg      0          reg        reg         | x1 strides
    //
    // So, the iteration space itself will be a canonical AABB,
    // and so the affine maps must be projections 
    // C : R^(iteration domain rank) -> R^(index domain rank) s.t some
    // coordinate in the iteration domain may map to the same coordinate
    // in the index domain where the corresponding index domain axis
    // "stride" is zero. That looks like a diagonal matrix, something like
    // this: 
    // { 1, 0, 0 } // a non-zero axis
    // { 0, 1, 0 } // a non-zero axis
    // { 0, 0, 0 } // a zero-strided axis 
    //
    // Combined with accumulation semantics, this can be used to reduce
    // along the zero-strided axes.
    //
    // In English: this is a highly generalized version of matrix 
    // multiplication.

    // x0.rank = n_batch + n_contracted + x0_free
    // x1.rank = n_batch + n_contracted + x1_free
    // y.rank = n_batch + x0_free + x1_free
    // ergo ...
    const size_t n_contracted_axes = (x0->rank + x1->rank - y->rank - n_batch_axes) / 2;
    const size_t iter_domain_rank = y->rank + n_contracted_axes;
    const size_t x0_first_contracted_axis = x0->rank - n_contracted_axes;
    const size_t x1_first_free_axis = n_contracted_axes + n_batch_axes;
    const size_t x0_n_free_axes = x0->rank - n_batch_axes - n_contracted_axes;
    const size_t x1_n_free_axes = x1->rank - n_batch_axes - n_contracted_axes;

    lg_assert(n_contracted_axes < LG_MAX_RANK);
    lg_assert(iter_domain_rank < LG_MAX_RANK);
    lg_assert(x0_first_contracted_axis < LG_MAX_RANK);
    lg_assert(x1_first_free_axis < LG_MAX_RANK);
    lg_assert(x0_n_free_axes < LG_MAX_RANK);
    lg_assert(x1_n_free_axes < LG_MAX_RANK);


    ////////////////////////////////////////////////////////////////////
    /// ~~ Calculate iteration domain extents & map to transforms ~~

    LG_Polyhedron *iter_domain = lg_arena_alloc_famstruct(arena, LG_Polyhedron, iter_domain_rank * sizeof(uint64_t));
    if (iter_domain == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    LG_AffineTransform *to_y_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * y->rank * sizeof(uint64_t));
    if (to_y_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }
    LG_AffineTransform *to_x0_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * x0->rank * sizeof(uint64_t));
    if (to_x0_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }
    LG_AffineTransform *to_x1_coords = lg_arena_alloc_famstruct(arena, LG_AffineTransform, iter_domain_rank * x1->rank * sizeof(uint64_t));
    if (to_x1_coords == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    int64_t *iter_domain_extents = lg_canonical_aabb_get_extents(iter_domain);

    iter_domain->repr_kind = LG_PolyhedronReprKind_CanonicalAABB;
    iter_domain->as.canonical_aabb.rank = iter_domain_rank;

    int64_t *const restrict A_y  = lg_atran_get_A(to_y_coords);
    int64_t *const restrict A_x0 = lg_atran_get_A(to_x0_coords);
    int64_t *const restrict A_x1 = lg_atran_get_A(to_x1_coords);

    {
        uint8_t r = 0;
        
        // Batch axes
        for (uint8_t i = 0; i < n_batch_axes; i++, r++) {
            if (
                y->dim[r] != x0->dim[r] ||
                y->dim[r] != x1->dim[r] ||
                x0->dim[r] != x1->dim[r]
            ) {
                return LG_StatusKind_ShapeMismatch;
            }
            A_y[r*y->rank + r] = 1;
            A_x0[r*x0->rank + r] = 1;
            A_x1[r*x1->rank + r] = 1;
            iter_domain_extents[r] = x0->dim[r];
        }

        // Free axes
        for (uint8_t i = n_batch_axes; i < x0_first_contracted_axis; i++, r++) {
            if (y->dim[r] != x0->dim[i]) {
                return LG_StatusKind_ShapeMismatch;
            }
            A_y[r*y->rank + r] = 1;
            A_x0[r*x0->rank + r] = 1;
            A_x1[r*x1->rank + r] = 0;
            iter_domain_extents[r] = x0->dim[i];
        }
        for (uint8_t i = x1_first_free_axis; i < x1->rank; i++, r++) {
            if (y->dim[r] != x1->dim[i]) {
                return LG_StatusKind_ShapeMismatch;
            }
            A_y[r*y->rank + r] = 1;
            A_x0[r*x0->rank + r] = 0;
            A_x1[r*x1->rank + r] = 1;
            iter_domain_extents[r] = x1->dim[i];
        }

        // Contracted axes
        if (n_contracted_axes > 0) {
            for (
                uint8_t x0_ax = x0_first_contracted_axis, x1_ax = x1_first_free_axis - 1;
                x0_ax < x0->rank; // x1_ax > 0
                x0_ax++, x1_ax--, r++
            ) {
                lg_assert(x1_ax > 0);
                if (x0->dim[x0_ax] != x1->dim[x1_ax]) {
                    return LG_StatusKind_ShapeMismatch;
                }
                A_y[r*y->rank + r] = 0;
                A_x0[r*x0->rank + r] = 1;
                A_x1[r*x1->rank + r] = 1;
                iter_domain_extents[r] = x0->dim[x0_ax];
            }
        }

        lg_assert(r == iter_domain_rank);
    }


    ///////////////////////////////////////////
    /// ~~ fin ~~

    lg_memzero(out_space, sizeof(LG_MappedSpace));

    out_space->iteration_domain = iter_domain;
    out_space->to_y_coords      = to_y_coords;
    out_space->to_x0_coords     = to_x0_coords;
    out_space->to_x1_coords     = to_x1_coords;

    lg_assert(status == LG_StatusKind_OK);

    return status;
}
