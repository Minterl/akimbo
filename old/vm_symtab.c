#include "libgrad/internal/alloc.h"
#include <libgrad/internal/vm_symtab.h>
#include <libgrad/internal/debug.h>

// TODO: deduplicate this code, and migrate the symbol table over to the new
// map structure

#define LG__MMH_C1 0xcc9e2d51u
#define LG__MMH_C2 0x1b873593u
#define LG__MMH_C3 0x85ebca6bu
#define LG__MMH_C4 0xc2b2ae35u
#define LG__MMH_R1 15u
#define LG__MMH_R2 13u
#define LG__MMH_M  5u
#define LG__MMH_N  0xe6546b64u
#define LG__MMH_S  0u

#define LG__MMH_ROL(x, width, bits) (((x) << (bits)) | ((x) >> ((width) - (bits))))

uint32_t 
lg_murmur_hash(uint32_t kh) {
    kh = LG__MMH_ROL(kh * LG__MMH_C1, 32, LG__MMH_R1);
    kh *= LG__MMH_C2;
    kh = LG__MMH_S ^ kh;
    kh = LG__MMH_ROL(kh, 32, LG__MMH_R2) * LG__MMH_M + LG__MMH_N;
    kh = kh ^ 4;
    kh = kh ^ (kh >> 16);
    kh = kh * LG__MMH_C3;
    kh = kh ^ (kh >> 13);
    kh = kh * LG__MMH_C4;
    kh = kh ^ (kh >> 16);
    return kh;
}

LG_StatusKind 
lg_symtab_init(LG_LogicalSymbolTable *table, LG_Allocator alloc, size_t cap) {
    const size_t align = 16;

    const size_t sz_occupied = cap * sizeof(bool);
    const size_t sz_symbol_ids = cap * sizeof(uint32_t);
    const size_t sz_array_idxs = cap * sizeof(size_t);
    const size_t sz_descs = cap * sizeof(LG_StridedDesc);
    const size_t sz_buffer_ids = cap * sizeof(uint32_t);
    const size_t sz_buffer_offsets = cap * sizeof(size_t);

    uint8_t *ptrs[6] = {0};
    size_t bytes_allocated = 0;
    LG_StatusKind status = lg_alloc_contiguous_blocks(
        alloc, 
        ptrs,
        &bytes_allocated,
        (size_t[]){
            sz_occupied,
            sz_symbol_ids,
            sz_array_idxs,
            sz_descs,
            sz_buffer_ids,
            sz_buffer_offsets,
        },
        6,
        align
    );
    if (status != LG_StatusKind_OK) {
        return status;
    }

    for (size_t i = 0; i < bytes_allocated; i++) {
        ptrs[0][i] = 0;
    }

    table->table_cap = cap;
    table->array_cap = cap;
    table->occupied = (bool*)ptrs[0];
    table->symbol_ids = (uint32_t*)ptrs[1];
    table->array_idxs = (size_t*)ptrs[2];
    table->descs = (LG_StridedDesc*)ptrs[3];
    table->buffer_ids = (uint32_t*)ptrs[4];
    table->buffer_offsets = (size_t*)ptrs[5];

    return LG_StatusKind_OK;
}

void 
lg_symtab_deinit(LG_LogicalSymbolTable *table, LG_Allocator alloc) {
    alloc->free(alloc->ctx, table->occupied);
    alloc->free(alloc->ctx, table->descs);
    lg_memzero(table, sizeof(LG_LogicalSymbolTable));
}

LG_StatusKind 
lg_symtab_upsert(
    LG_LogicalSymbolTable *table,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied,
    uint32_t symbol_id
) {
    if (table->table_cap == 0) {
        return LG_StatusKind_Overflow;
    }

    const size_t start_idx = (table->table_cap <= 8) ? (symbol_id % table->table_cap) : (lg_murmur_hash(symbol_id) % table->table_cap);
    for (
        size_t i = start_idx, n_visited = 0;
        n_visited < table->table_cap;
        i = (i + 1) % table->table_cap, n_visited++
    ) {
        if (!table->occupied[i]) {
            table->occupied[i] = true;
            table->symbol_ids[i] = symbol_id;
            table->array_idxs[i] = table->n_symbols;
            table->n_symbols++;
            if (out_idx != NULL) {
                *out_idx = table->array_idxs[i];
            }
            if (out_was_occupied != NULL) {
                *out_was_occupied = false;
            }
            return LG_StatusKind_OK;
        }
        if (table->symbol_ids[i] == symbol_id) {
            if (out_idx != NULL) {
                *out_idx = table->array_idxs[i];
            }
            if (out_was_occupied != NULL) {
                *out_was_occupied = true;
            }
            return LG_StatusKind_OK;
        }
    }

    return LG_StatusKind_Overflow;
}

void 
lg_symtab_iter_init(LG_LogicalSymbolTableIter *iter, LG_LogicalSymbolTable *symtab) {
    lg_assert(symtab != NULL);
    lg_memzero(iter, sizeof(LG_LogicalSymbolTableIter));
    iter->symtab = symtab;
}

lg_force_inline bool 
lg_symtab_iter_advance(LG_LogicalSymbolTableIter *iter) {
    for (; iter->last_idx < iter->symtab->table_cap; iter->last_idx++) {
        const size_t i = iter->last_idx;

        if (!iter->symtab->occupied[i]) {
            continue;
        }

        size_t array_idx = 0;
        LG_StatusKind status = lg_symtab_upsert(iter->symtab, &array_idx, NULL, iter->symtab->symbol_ids[i]);
        lg_assert(status == LG_StatusKind_OK);

        iter->symbol_id = iter->symtab->symbol_ids[i];
        iter->array_idx = array_idx;

        iter->last_idx++;
        return true;
    }

    return false;
}
