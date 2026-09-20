#include <libgrad/internal/base.h>

int32_t
lg_memcmp_(uint8_t *a, uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return (int32_t)a[i] - (int32_t)b[i];
        }
    }
    return 0;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// string & stream implementation stuff
///
////////////////////////////////////////////////////////////////////////////////

size_t
lg_write(LG_Writer *writer, lg_str8 string) {
    if (writer != NULL && writer->write != NULL) {
        return writer->write(writer->ctx, string);
    } else {
        return 0;
    }
}

#define LG_FMT_SPEC_TABLE \
    LG_X(i64) \
    LG_X(str) \
    LG_X(cstr) \
    LG_X(lshape_ptr) \
    LG_X(status)

#define LG_X(fmtspec) \
    size_t \
    lg_vfmt_##fmtspec(va_list ap, LG_Writer *writer);
LG_FMT_SPEC_TABLE
#undef LG_X

static const struct {
    uint32_t hash;
    size_t (*fn)(va_list ap, LG_Writer *writer);
} LG_FMT_FN_LUT[] = {
#   define LG_X(fmtspec) {lg_hash_lit_16(#fmtspec), lg_vfmt_##fmtspec},
    LG_FMT_SPEC_TABLE
#   undef LG_X
};
#define LG_FMT_FN_LUT_LEN (sizeof(LG_FMT_FN_LUT) / sizeof(LG_FMT_FN_LUT[0]))

size_t 
lg_vfmt_i64(va_list ap, LG_Writer *writer) {
    int64_t arg = va_arg(ap, int64_t);
    return lg_write_itoa(writer, arg); 
}
size_t 
lg_vfmt_str(va_list ap, LG_Writer *writer) {
    lg_str8 s = va_arg(ap, lg_str8);
    return lg_write(writer, s);
}
size_t 
lg_vfmt_cstr(va_list ap, LG_Writer *writer) {
    uint8_t *s = va_arg(ap, uint8_t*);
    lg_str8 str8 = lg_str8_from_cstr(s);
    return lg_write(writer, str8);
}
size_t 
lg_vfmt_status(va_list ap, LG_Writer *writer) {
    LG_StatusKind status = va_arg(ap, LG_StatusKind);
    uint8_t *s = (uint8_t*)lg_status_kind_as_cstring(status);
    lg_str8 str8 = lg_str8_from_cstr(s);
    return lg_write(writer, str8);
}

lg_str8
lg_str8_from_cstr(uint8_t *cstr) {
    size_t len = 0;
    while (cstr[len] != '\0') {len++;};
    return (lg_str8){ .len = len, .p = cstr };
}

int32_t 
lg_strcmp(const lg_str8 a, const lg_str8 b) {
    if (a.p == b.p && a.len == b.len) {
        return 0;
    }

    if (a.len > b.len) {
        return a.p[b.len];
    } else if (b.len > a.len) {
        return b.p[a.len];
    }

    const size_t len = a.len > b.len ? b.len : a.len;
    return lg_memcmp(a.p, b.p, len);
}

size_t 
lg_strcpy(lg_str8 dest, const lg_str8 src) {
    size_t i = 0;
    for (; i < dest.len && i < src.len; i++) {
        dest.p[i] = src.p[i];
    }
    return i;
}

