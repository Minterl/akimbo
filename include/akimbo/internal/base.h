#ifndef AK_BASE_H_
#define AK_BASE_H_

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
#   define ak_nullable _Nullable
#else
#   define ak_nullable 
#endif // defined(__has_attribute) && __has_attribute(nullability)
       
#if defined(__has_attribute) && __has_attribute(unused) 
#   define AK_FEATURE_MAYBE_UNUSED
#   define ak_maybe_unused __attribute__((unused))
#else
#   define ak_maybe_unused
#endif // defined(__has_attribute) && __has_attribute(unused) 

/// Block ordering hints
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

/// Compiler inline hints
#if defined(__has_attribute) && __has_attribute(always_inline)
#   define ak_force_inline __attribute__((always_inline)) inline
#else
#   define ak_force_inline inline
#endif // defined(__has_attribute) && __has_attribute(always_inline)

/// Static assertions
#if defined(_Static_assert)
#   define ak_static_assert(cond) _Static_assert((cond), "")
#elif defined(__COUNTER__) && defined(AK_FEATURE_MAYBE_UNUSED)
#   define ak_static_assert_concat_(a, b) a##b 
#   define ak_static_assert_concat(a, b) ak_static_assert_concat_(a, b)
#   define ak_static_assert(cond) size_t ak_maybe_unused ak_static_assert_concat(ak_static_assert, __COUNTER__) = sizeof(uint8_t[(cond) ? 1 : -1])
#else
#   define ak_static_assert(cond)
#endif // defined(_Static_assert)

/// memory utils

#define ak_align_up(x, align) (((x) + (align) - 1) & ~((align) - 1))

#if defined(__has_builtin) && __has_builtin(__builtin_memcpy)
#   define ak_memcpy(dest, src, size) __builtin_memcpy((dest), (src), (size))
#else
#   define ak_memcpy(dest, src, size) do { \
         for(size_t AK__MACRO_ITER__ = 0; AK__MACRO_ITER__ < (size); AK__MACRO_ITER__++) { \
             ((uint8_t*)(dest))[AK__MACRO_ITER__] = ((uint8_t*)(src))[AK__MACRO_ITER__]; \
         } \
     } while(0) 
#endif // defined(__has_builtin) && __has_builtin(__builtin_memcpy)

#if defined(__has_builtin) && __has_builtin(__builtin_memset)
#   define ak_memzero(ptr, size) __builtin_memset(ptr, 0, size) 
#else
#   define ak_memzero(ptr, size) do { \
         for(size_t AK__MACRO_ITER__ = 0; AK__MACRO_ITER__ < (size); AK__MACRO_ITER__++) { \
             ((uint8_t*)(ptr))[AK__MACRO_ITER__] = 0; \
         } \
     } while(0) 
#endif // defined(__has_builtin) && __has_builtin(__builtin_memset)

#if defined(__has_builtin) && __has_builtin(__builtin_memcmp)
#   define ak_memcmp(a, b, len) __builtin_memcmp((a), (b), (len));
#else
#   define ak_memcmp(a, b, len) ak_memcmp_((uint8_t*)(a), (uint8_t*)(b), (len))
#endif // defined(__has_builtin) && __has_builtin(__builtin_memcmp)

#define ak_deep_copy_struct(T, dest, psrc) do { \
    ak_memcpy((dest), *(psrc), sizeof(T)); \
    *(psrc) = (T*)(dest); \
} while (0)

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// status codes
///
////////////////////////////////////////////////////////////////////////////////
 
#define AK_DEFINE_STATUS_KINDS \
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

typedef enum
AK_StatusKind {
#   define AK_X(x) AK_StatusKind_##x
    AK_DEFINE_STATUS_KINDS
#   undef AK_X
} AK_StatusKind;

// TODO: maybe these should be ak_str8s
ak_maybe_unused static const uint8_t*
AK_STATUS_KIND_CSTRING_TABLE[] = {
#   define AK_X(x) [AK_StatusKind_##x] = (const uint8_t*)#x
    AK_DEFINE_STATUS_KINDS
#   undef AK_X
};

