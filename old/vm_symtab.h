#ifndef AK_VM_SYMTAB_H_
#define AK_VM_SYMTAB_H_

#include <akimbo/internal/alloc.h>
#include <akimbo/internal/core.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
AK_LogicalSymbolTable {
    size_t    table_cap;
    size_t    array_cap;
    size_t    n_symbols;

    bool      *occupied    ak_check_bounds(table_cap);
    uint32_t  *symbol_ids  ak_check_bounds(table_cap);
    size_t    *array_idxs  ak_check_bounds(table_cap);

    AK_StridedDesc  *descs           ak_check_bounds(array_cap);
    size_t          *buffer_offsets  ak_check_bounds(array_cap);
    uint32_t        *buffer_ids      ak_check_bounds(array_cap);
} AK_LogicalSymbolTable;

typedef struct
AK_LogicalSymbolTableIter {
    AK_LogicalSymbolTable *symtab;
    uint32_t symbol_id;
    size_t array_idx;
    
    size_t last_idx;
} AK_LogicalSymbolTableIter;

AK_StatusKind 
ak_symtab_init(AK_LogicalSymbolTable *table, AK_Allocator alloc, size_t cap);

void 
ak_symtab_deinit(AK_LogicalSymbolTable *table, AK_Allocator alloc);

AK_StatusKind 
ak_symtab_upsert(
    AK_LogicalSymbolTable *table,
    size_t *ak_nullable out_idx,
    bool *ak_nullable out_was_occupied,
    uint32_t symbol_id
);

void 
ak_symtab_iter_init(AK_LogicalSymbolTableIter *iter, AK_LogicalSymbolTable *symtab);

/// Returns false once there are no more entries to iterate.
ak_force_inline bool 
ak_symtab_iter_advance(AK_LogicalSymbolTableIter *iter);

#endif // AK_VM_SYMTAB_H_
