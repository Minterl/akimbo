#ifndef AK_LINALG_H_
#define AK_LINALG_H_

#include <akimbo/internal/base.h>
#include <akimbo/internal/expr.h>
 
/// Maximum possible shape rank
/// All shapes will have an array of this size to store
/// dims, so  keep this to a minimum.
#ifndef AK_MAX_RANK
#   define AK_MAX_RANK 8
#endif // AK_MAX_RANK
 
/// Layout of a physical buffer
typedef enum
AK_LayoutKind {
    AK_LayoutKind_RowMajor,
    AK_LayoutKind_ColumnMajor,
} AK_LayoutKind;

typedef struct
AK_LogicalShape {
    size_t rank;
    size_t dim[AK_MAX_RANK];
} AK_LogicalShape;

typedef uint8_t 
AK_PolyhedronReprKind;
enum {
    AK_PolyhedronReprKind_Hyperplane,
    AK_PolyhedronReprKind_CanonicalAABB,
};

/// Hyperplane representation of a convex polyhedron bounded by
/// the inequalities Ax <= b; stored row-major
typedef struct
AK_HPolyhedron {
    uint8_t  n_rows, n_cols;

    // what this would look like with separate pointers:
    // int64_t  *A; // R^(rows, cols)
    // int64_t  *b; // R^(rows)
} AK_HPolyhedron;

/// Represents a polyhedron in R^(rank) whos lower bounds exist at the origin,
/// and whos upper bounds are all positive i.e the furthest point
/// from the origin in a CanonicalAABB is always in the first
/// orthant.
typedef struct
AK_CanonicalAABB {
    uint8_t  rank;
} AK_CanonicalAABB;

typedef union
AK_PolyhedronRepr {
    AK_HPolyhedron    hyperplane;
    AK_CanonicalAABB  canonical_aabb;
} AK_PolyhedronRepr;

typedef struct
AK_Polyhedron {
    AK_PolyhedronReprKind  repr_kind;
    AK_PolyhedronRepr      as; 
    int64_t                data[];
} AK_Polyhedron;

/// Affine map representing the equation y = Ax + b
/// stored row-major
typedef struct
AK_AffineTransform {
    uint8_t  n_rows, n_cols;
    int64_t  data[];
    
    // what this would look like with separate pointers:
    // int64_t  *A; // R^(rows, cols)
    // int64_t  *b; // R^(rows)
} AK_AffineTransform;

typedef struct
AK_MappedSpace {
    AK_Polyhedron       *iteration_domain;
    AK_AffineTransform  *to_y_coords;
    AK_AffineTransform  *to_x0_coords;
    AK_AffineTransform  *to_x1_coords;
} AK_MappedSpace;

#define ak_hpoly_get_A(poly) ((poly)->data)
#define ak_hpoly_get_b(poly) ((poly)->data + ((size_t)((poly)->as.hyperplane.n_rows) * (poly)->as.hyperplane.n_cols))

#define ak_canonical_aabb_get_extents(poly) ((poly)->data)

#define ak_atran_get_A(atran) ((atran)->data)
#define ak_atran_get_b(atran) ((atran)->data + ((size_t)((atran)->n_rows) * (atran)->n_cols))
#define ak_atran_is_valid_address_operator(atran) ((atran)->n_rows == 1)
#define ak_atran_get_x_len(atran) ((atran)->n_cols)
#define ak_atran_get_y_len(atran) ((atran)->n_rows)

void
ak_lshape_fmt(AK_LogicalShape *shape, AK_Writer *writer);

AK_StatusKind
ak_poly_make_parallelotope(
    AK_Arena *arena,
    AK_Polyhedron **out_poly,
    uint8_t rank,
    int64_t *lower,
    int64_t *upper
);

AK_StatusKind
ak_atran_strided_projection_from_shape(
    AK_Arena *arena,
    const AK_LogicalShape *shape,
    AK_LayoutKind layout,
    uint32_t unit_align,
    AK_AffineTransform **out_atran
);

void
ak_atran_apply(
    const AK_AffineTransform *atran,
    const int64_t *x, // must be an array of length atran.n_cols
    int64_t *y // must be an array of length atran.n_rows
);

AK_StatusKind 
ak_infer_contracted_dims(
    AK_LogicalShape *ak_nullable out_y,
    const AK_LogicalShape *x0,
    const AK_LogicalShape *x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);

AK_StatusKind 
ak_infer_broadcasted_dims(
    AK_LogicalShape *ak_nullable out,
    const AK_LogicalShape **shapes,
    size_t n_descs
);

AK_StatusKind
ak_create_broadcasted_iteration_space(
    AK_Arena *arena,
    const AK_LogicalShape *y,
    const AK_LogicalShape *x0,
    const AK_LogicalShape *x1,
    AK_MappedSpace *out_space
);

AK_StatusKind 
ak_create_contracted_iteration_space(
    AK_Arena *arena,
    const AK_LogicalShape *y,
    const AK_LogicalShape *x0,
    const AK_LogicalShape *x1,
    size_t n_batch_axes,
    AK_MappedSpace *out_space
);

#endif // AK_LINALG_H_
