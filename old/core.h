#ifndef AK_CORE_H_
#define AK_CORE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/// Maximum possible Tensor rank
/// All tensors will have an array of this size to store
/// dims, so  keep this to a minimum.
#ifndef AK_MAX_RANK
#   define AK_MAX_RANK 8
#endif // AK_MAX_RANK

/// The number of tensors tracked by `ak_nditer`.
#ifndef AK_N_TRACKED_TENSORS
#   define AK_N_TRACKED_TENSORS 4
#endif // AK_N_TRACKED_TENSORS 4

/// Type to back Tensor data
#ifndef ak_scalar
#   define ak_scalar float
#endif // ak_scalar

#if defined(__has_feature) && __has_feature(nullability)
#   define ak_nullable _Nullable
#else
#   define ak_nullable 
#endif // defined(__has_attribute) && __has_attribute(nullability)
       
#if defined(__has_attribute) && __has_attribute(unused) 
#   define ak_maybe_unused __attribute__((unused))
#else
#   define ak_maybe_unused
#endif // defined(__has_attribute) && __has_attribute(unused) 

#if defined(__has_builtin) && __has_builtin(__builtin_expect)
#   define ak_likely(expr)    __builtin_expect((expr), 1)
#   define ak_unlikely(expr)  __builtin_expect((expr), 0)
#else
#   define ak_likely(expr)    (expr)
#   define ak_unlikely(expr)  (expr)
#endif // defined(__has_builtin) && __has_builtin(__builtin_expect)

#define ak_nil(T) (T){0}

/// Bounds checking
#ifdef __cplusplus
#   define ak_check_bounds(x) /* nothing */
#   define ak_check_bounds_nullable(x) /* nothing */
#else
#   if defined(__clang__) && __has_attribute(counted_by)
#       define ak_check_bounds(x) __attribute__((counted_by(x)))
#       define ak_check_bounds_nullable(x) __attribute__((counted_by_or_null(x)))
#   elif defined(__GNUC__) && (__GNUC__ >= 16) // Pointer support introduced in GCC 16
#       define ak_check_bounds(x) __attribute__((counted_by(x)))
#       define ak_check_bounds_nullable(x) __attribute__((counted_by_or_null(x)))
#   else
#       define ak_check_bounds(x) /* nothing */
#       define ak_check_bounds_nullable(x) /* nothing */
#   endif
#endif

#if defined(__has_attribute) && __has_attribute(always_inline)
#   define ak_force_inline __attribute__((always_inline)) inline
#else
#   define ak_force_inline inline
#endif // defined(__has_attribute) && __has_attribute(always_inline)


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// Define status codes with a string reflection table
///
////////////////////////////////////////////////////////////////////////////////
 
#define ak_define_status_kinds \
    AK_X(OK), \
    AK_X(InvalidArgument), \
    AK_X(InvalidRank), \
    AK_X(ShapeMismatch), \
    AK_X(StrideMismatch), \
    AK_X(Overflow), \
    AK_X(NotFound), \
    AK_X(Duplicate), \
    AK_X(UnsupportedOpcode), \
    AK_X(OutOfMemory), \
    AK_X(OutOfBounds), \
    AK_X(UnexpectedNaN),

#define AK_X(x) AK_StatusKind_##x
    typedef enum
    AK_StatusKind {
        ak_define_status_kinds
    } AK_StatusKind;
#undef AK_X

// TODO: maybe these should be ak_str8s
#define AK_X(x) [AK_StatusKind_##x] = (const uint8_t*)#x
    ak_maybe_unused static const uint8_t*
    AK_STATUS_KIND_CSTRING_TABLE[] = {
        ak_define_status_kinds
    };
#undef AK_X

#define ak_status_kind_as_cstring(status) AK_STATUS_KIND_CSTRING_TABLE[(status)]


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// Core program data structures
///
////////////////////////////////////////////////////////////////////////////////

/// Layout of a physical buffer
typedef enum
AK_LayoutKind {
    AK_LayoutKind_RowMajor,
    AK_LayoutKind_ColumnMajor,
} AK_LayoutKind;

#define AK_LOGICAL_SHAPE_FIELDS \
    size_t rank; \
    size_t dim[AK_MAX_RANK];

typedef struct
AK_LogicalShape {
    AK_LOGICAL_SHAPE_FIELDS
} AK_LogicalShape;

