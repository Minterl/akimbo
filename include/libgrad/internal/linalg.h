#ifndef LG_LINALG_H_
#define LG_LINALG_H_

#include <libgrad/internal/base.h>
#include <libgrad/internal/expr.h>
 
/// Maximum possible shape rank
/// All shapes will have an array of this size to store
/// dims, so  keep this to a minimum.
#ifndef LG_MAX_RANK
#   define LG_MAX_RANK 8
#endif // LG_MAX_RANK
 
/// Layout of a physical buffer
typedef enum
LG_LayoutKind {
    LG_LayoutKind_RowMajor,
    LG_LayoutKind_ColumnMajor,
} LG_LayoutKind;

typedef struct
LG_LogicalShape {
    size_t rank;
    size_t dim[LG_MAX_RANK];
} LG_LogicalShape;

typedef uint8_t 
LG_PolyhedronReprKind;
enum {
    LG_PolyhedronReprKind_Hyperplane,
    LG_PolyhedronReprKind_CanonicalAABB,
};

/// Hyperplane representation of a convex polyhedron bounded by
/// the inequalities Ax <= b; stored row-major
typedef struct
LG_HPolyhedron {
    uint8_t  n_rows, n_cols;

    // what this would look like with separate pointers:
    // int64_t  *A; // R^(rows, cols)
    // int64_t  *b; // R^(rows)
} LG_HPolyhedron;

/// Represents a polyhedron in R^(rank) whos lower bounds exist at the origin,
/// and whos upper bounds are all positive i.e the furthest point
/// from the origin in a CanonicalAABB is always in the first
/// orthant.
typedef struct
LG_CanonicalAABB {
    uint8_t  rank;
} LG_CanonicalAABB;

typedef union
LG_PolyhedronRepr {
    LG_HPolyhedron    hyperplane;
    LG_CanonicalAABB  canonical_aabb;
} LG_PolyhedronRepr;

typedef struct
LG_Polyhedron {
    LG_PolyhedronReprKind  repr_kind;
    LG_PolyhedronRepr      as; 
    int64_t                data[];
} LG_Polyhedron;

/// Affine map representing the equation y = Ax + b
/// stored row-major
typedef struct
LG_AffineTransform {
    uint8_t  n_rows, n_cols;
    int64_t  data[];
    
    // what this would look like with separate pointers:
    // int64_t  *A; // R^(rows, cols)
    // int64_t  *b; // R^(rows)
} LG_AffineTransform;

typedef struct
LG_MappedSpace {
    LG_Polyhedron       *iteration_domain;
    LG_AffineTransform  *to_y_coords;
    LG_AffineTransform  *to_x0_coords;
    LG_AffineTransform  *to_x1_coords;
} LG_MappedSpace;

#define lg_hpoly_get_A(poly) ((poly)->data)
#define lg_hpoly_get_b(poly) ((poly)->data + ((size_t)((poly)->as.hyperplane.n_rows) * (poly)->as.hyperplane.n_cols))

#define lg_canonical_aabb_get_extents(poly) ((poly)->data)

#define lg_atran_get_A(atran) ((atran)->data)
#define lg_atran_get_b(atran) ((atran)->data + ((size_t)((atran)->n_rows) * (atran)->n_cols))
#define lg_atran_is_valid_address_operator(atran) ((atran)->n_rows == 1)
#define lg_atran_get_x_len(atran) ((atran)->n_cols)
#define lg_atran_get_y_len(atran) ((atran)->n_rows)

void
lg_lshape_fmt(LG_LogicalShape *shape, LG_Writer *writer);

LG_StatusKind
lg_poly_make_parallelotope(
    LG_Arena *arena,
    LG_Polyhedron **out_poly,
    uint8_t rank,
    int64_t *lower,
    int64_t *upper
);

LG_StatusKind
lg_atran_strided_projection_from_shape(
    LG_Arena *arena,
    const LG_LogicalShape *shape,
    LG_LayoutKind layout,
    uint32_t unit_align,
    LG_AffineTransform **out_atran
);

void
lg_atran_apply(
    const LG_AffineTransform *atran,
    const int64_t *x, // must be an array of length atran.n_cols
    int64_t *y // must be an array of length atran.n_rows
);

LG_StatusKind 
lg_infer_contracted_dims(
    LG_LogicalShape *lg_nullable out_y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);

LG_StatusKind 
lg_infer_broadcasted_dims(
    LG_LogicalShape *lg_nullable out,
    const LG_LogicalShape **shapes,
    size_t n_descs
);

LG_StatusKind
lg_create_broadcasted_iteration_space(
    LG_Arena *arena,
    const LG_LogicalShape *y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    LG_MappedSpace *out_space
);

LG_StatusKind 
lg_create_contracted_iteration_space(
    LG_Arena *arena,
    const LG_LogicalShape *y,
    const LG_LogicalShape *x0,
    const LG_LogicalShape *x1,
    size_t n_batch_axes,
    LG_MappedSpace *out_space
);

#endif // LG_LINALG_H_c