#define ak_status_kind_as_cstring(status) AK_STATUS_KIND_CSTRING_TABLE[(status)]


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// memory allocation utilities
///
////////////////////////////////////////////////////////////////////////////////

typedef uint8_t
AK_AllocatorModeKind;
enum 
AK_AllocatorModeKind {
    AK_AllocatorModeKind_Alloc,
    AK_AllocatorModeKind_Free,
    AK_AllocatorModeKind_GetDefaultPreAllocation,
    AK_AllocatorModeKind_GetFlags,
};

typedef uint32_t 
AK_AllocatorFlags;
enum {
    AK_AllocatorFlag_NoRecycle    = UINT32_C(0x1),
    AK_AllocatorFlag_AssumeZeroed = UINT32_C(0x1) << 1,
};

typedef union
AK_AllocatorModeParams {
    struct {
        size_t size_bytes;
    } alloc;    

    struct {
        uint8_t *ptr;
    } free;

    // void get_default_pre_allocation
    // void get_flags
} AK_AllocatorModeParams;

typedef union
AK_AllocatorModeReturn {
    struct {
        uint8_t *ptr;
    } alloc;

    // void free;

    struct {
        size_t size_bytes;
    } get_default_pre_allocation;

    struct {
        AK_AllocatorFlags flags;
    } get_flags;
} AK_AllocatorModeReturn;

/// To be passed by value, since it is not all that large, and doing so
/// eliminates dependent instruction loads.
///
/// Don't try to get clever and swap this out under the library's feet between calls.
/// That will almost certainly end poorly.
typedef struct
AK_Allocator {
    /// Context passed to each allocator method.
    void *ctx;
    AK_AllocatorModeReturn (*f)(void *ctx, AK_AllocatorModeKind mode, AK_AllocatorModeParams params);
} AK_Allocator;

typedef struct
AK_Slab {
    // these are called up/down rather than next/prev so it's easier to visualize
    // them as a physical stack.
    // not like a "the stack grows down" kind of stack, but a real-world stack of
    // something like paper
    // next and prev don't mean all that much in this case regardless, since the
    // "next" allocation can actually be "downwards" on this stack.

    struct AK_Slab       *up;
    struct AK_Slab       *down;

    size_t                cap;
    uint8_t _Alignas(16)  buf[] ak_check_bounds(cap);
} AK_Slab;

typedef struct 
AK_Arena {
    AK_Allocator host;
    size_t current_offset;
    struct AK_Slab *top_slab;
    struct AK_Slab *top_recycled_slab;
} AK_Arena;

typedef struct
AK_Scope {
    AK_Slab *slab;
    size_t offset;
} AK_Scope;

uint8_t*
ak_alloc_zero(AK_Allocator alloc, size_t size_bytes);

void 
ak_free(AK_Allocator alloc, void *ptr);

/// Allocates `n` blocks of size `sizes[i]` and puts the resulting pointer
/// in `out_ptrs[i]`.
///
/// These blocks are guaranteed to be contiguous in memory and aligned to `align`.
///
/// `out_ptrs[0]` is the pointer the allocated region itself i.e the pointers
/// are allocated in the order of `out_ptrs`.
AK_StatusKind 
ak_alloc_contiguous_blocks(
    AK_Allocator alloc,
    uint8_t **out_ptrs,
    size_t *ak_nullable out_bytes_allocated,
    const size_t *sizes,
    size_t n,
    size_t align
);

#define ak_arena_alloc_array(arena, T, len) (T*)ak_arena_alloc((arena), (len) * sizeof(T), _Alignof(T))
#define ak_arena_alloc_struct(arena, T) (T*)ak_arena_alloc((arena), sizeof(T), _Alignof(T))
#define ak_arena_alloc_famstruct(arena, THeader, data_size) (THeader*)ak_arena_alloc((arena), sizeof(THeader) + (data_size), _Alignof(THeader))

