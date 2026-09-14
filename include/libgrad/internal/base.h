#ifndef LG_BASE_H_
#define LG_BASE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// ergonomics/core macro utilities
///
////////////////////////////////////////////////////////////////////////////////
      
/// Nullability
#if defined(__has_feature) && __has_feature(nullability)
#   define lg_nullable _Nullable
#else
#   define lg_nullable 
#endif // defined(__has_attribute) && __has_attribute(nullability)
       
#if defined(__has_attribute) && __has_attribute(unused) 
#   define LG_FEATURE_MAYBE_UNUSED
#   define lg_maybe_unused __attribute__((unused))
#else
#   define lg_maybe_unused
#endif // defined(__has_attribute) && __has_attribute(unused) 

/// Block ordering hints
#if defined(__has_builtin) && __has_builtin(__builtin_expect)
#   define lg_likely(expr)    __builtin_expect((expr), 1)
#   define lg_unlikely(expr)  __builtin_expect((expr), 0)
#else
#   define lg_likely(expr)    (expr)
#   define lg_unlikely(expr)  (expr)
#endif // defined(__has_builtin) && __has_builtin(__builtin_expect)

#define lg_nil(T) (T){0}

/// Bounds checking
#ifdef __cplusplus
#   define lg_check_bounds(x) /* nothing */
#   define lg_check_bounds_nullable(x) /* nothing */
#else
#   if defined(__clang__) && __has_attribute(counted_by)
#       define lg_check_bounds(x) __attribute__((counted_by(x)))
#       define lg_check_bounds_nullable(x) __attribute__((counted_by_or_null(x)))
#   elif defined(__GNUC__) && (__GNUC__ >= 16) // Pointer support introduced in GCC 16
#       define lg_check_bounds(x) __attribute__((counted_by(x)))
#       define lg_check_bounds_nullable(x) __attribute__((counted_by_or_null(x)))
#   else
#       define lg_check_bounds(x) /* nothing */
#       define lg_check_bounds_nullable(x) /* nothing */
#   endif
#endif

/// Compiler inline hints
#if defined(__has_attribute) && __has_attribute(always_inline)
#   define lg_force_inline __attribute__((always_inline)) inline
#else
#   define lg_force_inline inline
#endif // defined(__has_attribute) && __has_attribute(always_inline)

/// Static assertions
#if defined(_Static_assert)
#   define lg_static_assert(cond) _Static_assert((cond), "")
#elif defined(__COUNTER__) && defined(LG_FEATURE_MAYBE_UNUSED)
#   define lg_static_assert_concat_(a, b) a##b 
#   define lg_static_assert_concat(a, b) lg_static_assert_concat_(a, b)
#   define lg_static_assert(cond) size_t lg_maybe_unused lg_static_assert_concat(lg_static_assert, __COUNTER__) = sizeof(uint8_t[(cond) ? 1 : -1])
#else
#   define lg_static_assert(cond)
#endif // defined(_Static_assert)

/// memory utils

#define lg_align_up(x, align) (((x) + (align) - 1) & ~((align) - 1))

#if defined(__has_builtin) && __has_builtin(__builtin_memcpy)
#   define lg_memcpy(dest, src, size) __builtin_memcpy((dest), (src), (size))
#else
#   define lg_memcpy(dest, src, size) do { \
         for(size_t LG__MACRO_ITER__ = 0; LG__MACRO_ITER__ < (size); LG__MACRO_ITER__++) { \
             ((uint8_t*)(dest))[LG__MACRO_ITER__] = ((uint8_t*)(src))[LG__MACRO_ITER__]; \
         } \
     } while(0) 
#endif // defined(__has_builtin) && __has_builtin(__builtin_memcpy)

#if defined(__has_builtin) && __has_builtin(__builtin_memset)
#   define lg_memzero(ptr, size) __builtin_memset(ptr, 0, size) 
#else
#   define lg_memzero(ptr, size) do { \
         for(size_t LG__MACRO_ITER__ = 0; LG__MACRO_ITER__ < (size); LG__MACRO_ITER__++) { \
             ((uint8_t*)(ptr))[LG__MACRO_ITER__] = 0; \
         } \
     } while(0) 
#endif // defined(__has_builtin) && __has_builtin(__builtin_memset)