/// Tensor shape descriptor
typedef struct
AK_StridedDesc {
    AK_LOGICAL_SHAPE_FIELDS

    /// The strides of the tensor.
    /// The order of this array must match that of `dim`.
    size_t strides[AK_MAX_RANK];
} AK_StridedDesc;

/// Tracks the coordinates of AK_N_TRACKED_TENSORS tensors.
///
/// All tensors in a single iter must be both broadcasted and
/// have their dims sorted in descending order.
typedef struct
AK_NDIter {
    size_t          coords[AK_MAX_RANK];
    AK_StridedDesc  descs[AK_N_TRACKED_TENSORS];
    size_t          indices[AK_N_TRACKED_TENSORS];
    size_t          n_tracked_dims;
} AK_NDIter;

#define ak_mkshape_count_args(...) (sizeof((uint8_t[]){__VA_ARGS__}))
#define ak_mkshape(...) (AK_StridedDesc){ .rank = ak_mkshape_count_args(__VA_ARGS__), .dim = {__VA_ARGS__} }

/// Increment the coordinate `axis` on `iter` and update offsets.
/// 
/// Does not perform any bounds checking.
bool 
ak_nditer_increment(AK_NDIter *iter, size_t axis);

/// Recomputes the indices in `iter` according to its `coords`.
///
/// If you want to "jump" to a specific coordinate in a tensor, this is the
/// easiest way to do it.
void 
ak_nditer_goto(AK_NDIter *iter, size_t *coords);

/// Infers the broadcasted logical dimensions between `descs`.
AK_StatusKind 
ak_infer_broadcasted_dims(
    AK_LogicalShape *ak_nullable out,
    const AK_LogicalShape **descs,
    size_t n_descs
);

/// Create a shared virtual contraction space between `descs` such that element wise accumulators
/// may function according to broadcast semantics.
AK_StatusKind 
ak_create_broadcast_space(AK_StridedDesc **descs, size_t n_descs);

/// Computes the dimensions of a contraction between `x0` and `x1`
///
/// Does not compute strides.
AK_StatusKind 
ak_infer_contracted_dims(
    AK_LogicalShape *ak_nullable out_y,
    const AK_LogicalShape *x0,
    const AK_LogicalShape *x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);

/// Contracts the dimensions of `y`, inferring the contracted dimensions.
///
/// The contracted dimensions must be aligned at the beginning of `x0`, and `x1` with batch dimensions
/// following.
AK_StatusKind 
ak_create_contraction_space(AK_StridedDesc *y, AK_StridedDesc *x0, AK_StridedDesc *x1, size_t n_batch_axes);

/// Sort axes such that the primary is unit stride first.
///
/// Inputs to this function MUST be broadcasted.
///
/// The first tensor in the set is considered the 
/// "primary."
/// The primary tensor is the one that dictates the optimized plan
/// plan for the other tensors. In that, this is the tensor where it is 
/// guaranteed that the contiguous dimension (the dimension with the unit stride)
/// will be accessed sequentially in memory.
AK_StatusKind 
ak_sort_axes(AK_StridedDesc **descs, size_t n_descs);

/// Coalesce tensor axes to be as flat as possible.
///
/// Inputs to this function MUST be broadcasted AND sorted from least to greatest
/// using `AK_SortAxes`.
AK_StatusKind 
ak_coalesce_axes(AK_StridedDesc **descs, size_t n_descs);

/// Compute the size in bytes of a tensor's data buffer.
size_t 
ak_desc_size_in_bytes(AK_StridedDesc desc);

/// Copy a vector value to the dim `copy_to_dim`.
void 
ak_copy_vector_to_axis(AK_StridedDesc desc, ak_scalar *restrict dest, const ak_scalar *vector, size_t copy_to_axis);

/// Lays out a tensor with pre-populated `dim` and `rank` with the strides to be stored in
/// the order in `layout`. In this layout, the rightmost dimension has the unit stride.
///
/// Rows (the unit stride dimension) are padded to align with `unit_align` if `unit_align` > 1.
///
/// Does not allocate any memory; that can be done with `ak_alloc_tensor`.
///
/// This is the recommended and standard way to initialize a tensor layout.
AK_StatusKind 
ak_desc_compute_strides(AK_StridedDesc *desc, AK_LayoutKind layout, size_t unit_align);

/// Returns true if a tensor is isotropic.
/// 
/// Tensors with a rank of zero, and all scalars are considered isotropic,
/// while all vectors are considered anisotropic.
bool 
ak_desc_is_isotropic(AK_StridedDesc desc);

#endif // AK_CORE_H_