void
ak_arena_init(AK_Arena *arena, AK_Allocator host);
uint8_t*
ak_arena_alloc(AK_Arena *arena, size_t unaligned_size_bytes, size_t align);
AK_Scope
ak_push_scope(AK_Arena *arena);
void
ak_pop_scope(AK_Arena *arena, AK_Scope scope);
void
ak_arena_free_recycled(AK_Arena *arena);
void
ak_arena_recycle_all(AK_Arena *arena);
void
ak_arena_free_all(AK_Arena *arena);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// compile-time hash utilities
///
////////////////////////////////////////////////////////////////////////////////

#define AK_FNV_PRIME 16777619U
#define AK_FNV_OFFSET_BASIS 2166136261U

#define ak_fnv_getc(str, idx, len) ((idx) < (len) ? (str)[idx] : '\0')
#define ak_fnv_step(hash, c) (((hash) ^ (char)(c)) * AK_FNV_PRIME)

#define ak_hash_lit_16(str) ak_hash_16((str), ((size_t)(sizeof(str))))
#define ak_hash_16(str, len) \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step( \
    ak_fnv_step(AK_FNV_OFFSET_BASIS, ak_fnv_getc(str, 0, len)), \
    ak_fnv_getc(str, 1, len)), \
    ak_fnv_getc(str, 2, len)), \
    ak_fnv_getc(str, 3, len)), \
    ak_fnv_getc(str, 4, len)), \
    ak_fnv_getc(str, 5, len)), \
    ak_fnv_getc(str, 6, len)), \
    ak_fnv_getc(str, 7, len)), \
    ak_fnv_getc(str, 8, len)), \
    ak_fnv_getc(str, 9, len)), \
    ak_fnv_getc(str, 10, len)), \
    ak_fnv_getc(str, 11, len)), \
    ak_fnv_getc(str, 12, len)), \
    ak_fnv_getc(str, 13, len)), \
    ak_fnv_getc(str, 14, len)), \
    ak_fnv_getc(str, 15, len))


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// length strings & formatting
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
ak_str8 {
    size_t    len;
    uint8_t  *p ak_check_bounds(len);
} ak_str8;

typedef struct
AK_StringListHead {
    struct AK_StringListHead *next;
    struct AK_StringListHead *prev;

    ak_str8 str;
} AK_StringListHead;

typedef struct
AK_StringList {
    AK_StringListHead *tail;
} AK_StringList;

// We use sizeof(str) - 1 to trim the null terminator
#define ak_str8_lit(str) ((ak_str8){ .len = sizeof(str) - 1, .p = (uint8_t*)(str) })

typedef struct
AK_Writer {
    void *ctx;
    /// Must copy `str`, since it is not guaranteed to live any longer
    /// than the lifetime of the call to `Write`.
    ///
    /// Must return the number of bytes written.
    size_t (*write)(void *ctx, const ak_str8 str);
} AK_Writer;

ak_str8
ak_str8_from_cstr(uint8_t *cstr);

/// Behaves like libc `strcmp`, but returns 1 when `a.len` > `b.len`, and -1 in the opposite case.
int32_t 
ak_strcmp(const ak_str8 a, const ak_str8 b);

/// Copies from `src` to `dest` on a best-effort basis, meaning if `dest.len` < `src.len`, 
/// only `dest.len` bytes will ever be written, at a maximum.
///
/// Returns the number of bytes written.
size_t 
ak_strcpy(ak_str8 dest, const ak_str8 src);

AK_StatusKind
ak_strcat(
    AK_Arena *arena,
    ak_str8 *strings,
    size_t n_strings,
    ak_str8 *out_str
);

void 
ak_copy_to_cstring(uint8_t *dst, const ak_str8 src);

size_t
ak_write(AK_Writer *writer, ak_str8 string);

AK_StatusKind 
ak_printf(AK_Writer *writer, const ak_str8 fmt, ...);

AK_StatusKind 
ak_vprintf(AK_Writer *writer, const ak_str8 fmt, va_list ap);

AK_StatusKind
ak_sprintf(AK_Arena *arena, ak_str8 *out_str, ak_str8 fmt, ...);

size_t 
ak_write_itoa(AK_Writer *writer, int64_t n);