#if defined(__has_builtin) && __has_builtin(__builtin_memcmp)
#   define lg_memcmp(a, b, len) __builtin_memcmp((a), (b), (len));
#else
#   define lg_memcmp(a, b, len) lg_memcmp_((uint8_t*)(a), (uint8_t*)(b), (len))
#endif // defined(__has_builtin) && __has_builtin(__builtin_memcmp)

#define lg_deep_copy_struct(T, dest, psrc) do { \
    lg_memcpy((dest), *(psrc), sizeof(T)); \
    *(psrc) = (T*)(dest); \
} while (0)

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// status codes
///
////////////////////////////////////////////////////////////////////////////////
 
#define LG_DEFINE_STATUS_KINDS \
    LG_X(OK), \
    LG_X(InvalidArgument), \
    LG_X(InvalidRank), \
    LG_X(ShapeMismatch), \
    LG_X(StrideMismatch), \
    LG_X(Overflow), \
    LG_X(NotFound), \
    LG_X(Duplicate), \
    LG_X(UnsupportedOpcode), \
    LG_X(OutOfMemory), \
    LG_X(OutOfBounds), \
    LG_X(UnexpectedNaN),

typedef enum
LG_StatusKind {
#   define LG_X(x) LG_StatusKind_##x
    LG_DEFINE_STATUS_KINDS
#   undef LG_X
} LG_StatusKind;

// TODO: maybe these should be lg_str8s
lg_maybe_unused static const uint8_t*
LG_STATUS_KIND_CSTRING_TABLE[] = {
#   define LG_X(x) [LG_StatusKind_##x] = (const uint8_t*)#x
    LG_DEFINE_STATUS_KINDS
#   undef LG_X
};

#define lg_status_kind_as_cstring(status) LG_STATUS_KIND_CSTRING_TABLE[(status)]


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// memory allocation utilities
///
////////////////////////////////////////////////////////////////////////////////

typedef uint32_t 
LG_AllocatorFlags;
enum {
    LG_AllocatorFlag_NoRecycle = UINT32_C(0x1),
    LG_AllocatorFlag_AssumeZeroed = UINT32_C(0x1 << 1),
};

/// The only method that must be defined is `alloc`, the others may
/// legally be NULL. Such is the case when using an arena-style allocator,
/// where free is a no-op and the user deallocates memory outside of this interface.
///
/// Don't try to get clever and swap this out under the library's feet between calls.
/// That will almost certainly end poorly.
typedef struct
LG_Allocator {
    /// Context passed to each allocator method.
    void *ctx;

    /// Allocate `size_bytes` bytes.
    /// Callers will assume that this pointer is aligned.
    void* (*alloc)(void *ctx, size_t size_bytes);
    /// Free the memory at `ptr`.
    /// TODO: find the direct calls to this and replace them with a macro or something.
    void  (*lg_nullable free)(void *ctx, void *ptr);

    size_t default_slab_size_bytes; 

    LG_AllocatorFlags flags;
} LG_Allocator;

typedef struct
LG_Slab {
    struct LG_Slab       *prev;
    struct LG_Slab       *next;
    size_t                cap;
    uint8_t _Alignas(16)  buf[] lg_check_bounds(cap);
} LG_Slab;

typedef struct 
LG_Arena {
    LG_Allocator *host;

    size_t current_offset;
    struct LG_Slab *current_slab;

    struct LG_Slab *recycled_slabs_head;
} LG_Arena;

typedef struct
LG_Scope {
    LG_Slab *slab;
    size_t offset;
} LG_Scope;

uint8_t*
lg_alloc_zero(LG_Allocator *alloc, size_t size_bytes);

void 
lg_free(LG_Allocator *alloc, void *ptr);

/// Allocates `n` blocks of size `sizes[i]` and puts the resulting pointer
/// in `out_ptrs[i]`.
///
/// These blocks are guaranteed to be contiguous in memory and aligned to `align`.
///
/// `out_ptrs[0]` is the pointer the allocated region itself i.e the pointers
/// are allocated in the order of `out_ptrs`.
LG_StatusKind 
lg_alloc_contiguous_blocks(
    LG_Allocator *alloc,
    uint8_t **out_ptrs,
    size_t *lg_nullable out_bytes_allocated,
    const size_t *sizes,
    size_t n,
    size_t align
);