LG_StatusKind
lg_strcat(
    LG_Arena *arena,
    lg_str8 *strings,
    size_t n_strings,
    lg_str8 *out_str
) {
    lg_assert(out_str != NULL);

    size_t new_len = 0;
    for (size_t i = 0; i < n_strings; i++) {
        new_len += strings[i].len;
    }

    uint8_t *new_p = lg_arena_alloc_array(arena, uint8_t, new_len);
    if (new_p == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    lg_str8 new_str = { .len = new_len, .p = new_p };

    size_t offset = 0;
    for (size_t i = 0; i < n_strings; i++) {
        lg_memcpy(new_p + offset, strings[i].p, strings[i].len);
        offset += strings[i].len;
        lg_assert(offset <= new_len);
    }

    *out_str = new_str;
    return LG_StatusKind_OK;
}

void 
lg_copy_to_cstring(uint8_t *dest, const lg_str8 src) {
    lg_assert(dest != NULL);
    lg_assert(src.p != NULL);

    size_t i = 0;
    for (; i < src.len; i++) {
        dest[i] = src.p[i];
    }
    dest[i] = '\0';
}

size_t
lg_write_itoa(LG_Writer *writer, int64_t n) {
    lg_static_assert(INT64_MAX == 9223372036854775807);
    //            ... which is -- 1234567890123456789 -- 19 digits long
    // +1 for the sign character.
    // `lg_str8` does not need a null terminator
    uint8_t buf[20] = {0};
    size_t len = 0;

    uint64_t abs_n = (n < 0) ? (uint64_t)-(n + 1) + 1 : (uint64_t)n;
    do {
        buf[len] = '0' + (abs_n % 10);
        len++;
        abs_n /= 10;
    } while (abs_n > 0);

    if (n < 0) {
        buf[len] = '-';
        len++;
    }

    for (size_t i = 0; i < len / 2; i++) {
        const size_t i_left = i;
        const size_t i_right = len - 1 - i;
        const uint8_t temp = buf[i_left];
        buf[i_left] = buf[i_right];
        buf[i_right] = temp;
    }

    return lg_write(writer, ((lg_str8){ .len = len, .p = buf }));
}

LG_StatusKind 
lg_printf(LG_Writer *writer, const lg_str8 fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LG_StatusKind status = lg_vprintf(writer, fmt, ap);
    va_end(ap);
    return status;
}

LG_StatusKind 
lg_vprintf(LG_Writer *writer, const lg_str8 fmt, va_list ap) {
    LG_StatusKind status = LG_StatusKind_OK;

    for (size_t i = 0; i < fmt.len; i++) {
        if (
            fmt.p[i] != '%' ||
            (i + 1) >= fmt.len ||
            fmt.p[i + 1] != '{'
        ) {
            lg_write(writer, ((lg_str8){ .len = 1, .p = fmt.p + i }));
            continue;
        }


        ////////////////////////////////////////////////// 
        // ~~ Parse the format specifier ~~

        lg_str8 fmtspec;
        {
            lg_assert(fmt.p[i] == '%');
            lg_assert(fmt.p[i + 1] == '{');
            lg_assert(fmt.len > i + 2);

            size_t fmtspec_begin = i + 2;
            size_t fmtspec_end = fmtspec_begin;
            {
                while (fmt.p[fmtspec_end] != '}') {
                    // unterminated format specifier
                    if (fmtspec_end >= fmt.len - 1) {
                        status = LG_StatusKind_InvalidArgument;
                        goto out;
                    }
                    fmtspec_end++;
                }      
                i = fmtspec_end; // i will be incremeted at the bottom of the loop
            }

            fmtspec = (lg_str8){
                .len = fmtspec_end - fmtspec_begin,
                .p = fmt.p + fmtspec_begin,
            };
            if (fmtspec.len == 0) {
                status = LG_StatusKind_InvalidArgument;
                goto out;
            }

            lg_assert((fmtspec.p + fmtspec.len) < (fmt.p + fmt.len));
        }


        ////////////////////////////////////////////////// 
        // ~~ Format specifier LUT lookup ~~
        {
            uint32_t hash = lg_hash_16(fmtspec.p, (fmtspec.len < 16 ? fmtspec.len : 16));
            bool found = false;
            for (size_t i = 0; i < LG_FMT_FN_LUT_LEN; i++) {
                if (LG_FMT_FN_LUT[i].hash == hash) {
                    LG_FMT_FN_LUT[i].fn(ap, writer);
                    found = true;
                    break;
                }
            }
            if (!found) {
                status = LG_StatusKind_InvalidArgument;
                goto out;
            }
        }

        lg_assert(i < fmt.len);
    }

out:
    if (status != LG_StatusKind_OK) {
        lg_write(writer, lg_str8_lit("(error)"));
    }
    return LG_StatusKind_OK;
}

typedef struct
LG_SPrintfContext {
    lg_str8 fmt;
    uint32_t count_len;
    uint32_t out_cur_offset;
    uint8_t *out;
} LG_SPrintfContext;

size_t
lg_sprintf_count(void *ctx_, lg_str8 txt) {
    LG_SPrintfContext *ctx = ctx_;
    ctx->count_len += txt.len;
    return txt.len;
}

size_t
lg_sprintf_write(void *ctx_, lg_str8 txt) {
    LG_SPrintfContext *ctx = ctx_;
    lg_memcpy(ctx->out + ctx->out_cur_offset, txt.p, txt.len);
    ctx->out_cur_offset += txt.len;
    return txt.len;
}

LG_StatusKind
lg_sprintf(LG_Arena *arena, lg_str8 *out_str, lg_str8 fmt, ...) {
    lg_assert(out_str != NULL);

    LG_SPrintfContext closure = {
        .fmt = fmt,
    };

    LG_Writer counting_writer = (LG_Writer){
        .ctx = &closure,
        .write = lg_sprintf_count,
    };

    {
        va_list ap;
        va_start(ap, fmt);
        LG_StatusKind status = lg_vprintf(&counting_writer, fmt, ap);
        va_end(ap);
        lg_assert(status == LG_StatusKind_OK); // this writer cannot fail
    }

    const size_t len = closure.count_len;
    uint8_t *p = lg_arena_alloc_array(arena, uint8_t, len);
    if (p == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    closure.out = p;

    LG_Writer writing_writer = (LG_Writer){
        .ctx = &closure,
        .write = lg_sprintf_write,
    };
    
    {
        va_list ap;
        va_start(ap, fmt);
        LG_StatusKind status = lg_vprintf(&writing_writer, fmt, ap);
        va_end(ap);
        lg_assert(status == LG_StatusKind_OK); // this writer also cannot fail
    }

    *out_str = (lg_str8){ .len = len, .p = p };

    return LG_StatusKind_OK;
}

lg_force_inline bool
lg_char_is_whitespace(uint8_t ch) {
    return (
        ch == ' '  ||
        ch == '\n' ||
        ch == '\r' ||
        ch == '\t' ||
        ch == '\f' ||
        ch == '\v'
    );
}

lg_force_inline bool
lg_char_is_alpha(uint8_t ch) {
    return (
        ('a' <= ch && ch <= 'z') ||
        ('A' <= ch && ch <= 'Z')
    );
}

lg_force_inline bool
lg_char_is_capital_letter(uint8_t ch) {
    return 'A' <= ch && ch <= 'Z';
}

lg_force_inline bool
lg_char_is_lower_case_letter(uint8_t ch) {
    return 'a' <= ch && ch <= 'z';
}

lg_force_inline bool
lg_char_is_numeric(uint8_t ch) {
    return '0' <= ch && ch <= '9';
}

lg_force_inline bool
lg_char_is_alphanumeric(uint8_t ch) {
    return lg_char_is_alpha(ch) || lg_char_is_numeric(ch);
}

LG_StatusKind
lg_str8_pascal_to_snake_case(
    lg_str8 str,
    LG_Arena *arena,
    lg_str8 *out_str
) {
    lg_assert(out_str != NULL);

    lg_static_assert((int32_t)'a' - 'A' > 0);
    const size_t difference = 'a' - 'A';

    size_t new_len = str.len;

    for (size_t i = 0; i < str.len; i++) {
        if (lg_char_is_capital_letter(str.p[i]) && i != 0 && !lg_char_is_capital_letter(str.p[i - 1])) {
            new_len++;
        }
    }

    uint8_t *new_p = lg_arena_alloc_array(arena, uint8_t, new_len);
    if (new_p == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    for (
        size_t i_old = 0, i_new = 0;
        i_old < str.len;
        i_old++, i_new++
    ) {
        lg_assert(i_new < new_len);

        if (lg_char_is_capital_letter(str.p[i_old])) {
            if (i_old != 0 && !lg_char_is_capital_letter(str.p[i_old - 1])) {
                new_p[i_new] = '_';
                i_new++;
            }
            new_p[i_new] = str.p[i_old] + difference;
        } else {
            new_p[i_new] = str.p[i_old];
        }
    }

    *out_str = (lg_str8){ .len = new_len, .p = new_p };

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_str8_to_upper(
    lg_str8 str,
    LG_Arena *arena,
    lg_str8 *out_str
) {
    lg_assert(out_str != NULL);

    lg_static_assert((int32_t)'a' - 'A' > 0);
    const size_t difference = 'a' - 'A';

    uint8_t *new_p = lg_arena_alloc_array(arena, uint8_t, str.len);
    if (new_p == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    for (size_t i = 0; i < str.len; i++) {
        if (lg_char_is_lower_case_letter(str.p[i])) {
            new_p[i] = str.p[i] - difference;
        } else {
            new_p[i] = str.p[i];
        }
    }

    *out_str = (lg_str8){ .len = str.len, .p = new_p };

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_str8_to_lower(
    lg_str8 str,
    LG_Arena *arena,
    lg_str8 *out_str
) {
    lg_assert(out_str != NULL);

    lg_static_assert((int32_t)'a' - 'A' > 0);
    const size_t difference = 'a' - 'A';

    uint8_t *new_p = lg_arena_alloc_array(arena, uint8_t, str.len);
    if (new_p == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    for (size_t i = 0; i < str.len; i++) {
        if (lg_char_is_capital_letter(str.p[i])) {
            new_p[i] = str.p[i] + difference;
        } else {
            new_p[i] = str.p[i];
        }
    }

    *out_str = (lg_str8){ .len = str.len, .p = new_p };

    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_strlist_append(
    LG_StringList *strlist,
    LG_Arena *arena,
    lg_str8 str
) {
    LG_StringListHead *head = lg_arena_alloc_famstruct(arena, LG_StringListHead, str.len);
    if (head == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    head->str = str;
    if (strlist->tail != NULL) {
        lg_assert(strlist->tail->next == NULL);
        strlist->tail->next = head;
        head->prev = strlist->tail;
    }
    strlist->tail = head;

    return LG_StatusKind_OK;
}

void
lg_strlist_write(LG_StringList *strlist, LG_Writer *writer) {
    lg_assert(strlist != NULL);

    if (strlist->tail == NULL) {
        return;
    }

    LG_StringListHead *iter_head = strlist->tail;
    while (true) {
        if (iter_head->prev == NULL) {
            break;
        }
        iter_head = iter_head->prev;
    }

    while (iter_head != NULL) {
        lg_write(writer, iter_head->str);
        iter_head = iter_head->next;
    }
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// guarded debug utilities
///
////////////////////////////////////////////////////////////////////////////////

// Use a guard here b/c this block expects libc,
// which may not be available.
#ifdef LG_DEBUG

#include <stdio.h>
#include <stdlib.h>

size_t
lg_dbg_write_stdout_(void *ctx, lg_str8 msg) {
    (void)ctx;
    return printf("%.*s", (int32_t)msg.len, msg.p);
}

void 
lg_dbgf_(const char *file, int line, const char* fmt, ...) {
    fprintf(stderr, "\033[32m[DEBUG]\033[0m (%s:%d) -- ", file, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void 
lg_assert_(const char *file, int line, bool cond, const char *cond_str) {
    if (!cond) {
        fprintf(stderr, "\x1b[31m[ASSERTION FAILED]\x1b[0m (%s) at %s:%d\n", cond_str, file, line);
        abort();
    }
}

#else 

size_t
lg_dbg_write_stdout_(void *ctx, lg_str8 msg) {
    (void)ctx;
    (void)msg;
    return msg.len;
}

#endif // LG_DEBUG


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// allocator implemementations
///
////////////////////////////////////////////////////////////////////////////////

lg_force_inline uint8_t*
lg_alloc_nozero(LG_Allocator alloc, size_t size_bytes) {
    return alloc.f(alloc.ctx, LG_AllocatorModeKind_Alloc, (LG_AllocatorModeParams){
        .alloc = { .size_bytes = size_bytes },
    }).alloc.ptr;
}

lg_force_inline void
lg_free(LG_Allocator alloc, void *ptr) {
    alloc.f(alloc.ctx, LG_AllocatorModeKind_Free, (LG_AllocatorModeParams){
        .free = { .ptr = ptr },
    });
}


lg_force_inline LG_AllocatorFlags
lg_alloc_get_flags(LG_Allocator alloc) {
    return alloc.f(alloc.ctx, LG_AllocatorModeKind_GetFlags, (LG_AllocatorModeParams){0}).get_flags.flags;
}

lg_force_inline size_t
lg_alloc_get_default_pre_allocation(LG_Allocator alloc) {
    return alloc.f(
        alloc.ctx,
        LG_AllocatorModeKind_GetDefaultPreAllocation,
        (LG_AllocatorModeParams){0}
    ).get_flags.flags;
}

uint8_t*
lg_alloc_zero(LG_Allocator alloc, size_t size_bytes) {
    uint8_t *ptr = lg_alloc_nozero(alloc, size_bytes);
    if (ptr == NULL) {
        return ptr;
    }

    LG_AllocatorFlags flags = lg_alloc_get_flags(alloc);
    if (!(flags & LG_AllocatorFlag_AssumeZeroed)) {
        lg_memzero(ptr, size_bytes);
    }

    return ptr;
}

LG_StatusKind 
lg_alloc_contiguous_blocks(
    LG_Allocator alloc,
    uint8_t **out_ptrs,
    size_t *lg_nullable out_bytes_allocated,
    const size_t *sizes,
    size_t n,
    size_t align
) {
    size_t size = 0;
    for (size_t i = 0; i < n; i++) {
        size += lg_align_up(sizes[i], align);
    }

    uint8_t *ptr = lg_alloc_zero(alloc, size);
    if (ptr == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    if (out_bytes_allocated != NULL) {
        *out_bytes_allocated = size;
    }

    size_t current_offset = 0;
    for (size_t i = 0; i < n; i++) {
        out_ptrs[i] = ptr + current_offset;
        current_offset += lg_align_up(sizes[i], align);
    }

    return LG_StatusKind_OK;
}


lg_force_inline void
lg_slab_unlink(LG_Slab *slab) {
    lg_assert(slab != NULL);
    lg_assert(slab != slab->up);

    if (slab->up != NULL) {
        lg_assert(slab->up->down == slab);
        slab->up->down = slab->down;
    }            
    if (slab->down != NULL) {
        lg_assert(slab->down->up == slab);
        slab->down->up = slab->up;
    }

    slab->up = NULL;
    slab->down = NULL;
}

lg_force_inline void
lg_slab_free_downward_from(LG_Slab *slab, LG_Allocator alloc) {
    LG_Slab *next = slab;
    while (next != NULL) {
        LG_Slab *current = next;
        next = current->down;
        lg_assert(current != next);
        lg_free(alloc, current);
    }
}

void
lg_arena_init(LG_Arena *arena, LG_Allocator host) {
    lg_memzero(arena, sizeof(LG_Arena));
    arena->host = host;
}

uint8_t*
lg_arena_alloc(LG_Arena *arena, size_t unaligned_size_bytes, size_t align) {
    lg_assert(arena->top_slab == NULL || arena->top_slab != arena->top_recycled_slab);
    lg_assert(arena->top_slab == NULL || arena->top_slab->down != arena->top_slab);

    const size_t size_bytes = lg_align_up(unaligned_size_bytes, align);


    ////////////////////////////////////////
    // ~~ Plan A: use the current slab ~~

    if (lg_likely(
        arena->top_slab != NULL &&
        arena->current_offset + size_bytes <= arena->top_slab->cap
    )) {
        lg_memzero(arena->top_slab->buf + arena->current_offset, size_bytes);

        const size_t prev_offset = arena->current_offset;
        arena->current_offset += size_bytes;

        return arena->top_slab->buf + prev_offset;
    }


    //////////////////////////////////////////////////
    // ~~ Plan B: first-fit an existing free slab ~~

    if (lg_alloc_get_flags(arena->host) & LG_AllocatorFlag_NoRecycle) {
        goto plan_c;
    }

    LG_Slab *to_reuse = arena->top_recycled_slab;
    while (to_reuse != NULL) {
        if (to_reuse->cap >= size_bytes) {
            if (arena->top_recycled_slab == to_reuse) {
                lg_assert(arena->top_recycled_slab->up == NULL);
                arena->top_recycled_slab = arena->top_recycled_slab->down;
            }
            lg_slab_unlink(to_reuse);

            lg_assert(to_reuse->down == NULL);
            lg_assert(to_reuse->up == NULL);

            if (arena->top_slab != NULL) {
                arena->top_slab->up = to_reuse;
            }
            to_reuse->down = arena->top_slab;
            arena->top_slab = to_reuse;
            arena->current_offset = size_bytes;

            lg_memzero(to_reuse->buf, size_bytes);

            return to_reuse->buf;
        }

        to_reuse = to_reuse->down;
    }


    ////////////////////////////////////////
    // ~~ Plan C: allocate a new slab ~~

plan_c:;

    const size_t default_slab_size = lg_alloc_get_default_pre_allocation(arena->host);
    const size_t buf_size = size_bytes > default_slab_size ?
        size_bytes :
        default_slab_size;
    const size_t total_size = sizeof(LG_Slab) + buf_size;

    LG_Slab *next_on_top = (LG_Slab*)lg_alloc_nozero(arena->host, total_size);
    if (next_on_top == NULL) {
        return NULL;
    }

    lg_memzero(next_on_top, sizeof(LG_Slab));
    next_on_top->cap = buf_size;

    if (arena->top_slab != NULL) {
        arena->top_slab->up = next_on_top;
    }
    next_on_top->down = arena->top_slab;

    arena->top_slab = next_on_top;
    arena->current_offset = size_bytes;

    lg_memzero(next_on_top->buf, size_bytes);

    return next_on_top->buf;
}

LG_Scope
lg_push_scope(LG_Arena *arena) {
    return (LG_Scope){
        .offset = arena->current_offset,
        .slab = arena->top_slab,
    };
}

void
lg_pop_scope(LG_Arena *arena, LG_Scope scope) {
    if (lg_likely(arena->top_slab == scope.slab && scope.offset > 0)) {
        arena->current_offset = scope.offset;
        return;
    }

    LG_AllocatorFlags flags = lg_alloc_get_flags(arena->host);
    if (flags & LG_AllocatorFlag_NoRecycle) {
        lg_unreachable("TODO");
        return;
    }

    if (lg_unlikely(scope.slab == NULL)) {
        lg_arena_recycle_all(arena);
        arena->current_offset = 0;
        return;
    }

    LG_Slab *to_recycle = scope.slab->up;
    arena->current_offset = scope.offset;

    if (to_recycle != NULL) {
        LG_Slab *prev_top_slab = arena->top_slab;
        arena->top_slab = to_recycle->down;

        // first, we break the chain below the final slab we want to recycle.
        // this forms a chain like this:
        //
        //                  (nil)
        //                    ^
        //                    |
        //             [prev_top_slab]
        //                    ^
        //                    |
        //                    v
        //                ..........
        //                    ^
        //                    |
        //                    v
        //               [to_recycle]
        //                    |
        //                    v
        //                  (nil)
        //
        //            [the new top slab]
        //
        // then, we recycle from the top slab all the way down to that point.
        // this one is confusing, so close your eyes and think about it.

        // break the chain
        if (to_recycle->down != NULL) { 
            lg_assert(to_recycle->down->up == to_recycle);
            to_recycle->down->up = NULL;
            to_recycle->down = NULL;
        }
        lg_assert(prev_top_slab->up == NULL);

        // migrate the list
        if (arena->top_recycled_slab != NULL) {
            lg_assert((arena->top_recycled_slab)->up == NULL);
            (arena->top_recycled_slab)->up = to_recycle;
        }
        to_recycle->down = arena->top_recycled_slab;
        arena->top_recycled_slab = prev_top_slab;
    }
}

void
lg_arena_free_recycled(LG_Arena *arena) {
    LG_AllocatorFlags flags = lg_alloc_get_flags(arena->host);
    if (flags & LG_AllocatorFlag_NoRecycle) {
        lg_assert(arena->top_recycled_slab == NULL);
        return;
    }
    if (arena->top_recycled_slab == NULL) {
        return;
    }

    lg_slab_free_downward_from(arena->top_recycled_slab, arena->host);
    arena->top_recycled_slab = NULL;
}

void
lg_arena_recycle_all(LG_Arena *arena) {
    LG_AllocatorFlags flags = lg_alloc_get_flags(arena->host);
    if (
        flags & LG_AllocatorFlag_NoRecycle ||
        arena->top_slab == NULL
    ) {
        return;
    }

    LG_Slab *bottom_active = arena->top_slab;
    while (bottom_active->down != NULL) {
        lg_assert(bottom_active->down != bottom_active);
        bottom_active = bottom_active->down;
    }

    bottom_active->down = arena->top_recycled_slab;
    if (arena->top_recycled_slab != NULL) {
        lg_assert(arena->top_recycled_slab->up == NULL);
        arena->top_recycled_slab->up = bottom_active;
    }
    arena->top_recycled_slab = arena->top_slab;
    arena->top_slab = NULL;
}

void
lg_arena_free_all(LG_Arena *arena) {
    lg_slab_free_downward_from(arena->top_slab, arena->host);
    lg_arena_free_recycled(arena);
    arena->current_offset = 0;
    arena->top_slab = NULL;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// runtime hash table implementation
///
////////////////////////////////////////////////////////////////////////////////

enum {
    LG_TableSentinel_Empty = UINT8_C(0x0),
};

#define LG_MMH_C1 0xcc9e2d51u
#define LG_MMH_C2 0x1b873593u
#define LG_MMH_C3 0x85ebca6bu
#define LG_MMH_C4 0xc2b2ae35u
#define LG_MMH_R1 15u
#define LG_MMH_R2 13u
#define LG_MMH_M  5u
#define LG_MMH_N  0xe6546b64u
#define LG_MMH_S  0u

#define lg_mmh_rol(x, width, bits) (((x) << (bits)) | ((x) >> ((width) - (bits))))
#define lg_u64_has_zero_byte(x) ((((x) - UINT64_C(0x0101010101010101)) & ~(x) & UINT64_C(0x8080808080808080)) != 0)

lg_force_inline uint32_t 
lg_mmh(uint8_t *key, size_t len) {
    uint32_t hash = LG_MMH_S;

    for (size_t i = 0; i < len; i += 4) {
        const size_t remaining_len_clamped = (len - i) > 4 ? 4 : (len - i);
        uint32_t chunk = 0;
        lg_memcpy(&chunk, key + i, remaining_len_clamped);

        chunk = lg_mmh_rol(chunk * LG_MMH_C1, 32, LG_MMH_R1);
        chunk *= LG_MMH_C2;
        hash = LG_MMH_S ^ hash;
        hash = lg_mmh_rol(hash, 32, LG_MMH_R2) * LG_MMH_M + LG_MMH_N;
    }

    hash = hash ^ (uint32_t)len;
    hash = hash ^ 4;
    hash = hash ^ (hash >> 16);
    hash = hash * LG_MMH_C3;
    hash = hash ^ (hash >> 13);
    hash = hash * LG_MMH_C4;
    hash = hash ^ (hash >> 16);

    return hash;
}

lg_force_inline size_t 
lg_next_pow2(size_t x) {
    if (x == 0) {
        return 1;
    }
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

LG_StatusKind 
lg_table_init(LG_Table *table, LG_Arena *arena, size_t cap) {
    cap = cap < 8 ? 8 : lg_next_pow2(cap);
    const size_t align = 16;

    const size_t sz_keys = cap * sizeof(uint64_t);
    const size_t sz_fingerprints = (cap / 8) * sizeof(uint64_t);

    uint64_t *keys = (uint64_t*)lg_arena_alloc(arena, sz_keys, align);
    LG_TableFingerprintBlock *fingerprints = (LG_TableFingerprintBlock*)lg_arena_alloc(arena, sz_fingerprints, align);
    if (keys == NULL || fingerprints == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    table->cap = cap;
    table->keys = keys;
    table->fingerprints_as = fingerprints;

    return LG_StatusKind_OK;
}

lg_force_inline void 
lg_table_make_hash(
    uint8_t *key,
    size_t key_len,
    uint64_t *out_hash,
    uint8_t *out_fingerprint
) {
    const uint64_t full_hash = lg_mmh(key, key_len);
    const size_t hash = full_hash & ~UINT8_C(0xF);
    const uint8_t fingerprint = (full_hash & UINT8_C(0xF)) | UINT8_C(0x1);

    lg_assert(fingerprint != LG_TableSentinel_Empty);

    if (out_fingerprint != NULL) {
        *out_fingerprint = fingerprint;
    }
    if (out_hash != NULL) {
        *out_hash = hash;
    }
}

lg_force_inline LG_StatusKind 
lg_table_probe(
    LG_Table *table,
    uint64_t key,
    uint64_t hash,
    uint8_t fingerprint,
    bool search_for_empty,
    size_t *lg_nullable out_last_idx, 
    bool *lg_nullable out_found
) {
    // TODO: split this into two functions this is one is doing way too much

    const size_t fingerprint_blocks_cap = table->cap / 8;
    const size_t starting_fingerprint_block_idx = (hash % table->cap) / 8;

    const uint64_t fingerprint_broadcasted =
        (uint64_t)fingerprint       |
        (uint64_t)fingerprint << 8  |
        (uint64_t)fingerprint << 16 |
        (uint64_t)fingerprint << 24 |
        (uint64_t)fingerprint << 32 |
        (uint64_t)fingerprint << 40 |
        (uint64_t)fingerprint << 48 |
        (uint64_t)fingerprint << 56;

    size_t ret_last_idx = 0;
    bool ret_found = false;
    for (
        size_t i = starting_fingerprint_block_idx, n_visited = 0;
        n_visited < fingerprint_blocks_cap;
        i = (i + 1) & (fingerprint_blocks_cap - 1), n_visited++
    ) {
        const uint64_t block = table->fingerprints_as[i].block;

        const bool has_match = lg_u64_has_zero_byte(block ^ fingerprint_broadcasted);
        if (has_match) {
            for (size_t j = 0; j < 8; j++) {
                if (
                    table->fingerprints_as[i].individual[j] == LG_TableSentinel_Empty &&
                    search_for_empty
                ) {
                    ret_last_idx = i * 8 + j;
                    goto out_success;
                }
                if (table->fingerprints_as[i].individual[j] != fingerprint) {
                    continue;
                }
                if (table->keys[i * 8 + j] == key) {
                    ret_last_idx = i * 8 + j;
                    ret_found = true;
                    goto out_success;
                }
            }
        }

        if (search_for_empty) {
            const bool has_empty_sentinel = lg_u64_has_zero_byte(block);
            if (has_empty_sentinel) {
                for (size_t j = 0; j < 8; j++) {
                    if (table->fingerprints_as[i].individual[j] == LG_TableSentinel_Empty) {
                        ret_last_idx = i * 8 + j;
                        goto out_success;
                    }
                }
                lg_unreachable();
            }
        }
    }

    // If we got here, we have not
    // a) found a match, nor
    // b) found a single sentinel.
    // Thus, we must be out of capacity.
    if (out_last_idx != NULL) {
        *out_last_idx = 0;
    }
    if (out_found != NULL) {
        *out_found = false;
    }

    if (search_for_empty) {
        return LG_StatusKind_OutOfMemory;
    } else {
        return LG_StatusKind_NotFound;
    }

out_success:
    if (out_last_idx != NULL) {
        *out_last_idx = ret_last_idx;
    }
    if (out_found != NULL) {
        *out_found = ret_found;
    }
    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_table_ensure_g(
    LG_Table *table,
    uint64_t cmp_key,
    uint64_t hash,
    uint8_t fingerprint,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied
) {
    lg_assert(lg_next_pow2(table->cap) == table->cap && table->cap >= 8);

    LG_StatusKind status = LG_StatusKind_OK;

    bool found;
    size_t last_idx;
    status = lg_table_probe(table, cmp_key, hash, fingerprint, true, &last_idx, &found);
    if (status != LG_StatusKind_OK) {
        found = false;
        last_idx = 0;
        goto out;
    }

    if (!found) {
        const size_t outer_idx = last_idx / 8;
        const size_t inner_idx = last_idx % 8;
        table->fingerprints_as[outer_idx].individual[inner_idx] = fingerprint;
        table->keys[last_idx] = cmp_key;
    }

out:
    if (out_idx != NULL) {
        *out_idx = last_idx;
    }
    if (out_was_occupied != NULL) {
        *out_was_occupied = found;
    }
    return status;
}

size_t 
lg_table_get_g(
    LG_Table *table,
    uint64_t cmp_key,
    uint64_t hash,
    uint8_t fingerprint,
    bool *lg_nullable out_found
) {
    lg_assert(lg_next_pow2(table->cap) == table->cap && table->cap >= 8);

    bool found;
    size_t last_idx;
    LG_StatusKind status = lg_table_probe(table, cmp_key, hash, fingerprint, false, &last_idx, &found);
    // we are't allocating a slot, and this only returns not ok when we're out of capacity or
    // did not find something.
    lg_assert(status == LG_StatusKind_OK || status == LG_StatusKind_NotFound); 
    if (out_found != NULL) {
        *out_found = found;
    }

    return found ? last_idx : 0;
}

LG_StatusKind
lg_table_ensure_u64(
    LG_Table *table,
    uint64_t key,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied
) {
    uint64_t hash;
    uint8_t fingerprint;
    lg_table_make_hash((uint8_t*)&key, 4, &hash, &fingerprint);
    LG_StatusKind status = lg_table_ensure_g(table, key, hash, fingerprint, out_idx, out_was_occupied);
    return status;
}

size_t
lg_table_get_u64(
    LG_Table *table,
    uint64_t key,
    bool *lg_nullable out_found
) {
    uint64_t hash;
    uint8_t fingerprint;
    lg_table_make_hash((uint8_t*)&key, 4, &hash, &fingerprint);
    size_t idx = lg_table_get_g(table, key, hash, fingerprint, out_found);
    return idx;
}

LG_StatusKind
lg_table_ensure_str8(
    LG_Table *table,
    lg_str8 key,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied
) {
    if (key.len == 0) {
        if (out_idx != NULL) {
            *out_idx = 0;
        }
        if (out_was_occupied != NULL) {
            *out_was_occupied = false;
        }
        return LG_StatusKind_InvalidArgument;
    }
    
    uint64_t hash;
    uint8_t fingerprint;
    lg_table_make_hash(key.p, key.len, &hash, &fingerprint);

    // we can't assume the original string memory will still be alive
    // so we can use the first eight bytes (padded) of the string for
    // comparisons.
    // instead, we'll just use a different hash function.
    uint64_t padded_cmp_key = lg_hash_16(key.p, key.len > 16 ? 16 : key.len);

    LG_StatusKind status = lg_table_ensure_g(table, padded_cmp_key, hash, fingerprint, out_idx, out_was_occupied);

    return status;
}

size_t
lg_table_get_str8(
    LG_Table *table,
    lg_str8 key,
    bool *lg_nullable out_found
) {
    if (key.len == 0) {
        if (out_found != NULL) {
            *out_found = false;
        }
        return 0;
    }
    
    uint64_t hash;
    uint8_t fingerprint;
    lg_table_make_hash(key.p, key.len, &hash, &fingerprint);

    uint64_t padded_cmp_key = lg_hash_16(key.p, key.len > 16 ? 16 : key.len);

    size_t idx = lg_table_get_g(table, padded_cmp_key, hash, fingerprint, out_found);
    return idx;
}

void 
lg_table_iter_init(LG_TableIter *iter, LG_Table *table) {
    lg_memzero(iter, sizeof(LG_TableIter));
    iter->table = table;
}

lg_force_inline bool 
lg_table_iter_advance(
    LG_TableIter *iter,
    size_t *lg_nullable out_idx,
    uint64_t *lg_nullable out_cmp_key
) {
    lg_assert(lg_next_pow2(iter->table->cap) == iter->table->cap && iter->table->cap >= 8);

    const size_t fingerprint_blocks_cap = iter->table->cap >> 3;

    bool found = false;
    size_t outer_idx = iter->next_idx >> 3; // / 8
    uint8_t inner_idx = iter->next_idx & 7; // % 8

    while (outer_idx < fingerprint_blocks_cap) {
        uint8_t *block = iter->table->fingerprints_as[outer_idx].individual;
        for (; inner_idx < 8; inner_idx++) {
            if (block[inner_idx] != LG_TableSentinel_Empty) {
                found = true;
                goto out;
            }
        }
        outer_idx++;
        inner_idx = 0;
    }

out:
    if (lg_likely(found)) {
        size_t cur_idx = (outer_idx << 3) + inner_idx;
        if (out_idx != NULL) {
            *out_idx = cur_idx;
        }
        if (out_cmp_key != NULL) {
            *out_cmp_key = iter->table->keys[cur_idx];
        }
        iter->next_idx = cur_idx + 1;
    } else {
        iter->next_idx = iter->table->cap;
    }

    return found;
}
