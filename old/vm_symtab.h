#ifndef LG_VM_SYMTAB_H_
#define LG_VM_SYMTAB_H_

#include <libgrad/internal/alloc.h>
#include <libgrad/internal/core.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
LG_LogicalSymbolTable {
    size_t    table_cap;
    size_t    array_cap;
    size_t    n_symbols;

    bool      *occupied    lg_check_bounds(table_cap);
    uint32_t  *symbol_ids  lg_check_bounds(table_cap);
    size_t    *array_idxs  lg_check_bounds(table_cap);

    LG_StridedDesc  *descs           lg_check_bounds(array_cap);
    size_t          *buffer_offsets  lg_check_bounds(array_cap);
    uint32_t        *buffer_ids      lg_check_bounds(array_cap);
} LG_LogicalSymbolTable;

typedef struct
LG_LogicalSymbolTableIter {
    LG_LogicalSymbolTable *symtab;
    uint32_t symbol_id;
    size_t array_idx;
    
    size_t last_idx;
} LG_LogicalSymbolTableIter;

LG_StatusKind 
lg_symtab_init(LG_LogicalSymbolTable *table, LG_Allocator alloc, size_t cap);

void 
lg_symtab_deinit(LG_LogicalSymbolTable *table, LG_Allocator alloc);

LG_StatusKind 
lg_symtab_upsert(
    LG_LogicalSymbolTable *table,
    size_t *lg_nullable out_idx,
    bool *lg_nullable out_was_occupied,
    uint32_t symbol_id
);

void 
lg_symtab_iter_init(LG_LogicalSymbolTableIter *iter, LG_LogicalSymbolTable *symtab);

/// Returns false once there are no more entries to iterate.
lg_force_inline bool 
lg_symtab_iter_advance(LG_LogicalSymbolTableIter *iter);

#endif // LG_VM_SYMTAB_H_