#define lg_arena_alloc_array(arena, T, len) (T*)lg_arena_alloc((arena), (len) * sizeof(T), _Alignof(T))
#define lg_arena_alloc_struct(arena, T) (T*)lg_arena_alloc((arena), sizeof(T), _Alignof(T))
#define lg_arena_alloc_famstruct(arena, THeader, data_size) (THeader*)lg_arena_alloc((arena), sizeof(THeader) + (data_size), _Alignof(THeader))

void
lg_arena_init(LG_Arena *arena, LG_Allocator *host);
uint8_t*
lg_arena_alloc(LG_Arena *arena, size_t unaligned_size_bytes, size_t align);
LG_Scope
lg_push_scope(LG_Arena *arena);
void
lg_pop_scope(LG_Arena *arena, LG_Scope scope);
void
lg_arena_free_recycled(LG_Arena *arena);
void
lg_arena_recycle_all(LG_Arena *arena);
void
lg_arena_free_all(LG_Arena *arena);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// compile-time hash utilities
///
////////////////////////////////////////////////////////////////////////////////

#define LG_FNV_PRIME 16777619U
#define LG_FNV_OFFSET_BASIS 2166136261U

#define lg_fnv_getc(str, idx, len) ((idx) < (len) ? (str)[idx] : '\0')
#define lg_fnv_step(hash, c) (((hash) ^ (char)(c)) * LG_FNV_PRIME)

#define lg_hash_lit_16(str) lg_hash_16((str), ((size_t)(sizeof(str))))
#define lg_hash_16(str, len) \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step( \
    lg_fnv_step(LG_FNV_OFFSET_BASIS, lg_fnv_getc(str, 0, len)), \
    lg_fnv_getc(str, 1, len)), \
    lg_fnv_getc(str, 2, len)), \
    lg_fnv_getc(str, 3, len)), \
    lg_fnv_getc(str, 4, len)), \
    lg_fnv_getc(str, 5, len)), \
    lg_fnv_getc(str, 6, len)), \
    lg_fnv_getc(str, 7, len)), \
    lg_fnv_getc(str, 8, len)), \
    lg_fnv_getc(str, 9, len)), \
    lg_fnv_getc(str, 10, len)), \
    lg_fnv_getc(str, 11, len)), \
    lg_fnv_getc(str, 12, len)), \
    lg_fnv_getc(str, 13, len)), \
    lg_fnv_getc(str, 14, len)), \
    lg_fnv_getc(str, 15, len))


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// length strings & formatting
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
lg_str8 {
    size_t    len;
    uint8_t  *p lg_check_bounds(len);
} lg_str8;

typedef struct
LG_StringListHead {
    struct LG_StringListHead *next;
    struct LG_StringListHead *prev;

    lg_str8 str;
} LG_StringListHead;

typedef struct
LG_StringList {
    LG_StringListHead *tail;
} LG_StringList;

// We use sizeof(str) - 1 to trim the null terminator
#define lg_str8_lit(str) ((lg_str8){ .len = sizeof(str) - 1, .p = (uint8_t*)(str) })

typedef struct
LG_Writer {
    void *ctx;
    /// Must copy `str`, since it is not guaranteed to live any longer
    /// than the lifetime of the call to `Write`.
    ///
    /// Must return the number of bytes written.
    size_t (*write)(void *ctx, const lg_str8 str);
} LG_Writer;


lg_str8
lg_str8_from_cstr(uint8_t *cstr);

/// Behaves like libc `strcmp`, but returns 1 when `a.len` > `b.len`, and -1 in the opposite case.
int32_t 
lg_strcmp(const lg_str8 a, const lg_str8 b);

/// Copies from `src` to `dest` on a best-effor basis, meaning if `dest.len` < `src.len`, 
/// only `dest.len` bytes will ever be written, at a maximum.
///
/// Returns the number of bytes written.
size_t 
lg_strcpy(lg_str8 dest, const lg_str8 src);

LG_StatusKind
lg_strcat(
    LG_Arena *arena,
    lg_str8 *strings,
    size_t n_strings,
    lg_str8 *out_str
);

void 
lg_copy_to_cstring(uint8_t *dst, const lg_str8 src);

size_t
lg_write(LG_Writer *writer, lg_str8 string);

LG_StatusKind 
lg_printf(LG_Writer *writer, const lg_str8 fmt, ...);