ak_force_inline bool
ak_char_is_whitespace(uint8_t ch);

ak_force_inline bool
ak_char_is_alpha(uint8_t ch);

ak_force_inline bool
ak_char_is_numeric(uint8_t ch);

ak_force_inline bool
ak_char_is_alphanumeric(uint8_t ch);

AK_StatusKind
ak_str8_pascal_to_snake_case(
    ak_str8 str,
    AK_Arena *arena,
    ak_str8 *out_str
);

AK_StatusKind
ak_str8_to_upper(
    ak_str8 str,
    AK_Arena *arena,
    ak_str8 *out_str
);

AK_StatusKind
ak_strlist_append(
    AK_StringList *strlist,
    AK_Arena *arena,
    ak_str8 str
);

void
ak_strlist_write(AK_StringList *strlist, AK_Writer *writer);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// assertions etc.
///
////////////////////////////////////////////////////////////////////////////////

#ifdef AK_DEBUG
#   if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
#       define ak_unreachable_ __builtin_unreachable()
#   else
#       define ak_unreachable_
#   endif // __has_builtin(__builtin_unreachable)
#   define ak_dbgf(fmt, ...) ak_dbgf_(__FILE__, __LINE__, fmt, __VA_ARGS__)
#   define ak_assert(cond) ak_assert_(__FILE__, __LINE__, (cond), #cond)
#   define ak_unreachable(...) do { ak_assert(false); ak_unreachable_; } while (0)
#else
#   define ak_dbgf(fmt, ...)
#   define ak_assert(cond) ((void)(cond))
#   define ak_unreachable(...)
#endif // AK_DEBUG

size_t
ak_dbg_write_stdout_(void *ctx, ak_str8 msg);

AK_Writer 
AK_DBG_WRITER = {
    .write = ak_dbg_write_stdout_,
};

void ak_dbgf_(const char *file, int line, const char* fmt, ...);
void ak_assert_(const char *file, int line, bool cond, const char *cond_str);


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// runtime hash table
///
////////////////////////////////////////////////////////////////////////////////

typedef union 
AK_TableFingerprintBlock {
    uint8_t individual[8];
    uint64_t block;
} AK_TableFingerprintBlock;

/// Implements a swiss table-like slot map.
typedef struct
AK_Table {
    /// Must be a power of two, and will be implicitly rounded to one 
    /// during initialization.
    size_t cap;
    AK_TableFingerprintBlock *fingerprints_as;
    uint64_t *keys ak_check_bounds(cap);
} AK_Table;

typedef struct
AK_TableIter {
    AK_Table *table;
    size_t    next_idx;
} AK_TableIter;

ak_force_inline uint32_t 
ak_mmh(uint8_t *key, size_t len);

AK_StatusKind 
ak_table_init(AK_Table *table, AK_Arena *arena, size_t cap);

/// Ensures a key is present inside the table
/// Cannot realloc memory
AK_StatusKind 
ak_table_ensure_u64(AK_Table *table, uint64_t key, size_t *ak_nullable out_idx, bool *ak_nullable out_was_occupied);

/// Returns the index corresponding to the key in the table, otherwise
/// zero.
///
/// Since zero may be a valid index, use `out_found` to determine whether
/// an entry was found.
size_t 
ak_table_get_u64(AK_Table *table, uint64_t key, bool *ak_nullable out_found);

AK_StatusKind
ak_table_ensure_str8(
    AK_Table *table,
    ak_str8 key,
    size_t *ak_nullable out_idx,
    bool *ak_nullable out_was_occupied
);

size_t
ak_table_get_str8(
    AK_Table *table,
    ak_str8 key,
    bool *ak_nullable out_found
);

void 
ak_table_iter_init(AK_TableIter *iter, AK_Table *table);

/// Returns false once the iterator is finished.
ak_force_inline bool 
ak_table_iter_advance(
    AK_TableIter *iter,
    size_t *ak_nullable out_idx,
    uint64_t *ak_nullable out_cmp_key
);

#endif // AK_BASE_H_