LG_StatusKind 
lg_vprintf(LG_Writer *writer, const lg_str8 fmt, va_list ap);

LG_StatusKind
lg_sprintf(LG_Arena *arena, lg_str8 *out_str, lg_str8 fmt, ...);

size_t 
lg_write_itoa(LG_Writer *writer, int64_t n);

lg_force_inline bool
lg_char_is_whitespace(uint8_t ch);

lg_force_inline bool
lg_char_is_alpha(uint8_t ch);

lg_force_inline bool
lg_char_is_numeric(uint8_t ch);

lg_force_inline bool
lg_char_is_alphanumeric(uint8_t ch);

LG_StatusKind
lg_str8_pascal_to_snake_case(
    lg_str8 str,
    LG_Arena *arena,
    lg_str8 *out_str
);

LG_StatusKind
lg_str8_to_upper(
    lg_str8 str,
    LG_Arena *arena,
    lg_str8 *out_str
);

LG_StatusKind
lg_strlist_append(
    LG_StringList *strlist,
    LG_Arena *arena,
    lg_str8 str
);

void
lg_strlist_write(LG_StringList *strlist, LG_Writer *writer);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// assertions etc.
///
////////////////////////////////////////////////////////////////////////////////

#ifdef LG_DEBUG
#   if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
#       define lg_unreachable_ __builtin_unreachable()
#   else
#       define lg_unreachable_
#   endif // __has_builtin(__builtin_unreachable)
#   define lg_dbgf(fmt, ...) lg_dbgf_(__FILE__, __LINE__, fmt, __VA_ARGS__)
#   define lg_assert(cond) lg_assert_(__FILE__, __LINE__, (cond), #cond)
#   define lg_unreachable(...) do { lg_assert(false); lg_unreachable_; } while (0)
#else
#   define lg_dbgf(fmt, ...)
#   define lg_assert(cond) ((void)(cond))
#   define lg_unreachable(...)
#endif // LG_DEBUG

size_t
lg_dbg_write_stdout_(void *ctx, lg_str8 msg);

LG_Writer 
LG_DBG_WRITER = {
    .write = lg_dbg_write_stdout_,
};

void lg_dbgf_(const char *file, int line, const char* fmt, ...);
void lg_assert_(const char *file, int line, bool cond, const char *cond_str);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// runtime hash table
///
////////////////////////////////////////////////////////////////////////////////

typedef union 
LG_TableFingerprintBlock {
    uint8_t individual[8];
    uint64_t block;
} LG_TableFingerprintBlock;

/// Implements a swiss table-like slot map.
typedef struct
LG_Table {
    /// Must be a power of two, and will be implicitly rounded to one 
    /// during initialization.
    size_t cap;
    LG_TableFingerprintBlock *fingerprints_as;
    uint64_t *keys lg_check_bounds(cap);
} LG_Table;

typedef struct
LG_TableIter {
    LG_Table *table;
    size_t    next_idx;
} LG_TableIter;

lg_force_inline uint32_t 
lg_mmh(uint8_t *key, size_t len);

LG_StatusKind 
lg_table_init(LG_Table *table, LG_Arena *arena, size_t cap);

/// Ensures a key is present inside the table
/// Cannot realloc memory
LG_StatusKind 
lg_table_ensure_u64(LG_Table *table, uint64_t key, size_t *lg_nullable out_idx, bool *lg_nullable out_was_occupied);

/// Returns the index corresponding to the key in the table, otherwise
/// zero.
///
/// Since zero may be a valid index, use `out_found` to determine whether
/// an entry was found.
size_t 
lg_table_get_u64(LG_Table *table, uint64_t key, bool *lg_nullable out_found);

LG_StatusKind
lg_table_ensure_str8(
    LG_Table *table,
    lg_str8 key,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied
);

size_t
lg_table_get_str8(
    LG_Table *table,
    lg_str8 key,
    bool *lg_nullable out_found
);

void 
lg_table_iter_init(LG_TableIter *iter, LG_Table *table);

/// Returns false once the iterator is finished.
lg_force_inline bool 
lg_table_iter_advance(
    LG_TableIter *iter,
    size_t *lg_nullable out_idx,
    uint64_t *lg_nullable out_cmp_key
);

#endif // LG_BASE_H_
