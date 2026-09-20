#include <libgrad/internal/vm_symtab.h>
#include <libgrad/internal/alloc.h>
#include <libgrad/internal/core.h>
#include <libgrad/internal/vm.h>
#include <libgrad/internal/debug.h>
#include <libgrad/internal/strings.h>

#include <stdint.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// Bottom-level internal expression building API
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
LG_AppendOpOptions {
    LG_LogicalSymbol x0_logical;
    LG_LogicalSymbol x1_logical;

    LG_StridedDesc x0_physical;
    LG_StridedDesc x1_physical;

    LG_ExprNodeMeta meta;
} LG_AppendOpOptions;

#define lg_append_op(ctx, opcode, ...) lg_append_op_((ctx), (opcode), (LG_AppendOpOptions){__VA_ARGS__})

LG_LogicalSymbol 
lg_append_op_(
    LG_CompilationContext *ctx,
    LG_LogicalOpcode opcode,
    LG_AppendOpOptions opts
) {
    LG_ExprNode node = (LG_ExprNode){
        .opcode = opcode,
        .x0_logical = opts.x0_logical,
        .x1_logical = opts.x1_logical,

        .x0_physical = opts.x0_physical,
        .x1_physical = opts.x1_physical,

        .meta_as = opts.meta,
    };

    // Allocate a new symbol in the table
    LG_LogicalSymbol y;
    {
        if (lg_opcode_creates_symbol(opcode)) {
            const size_t id = ctx->next_symbol_id;
            ctx->next_symbol_id++;

            size_t symtab_idx;
            bool was_occupied;
            LG_StatusKind status = lg_symtab_upsert(&ctx->symtab, &symtab_idx, &was_occupied, id);
            if (status != LG_StatusKind_OK) {
                if (status == LG_StatusKind_Overflow) {
                    lg_report_error(ctx, status, lg_str8_lit(
                        "failed to insert into the symbol table\n"
                        "there wasn't enough space allocated for it"
                    ));
                } else {
                    lg_report_error(ctx, status, lg_str8_lit("failed to allocate a symbol in the symbol table"));
                }
                return lg_nil(LG_LogicalSymbol);
            }
            lg_assert(!was_occupied);

            y = (LG_LogicalSymbol){ .id = id };

            // In a source operation, the input == the output
            // @bugs revisit this invariant if the correctness of source operations is causing trouble
            if (opcode == LG_LogicalOpcode_Source) {
                ctx->symtab.descs[symtab_idx] = opts.x0_physical;
                node.x0_logical = y;
            }
        } else {
            y = lg_nil(LG_LogicalSymbol);
        }

        node.y_logical = y;
    }

    // Append to the expr
    {
        if (ctx->expr->nodes_len >= ctx->expr->nodes_cap) {
            lg_report_error(ctx, LG_StatusKind_Overflow, lg_str8_lit("overflowed the expr at index %{i64}"), ctx->expr->nodes_len);
            return lg_nil(LG_LogicalSymbol);
        }

        size_t next_idx = ctx->expr->nodes_len;
        ctx->expr->nodes_len += 1;
        ctx->expr->nodes[next_idx] = node;
    }

    return y;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// Buffer table operations
///
////////////////////////////////////////////////////////////////////////////////

LG_StatusKind
lg_buftab_init(
    LG_BufferTable *buftab,
    LG_Allocator alloc,
    size_t cap
) {
    LG_StatusKind status = lg_map_init(&buftab->map, alloc, cap);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    LG_BufferTableEntry* entries = (LG_BufferTableEntry*)lg_alloc_zero(alloc, cap * sizeof(LG_BufferTableEntry));
    if (entries == NULL) {
        lg_map_deinit(&buftab->map, alloc);
        return LG_StatusKind_OutOfMemory;
    }
    buftab->entries = entries;

    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_buftab_insert(LG_BufferTable *buftab, uint32_t id) {
    bool did_exist;
    size_t should_not_exist = lg_map_get(&buftab->map, id, &did_exist);
    if (did_exist) {
        return LG_StatusKind_Duplicate;
    }
    lg_assert(should_not_exist == 0);

    LG_StatusKind status = lg_map_ensure(&buftab->map, id, NULL, NULL);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_buftab_get(LG_BufferTable *buftab, LG_BufferTableEntry *lg_nullable out_entry, uint32_t id) {
    bool found;
    LG_BufferTableEntry entry = buftab->entries[lg_map_get(&buftab->map, id, &found)];
    if (!found) {
        return LG_StatusKind_NotFound;
    }

    if (out_entry != NULL) {
        *out_entry = entry;
    }

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_buftab_update(LG_BufferTable *buftab, uint32_t id, LG_BufferTableEntry new_entry) {
    bool found;
    LG_BufferTableEntry *const entry = &buftab->entries[lg_map_get(&buftab->map, id, &found)];
    if (!found) {
        return LG_StatusKind_NotFound;
    }

    *entry = new_entry;

    return LG_StatusKind_OK;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// Public expression building API
///
////////////////////////////////////////////////////////////////////////////////

LG_LogicalSymbol 
lg_declare_source(
    LG_CompilationContext *ctx,
    LG_StridedDesc physical_desc,
    uint32_t buf_id
) {
    LG_StatusKind status = lg_buftab_get(&ctx->expr->buftab, NULL, buf_id);
    if (status != LG_StatusKind_OK) {
        if (status == LG_StatusKind_NotFound) {
            lg_report_error(ctx, status, lg_str8_lit("attempt to declare source symbol for an invalid buffer id %{i64}"), buf_id);
        } else {
            lg_report_error(ctx, status, lg_str8_lit("failed to get buffer id for source symbol"));
        }
        return lg_nil(LG_LogicalSymbol);
    }

    LG_LogicalSymbol y = lg_append_op(ctx, LG_LogicalOpcode_Source, .x0_physical = physical_desc);

    return y;
}

void
lg_declare_sink(LG_CompilationContext *ctx, LG_LogicalSymbol sym) {
    for (size_t i = 0; i < ctx->expr->nodes_len; i++) {
        if (
            ctx->expr->nodes[i].y_logical.id == sym.id ||
            ctx->expr->nodes[i].x0_logical.id == sym.id ||
            ctx->expr->nodes[i].x1_logical.id == sym.id 
        ) {
            if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Sink) {
                lg_report_error(ctx, LG_StatusKind_Duplicate, lg_str8_lit("attempted to create multiple sink declarations for the same symbol %{i64}"), sym.id);
                return;
            }
            lg_append_op(ctx, LG_LogicalOpcode_Sink, .x0_logical = sym);
            return;
        }
    }

    lg_report_error(ctx, LG_StatusKind_NotFound, lg_str8_lit("attempted to declare invalid symbol %{i64} as sink"), sym.id);

    return;
}

LG_StatusKind 
lg_get_sink_location(
    uint32_t *lg_nullable out_buf_id,
    size_t *lg_nullable out_offset,
    LG_StridedDesc *lg_nullable out_desc,
    LG_LogicalSymbol sym,
    LG_Expr *expr
) {
    uint32_t buf_id = 0;
    size_t offset = 0;
    LG_StridedDesc desc = {0};

    for (size_t i = 0; i < expr->nodes_len; i++) {
        if (expr->nodes[i].opcode != LG_LogicalOpcode_Sink) {
            continue;
        }
        if (expr->nodes[i].y_logical.id == sym.id) {
            buf_id = expr->nodes[i].y_buf_id;
            offset = expr->nodes[i].y_offset;
            desc = expr->nodes[i].y_physical;
            goto found;
        }
        if (expr->nodes[i].x0_logical.id == sym.id) {
            buf_id = expr->nodes[i].x0_buf_id;
            offset = expr->nodes[i].x0_offset;
            desc = expr->nodes[i].x0_physical;
            goto found;
        }
        if (expr->nodes[i].x1_logical.id == sym.id) {
            buf_id = expr->nodes[i].x1_buf_id;
            offset = expr->nodes[i].x1_offset;
            desc = expr->nodes[i].x1_physical;
            goto found;
        }
    }
    return LG_StatusKind_NotFound;

found:;
    if (out_buf_id != NULL) {
        *out_buf_id = buf_id;
    }
    if (out_offset != NULL) {
        *out_offset = offset;
    }
    if (out_desc != NULL) {
        *out_desc = desc;
    }
    return LG_StatusKind_OK;
}

LG_LogicalSymbol 
lg_append_add(
    LG_CompilationContext *ctx,
    const LG_LogicalSymbol x0,
    const LG_LogicalSymbol x1
) {
    LG_LogicalSymbol y = lg_append_op(ctx, LG_LogicalOpcode_Add, .x0_logical = x0, .x1_logical = x1);
    return y;
}

LG_LogicalSymbol 
lg_append_contract(
    LG_CompilationContext *ctx,
    LG_LogicalSymbol x0,
    LG_LogicalSymbol x1,
    size_t n_contracted_axes, 
    size_t n_batch_axes
) {
    LG_LogicalSymbol y = lg_append_op(ctx, LG_LogicalOpcode_Contract, .x0_logical = x0, .x1_logical = x1, .meta = (LG_ExprNodeMeta){
        .contract.n_contracted_axes = n_contracted_axes,
        .contract.n_batch_axes = n_batch_axes,
    });
    return y;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// Compiler passes
///
////////////////////////////////////////////////////////////////////////////////

LG_StatusKind
lg_pass_validate_expr_structure(LG_CompilationContext *ctx) {
    LG_StatusKind status = LG_StatusKind_OK;
    LG_Scope scope = lg_push_scope(ctx->arena);

    // Source/sink rules
    {
        bool sources_begin = false;
        bool sources_end = false;
        bool sinks_begin = false;
            
        for (size_t i = 0; i < ctx->expr->nodes_len; i++) {
            // Source declarations must be the first section of the ctx->expr,
            // while sink declarations must be at the end.
            if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Source && !sources_begin) {
                if (i != 0) {
                    lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                        "found the first source declaration at node index %{i64}\n"
                        "note: source declarations must be the first thing in the expr"
                    ), i);
                    return LG_StatusKind_InvalidArgument;
                }
                sources_begin = true;
            } else if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Source && sources_end) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                    "found a source declaration after a non-source operation at node index %{i64}\n"
                    "note: source declarations must happen one after another"
                ), i);
                return LG_StatusKind_InvalidArgument;
            } else if (ctx->expr->nodes[i].opcode != LG_LogicalOpcode_Source && sources_begin) {
                sources_end = true;
            }

            if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Sink) {
                lg_assert(sources_end);
                sinks_begin = true;
            }

            // Sink declarations must also always be followed by a sink
            // declaration or be the end of the ctx->expr
            if (sinks_begin && ctx->expr->nodes[i].opcode != LG_LogicalOpcode_Sink) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                    "found a non-sink operation after a sink operation at node index %{i64}\n"
                    "note: sink declarations must happen one after another"
                    "and must be the last operations in the expr"
                ), i);
                return LG_StatusKind_InvalidArgument;
            }
        }

        lg_assert(!sinks_begin || (ctx->expr->nodes[ctx->expr->nodes_len - 1].opcode == LG_LogicalOpcode_Sink));
    }

    // Scope validation
    {
        // TODO: @perf this should 100% be a hash set instead of an O(N^2) nightmare
        const size_t seen_ids_cap = ctx->expr->nodes_len * 3;
        size_t seen_ids_len = 0;
        uint32_t *seen_ids = (uint32_t*)lg_arena_alloc(ctx->arena, seen_ids_cap * sizeof(uint32_t), 16);
        if (seen_ids == NULL) {
            // TODO: maybe we need another way to report these kinds of errors
            // these status codes probably shoudn't be mixed with the reporting semantics
            return LG_StatusKind_OutOfMemory;
        }

        for (size_t i = 0; i < ctx->expr->nodes_len; i++) {
            uint32_t new_id = 0;

            if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Source) {
                new_id = ctx->expr->nodes[i].x0_logical.id;
                for (size_t j = 0; j < seen_ids_len; j++) {
                    if (seen_ids[j] == new_id) {
                        lg_report_error(
                            ctx, 
                            LG_StatusKind_InvalidArgument, 
                            lg_str8_lit("found duplicate source declaration for symbol with id %{i64} at node index %{i64}"),
                            seen_ids[j], i
                        );
                        status = LG_StatusKind_InvalidArgument;
                        goto out;
                    }
                }
            } else {
                bool found_x0 = false;
                bool found_x1 = false;
                for (size_t j = 0; j < seen_ids_len; j++) {
                    if (seen_ids[j] == ctx->expr->nodes[i].x0_logical.id) {
                        found_x0 = true;
                    } else if (seen_ids[j] == ctx->expr->nodes[i].x1_logical.id) {
                        found_x1 = true;
                    }
                }

                if (!found_x0)  {
                    lg_report_error(
                        ctx, 
                        LG_StatusKind_InvalidArgument, 
                        lg_str8_lit("use of unknown symbol with id as operand x0 %{i64} at node index %{i64}"),
                        ctx->expr->nodes[i].x0_logical.id, i
                    );
                    status = LG_StatusKind_InvalidArgument;
                    goto out;
                } else if (!found_x1 && lg_opcode_is_binary(ctx->expr->nodes[i].opcode)) {
                    lg_report_error(
                        ctx, 
                        LG_StatusKind_InvalidArgument, 
                        lg_str8_lit("use of unknown symbol with id as operand x1 %{i64} at node index %{i64}"),
                        ctx->expr->nodes[i].x1_logical.id, i
                    );
                    status = LG_StatusKind_InvalidArgument;
                    goto out;
                }

                new_id = ctx->expr->nodes[i].y_logical.id;
            }

            lg_assert(seen_ids_len + 1 <= seen_ids_cap);
            seen_ids[seen_ids_len] = new_id;
            seen_ids_len++;
        }
    }

out:
    lg_pop_scope(ctx->arena, scope);
    return status;
}

LG_StatusKind 
lg_pass_infer_dims(LG_CompilationContext *ctx) {
    LG_StatusKind status = LG_StatusKind_OK;

    for (size_t i = 0; i < ctx->expr->nodes_len; i++) {
        if (ctx->expr->nodes[i].opcode == LG_LogicalOpcode_Source) {
            size_t symtab_idx;
            status = lg_symtab_upsert(&ctx->symtab, &symtab_idx, NULL, ctx->expr->nodes[i].x0_logical.id);
            lg_assert(status == LG_StatusKind_OK);
            ctx->symtab.descs[symtab_idx] = ctx->expr->nodes[i].x0_physical;
            continue;
        }

        bool was_occupied = false;
        size_t x0_symtab_idx = 0;
        size_t x1_symtab_idx = 0;
        status = lg_symtab_upsert(&ctx->symtab, &x0_symtab_idx, &was_occupied, ctx->expr->nodes[i].x0_logical.id);
        lg_assert(status == LG_StatusKind_OK);
        lg_assert(was_occupied);
        if (lg_opcode_is_binary(ctx->expr->nodes[i].opcode)) {
            status = lg_symtab_upsert(&ctx->symtab, &x1_symtab_idx, &was_occupied, ctx->expr->nodes[i].x1_logical.id);
            lg_assert(status == LG_StatusKind_OK);
            lg_assert(was_occupied);
        }
        lg_assert(x0_symtab_idx != x1_symtab_idx);

        size_t rank = 0;
        size_t dim[LG_MAX_RANK] = {0};

        switch (ctx->expr->nodes[i].opcode) {
        case LG_LogicalOpcode_Source:
            lg_unreachable();
            continue;

        case LG_LogicalOpcode_Sink:
            continue;

        case LG_LogicalOpcode_Add:
        case LG_LogicalOpcode_Sub: {
            status = lg_infer_broadcasted_dims(
                &rank,
                dim, 
                (const LG_StridedDesc*[2]){
                    &ctx->symtab.descs[x0_symtab_idx],
                    &ctx->symtab.descs[x1_symtab_idx],
                },
                2
            );
            if (status != LG_StatusKind_OK) {
                return status;
            }
            break;
        }

        case LG_LogicalOpcode_Contract: {
            status = lg_infer_contracted_dims(
                &rank,
                dim, 
                &ctx->symtab.descs[x0_symtab_idx],
                &ctx->symtab.descs[x1_symtab_idx],
                ctx->expr->nodes[i].meta_as.contract.n_contracted_axes,
                ctx->expr->nodes[i].meta_as.contract.n_batch_axes
            );
            if (status != LG_StatusKind_OK) {
                return status;
            }
            break;
        }

        case LG_LogicalOpcode_Hadamard:
        case LG_LogicalOpcode_MSELoss:
        case LG_LogicalOpcode_CrossEntropyLoss:
        case LG_LogicalOpcode_ReLU:
        case LG_LogicalOpcode_StableSoftmax:
        case LG_LogicalOpcode_Sigmoid:
        case LG_LogicalOpcode_LN:
            lg_unreachable("TODO");
        }

        size_t y_symtab_idx = 0;
        status = lg_symtab_upsert(&ctx->symtab, &y_symtab_idx, NULL, ctx->expr->nodes[i].y_logical.id);
        lg_assert(status == LG_StatusKind_OK);
        ctx->symtab.descs[y_symtab_idx].rank = rank;
        for (size_t j = 0; j < LG_MAX_RANK; j++) {
            ctx->symtab.descs[y_symtab_idx].dim[j] = dim[j];
        }
    }

    return LG_StatusKind_OK;
}

void 
lg_pass_assign_layouts(LG_CompilationContext *ctx, LG_LayoutKind layout, size_t unit_align) {
    for (size_t i_node = 0; i_node < ctx->expr->nodes_len; i_node++) {
        LG_StridedDesc *const descs[3] = {
            &ctx->expr->nodes[i_node].y_physical,
            &ctx->expr->nodes[i_node].x0_physical,
            &ctx->expr->nodes[i_node].x1_physical,
        };
        for (size_t i_desc = 0; i_desc < 3; i_desc++) {
            LG_StridedDesc *desc = descs[i_desc];
            for (size_t i_stride = 0; i_stride < LG_MAX_RANK; i_stride++) {
                if (desc->strides[i_stride] != 0) {
                    goto skip_layout;
                }
            }
            LG_StatusKind status = lg_desc_compute_strides(desc, layout, unit_align);
            lg_assert(status == LG_StatusKind_OK);
skip_layout:;
        }
    }
}

lg_force_inline void 
lg_wide_set_bit(size_t len, uint64_t *inout_bitset, bool bit, size_t offset_rtl) {
    const size_t idx = len - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;
    lg_assert(idx < len);
    if (bit) {
        inout_bitset[idx] |= UINT64_C(0x1) << shift;
    } else {
        inout_bitset[idx] &= ~(UINT64_C(0x1) << shift);
    }
}

lg_force_inline bool 
lg_wide_get_bit(size_t len, const uint64_t *bitset, size_t offset_rtl) {
    const size_t idx = len - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;
    lg_assert(idx < len);
    return (bitset[idx] & (UINT64_C(0x1) << shift)) != 0;
}

lg_force_inline void 
lg_wide_or(size_t len, uint64_t *restrict inout_bitset, uint64_t *b) {
    for (size_t i = 0; i < len; i++) {
        inout_bitset[i] |= b[i];
    }
}

lg_force_inline void 
lg_wide_and(size_t len, uint64_t *restrict inout_bitset, uint64_t *b) {
    for (size_t i = 0; i < len; i++) {
        inout_bitset[i] &= b[i];
    }
}

LG_StatusKind 
lg_pass_bufferize(
    LG_CompilationContext *ctx,
    uint32_t buf_id,
    size_t align
) {
    typedef struct 
    SizeTable {
        uint32_t symbol_id;
        size_t symtab_array_idx;
        size_t size_bytes;
        size_t offset;
    } SizeTable;

    LG_StatusKind status = LG_StatusKind_OK;
    LG_ScratchWaypoint *waypoint = lg_scratch_acquire(ctx->scratch);


    //////////////////////////////////////
    // ~~ Calculate physical sizes ~~

    size_t size_table_len = 0;
    SizeTable* const size_table = (SizeTable*)lg_scratch_alloc(
        ctx->scratch,
        &waypoint,
        ctx->symtab.n_symbols * sizeof(SizeTable)
    );
    if (size_table == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    }
    // Construct the size table
    {
        LG_LogicalSymbolTableIter iter = {0};
        lg_symtab_iter_init(&iter, &ctx->symtab);
        while (lg_symtab_iter_advance(&iter)) {
            if (ctx->symtab.buffer_ids[iter.array_idx] != buf_id) {
                continue;
            }

            const size_t size = lg_desc_size_in_bytes(ctx->symtab.descs[iter.array_idx]);
            const size_t size_aligned = lg_align_up(size, align);

            size_table[size_table_len].symbol_id = iter.symbol_id;
            size_table[size_table_len].symtab_array_idx = iter.array_idx;
            size_table[size_table_len].size_bytes = size_aligned;
            size_table_len++;
        }
    }
    // Construct a sorted index map over the size table
    // TODO: @perf something other than bubble sort
    size_t *size_table_sorted_map = (size_t*)lg_scratch_alloc(ctx->scratch, &waypoint, size_table_len * sizeof(size_t));
    if (size_table_sorted_map == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    } 
    for (size_t i = 0; i < size_table_len; i++) {
        size_table_sorted_map[i] = i;
    }
    for (size_t i = 0; i < size_table_len; i++) {
        bool swapped = false;
        for (size_t j = 1; j < size_table_len - i; j++) {
            const size_t idx_a = size_table_sorted_map[j - 1];
            const size_t idx_b = size_table_sorted_map[j];
            if (size_table[idx_a].size_bytes < size_table[idx_b].size_bytes) {
                const size_t temp = size_table_sorted_map[idx_b];
                size_table_sorted_map[idx_b] = size_table_sorted_map[idx_a];
                size_table_sorted_map[idx_a] = temp;
                swapped = true;
            }
        }
        if (!swapped) {
            break;
        }
    }


    /////////////////////////////////////////
    // ~~ Construct the interval graph ~~
    
    const size_t row_elements = lg_align_up(ctx->symtab.n_symbols, 64) / 64;
    const size_t mat_size = row_elements * ctx->symtab.n_symbols * sizeof(uint64_t);
    uint64_t *adj_matrix = (uint64_t*)lg_scratch_alloc(ctx->scratch, &waypoint, mat_size);
    if (adj_matrix == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    } 
    {
        uint64_t *live_set = (uint64_t*)lg_scratch_alloc(ctx->scratch, &waypoint, row_elements * sizeof(uint64_t));
        if (live_set == NULL) {
            status = LG_StatusKind_OutOfMemory;
            goto out_release_scratch;
        }
        for (size_t i_time = ctx->expr->nodes_len; i_time > 0; i_time--) {
            // When a symbol is used for the last time, it dies, meaning it will be live from now until it 
            // is declared (speaking in the reverse-temporal sense)
            size_t x0_idx = 0;
            status = lg_symtab_upsert(&ctx->symtab, &x0_idx, NULL, ctx->expr->nodes[i_time - 1].x0_logical.id);
            lg_assert(status == LG_StatusKind_OK);
            lg_wide_set_bit(row_elements, live_set, true, x0_idx);

            size_t x1_idx = 0;
            status = lg_symtab_upsert(&ctx->symtab, &x1_idx, NULL, ctx->expr->nodes[i_time - 1].x1_logical.id);
            lg_assert(status == LG_StatusKind_OK);
            lg_wide_set_bit(row_elements, live_set, true, x1_idx);

            // After the live set has been updated, we update the matrix
            LG_LogicalSymbolTableIter iter = {0};
            lg_symtab_iter_init(&iter, &ctx->symtab);
            while (lg_symtab_iter_advance(&iter)) {
                bool is_live = lg_wide_get_bit(row_elements, live_set, iter.array_idx);
                if (is_live) {
                    lg_wide_or(row_elements, adj_matrix + (row_elements * iter.array_idx), live_set);
                }
            }

            // Just before a symbol is born, it is not alive
            // We do not kill the value until *after* updating the adjacency matrix b/c
            // the output needs a valid buffer during the operation.
            size_t y_idx = 0;
            status = lg_symtab_upsert(&ctx->symtab, &y_idx, NULL, ctx->expr->nodes[i_time - 1].y_logical.id);
            lg_assert(status == LG_StatusKind_OK);
            lg_wide_set_bit(row_elements, live_set, false, y_idx);
        }
    }


    ////////////////////////////////////////////////////////
    // ~~ Best-fit greedy allocation ~~

    uint64_t *assigned_set = (uint64_t*)lg_scratch_alloc(ctx->scratch, &waypoint, row_elements * sizeof(uint64_t));
    if (assigned_set == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    }
    uint64_t *assigned_and_live_set = (uint64_t*)lg_scratch_alloc(ctx->scratch, &waypoint, row_elements * sizeof(uint64_t));
    if (assigned_and_live_set == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    }

    // `taken_ranges` is kind of a range set over the total memory space required by the block
    // at a given timestep.
    //
    // Each range [array[e], array[e + 1]) where e is on an even-or-zero integer index into the array
    // represents a range of offsets currently occupied by a value.
    // This is done so that any ranges [array[o], array[o + 1]), where o is an odd index into the array
    // represents a range of free offsets.
    //
    // Recording ranges in `taken_ranges` implicitly constructs `free_ranges`.
    const size_t free_ranges_cap = size_table_len * sizeof(size_t) * 2 + 1;
    size_t taken_ranges_len = 0;
    size_t *free_ranges = (size_t*)lg_scratch_alloc(ctx->scratch, &waypoint, free_ranges_cap);
    if (free_ranges == NULL) {
        status = LG_StatusKind_OutOfMemory;
        goto out_release_scratch;
    }
    size_t *const taken_ranges = free_ranges + 1;

    size_t total_size_bytes = 0;

    for (size_t i_unsorted = 0; i_unsorted < size_table_len; i_unsorted++) {
        const size_t i = size_table_sorted_map[i_unsorted];
        SizeTable *const this_symbol = &size_table[i];

        lg_memcpy(assigned_and_live_set, assigned_set, row_elements * sizeof(uint64_t));
        lg_wide_and(row_elements, assigned_and_live_set, adj_matrix + (row_elements * i));

        taken_ranges_len = 0;
        lg_memzero(free_ranges, free_ranges_cap);
        for (size_t j = 0; j < size_table_len; j++) {
            if (!lg_wide_get_bit(row_elements, assigned_and_live_set, size_table[j].symtab_array_idx)) {
                continue;
            }

            taken_ranges[taken_ranges_len] = size_table[j].offset;
            taken_ranges_len++;
            taken_ranges[taken_ranges_len] = size_table[j].offset + size_table[j].size_bytes;
            taken_ranges_len++;

            lg_assert(taken_ranges_len < free_ranges_cap - 1);
        }
        lg_assert(taken_ranges_len % 2 == 0);

        // TODO: @perf something other than bubble sort
        for (size_t j = 0; j < taken_ranges_len; j += 2) {
            for (size_t k = 2; k < taken_ranges_len - j; k += 2) {
                if (taken_ranges[k - 2] > taken_ranges[k]) {
                    size_t temp_start = taken_ranges[k - 2];
                    size_t temp_end = taken_ranges[k - 1];
                    taken_ranges[k - 2] = taken_ranges[k];
                    taken_ranges[k - 1] = taken_ranges[k + 1];
                    taken_ranges[k] = temp_start;
                    taken_ranges[k + 1] = temp_end;
                }
            }
        }

        // Add the infnite space after the end of the array to complete the last
        // free range
        size_t free_ranges_len = taken_ranges_len + 1;
        free_ranges[free_ranges_len] = SIZE_MAX;
        free_ranges_len++;

        bool found_free_range = false;
        size_t minimum_free_range_found = SIZE_MAX;
        size_t offset = 0;
        for (
            size_t j = 0;
            j < free_ranges_len - 1;
            j += 2
        ) {
            const size_t lo = free_ranges[j];
            const size_t hi = free_ranges[j + 1];
            const size_t gap = hi - lo;
            lg_assert(hi > lo);

            if (
                gap >= this_symbol->size_bytes &&
                gap < minimum_free_range_found
            ) {
                offset = free_ranges[j];
                found_free_range = true;
                minimum_free_range_found = gap;
                break;
            }
        }
        lg_assert(found_free_range);

        this_symbol->offset = offset;

        if (this_symbol->offset + this_symbol->size_bytes > total_size_bytes) {
            total_size_bytes = this_symbol->offset + this_symbol->size_bytes;
        }

        lg_wide_set_bit(row_elements, assigned_and_live_set, true, i_unsorted);
    }


    /////////////////////////////////////////////////////////////////////////
    // ~~ Write the offsets & buffer size back to the compilation state ~~

    for (size_t i = 0; i < size_table_len; i++) {
        size_t idx;
        bool occupied;
        status = lg_symtab_upsert(&ctx->symtab, &idx, &occupied, size_table[i].symbol_id);
        lg_assert(status == LG_StatusKind_OK);
        lg_assert(occupied);

        ctx->symtab.buffer_offsets[idx] = size_table[i].offset;
    }

    LG_BufferTableEntry buftab_entry = {
        .size_in_bytes = total_size_bytes,
    };
    status = lg_buftab_update(&ctx->expr->buftab, buf_id, buftab_entry);
    lg_assert(status == LG_StatusKind_OK); // We should only ever be passed valid buffer ids.

out_release_scratch:
    lg_scratch_release(ctx->scratch, &waypoint);
    return LG_StatusKind_OK;
}

void 
lg_pass_decorate_nodes(LG_CompilationContext *ctx) {
    LG_StatusKind status;

    for (size_t i_node = 0; i_node < ctx->expr->nodes_len; i_node++) {
        const uint32_t symbol_ids[3] = {
            ctx->expr->nodes[i_node].y_logical.id,
            ctx->expr->nodes[i_node].x0_logical.id,
            ctx->expr->nodes[i_node].x1_logical.id,
        };
        LG_StridedDesc *const desc_ptrs[3] = {
            &ctx->expr->nodes[i_node].y_physical,
            &ctx->expr->nodes[i_node].x0_physical,
            &ctx->expr->nodes[i_node].x1_physical,
        };
        size_t *const offset_ptrs[3] = {
            &ctx->expr->nodes[i_node].y_offset,
            &ctx->expr->nodes[i_node].x0_offset,
            &ctx->expr->nodes[i_node].x1_offset,
        };
        size_t *const bufid_ptrs[3] = {
            &ctx->expr->nodes[i_node].y_offset,
            &ctx->expr->nodes[i_node].x0_offset,
            &ctx->expr->nodes[i_node].x1_offset,
        };

        for (size_t i_sym = 0; i_sym < 3; i_sym++) {
            size_t symtab_array_idx;
            bool was_occupied;
            status = lg_symtab_upsert(&ctx->symtab, &symtab_array_idx, &was_occupied, symbol_ids[i_sym]);
            lg_assert(status == LG_StatusKind_OK);
            lg_assert(was_occupied);

            *(desc_ptrs[i_sym]) = ctx->symtab.descs[symtab_array_idx];
            *(offset_ptrs[i_sym]) = ctx->symtab.buffer_offsets[symtab_array_idx];
            *(bufid_ptrs[i_sym]) = ctx->symtab.buffer_ids[symtab_array_idx];
        }
    }
}

void 
lg_pass_decorate_with_maps(LG_CompilationContext *ctx) {
    for (size_t i = 0; i < ctx->expr->nodes_len; i++) {
        switch (ctx->expr->nodes[i].opcode) {
        case LG_LogicalOpcode_Source:
        case LG_LogicalOpcode_Sink:
            continue;
        case LG_LogicalOpcode_Add:
        case LG_LogicalOpcode_Sub: {
            LG_StatusKind status = lg_create_broadcast_space((LG_StridedDesc*[]){
                &ctx->expr->nodes[i].y_physical,         
                &ctx->expr->nodes[i].x0_physical,         
                &ctx->expr->nodes[i].x1_physical,         
            }, 3);
            lg_assert(status == LG_StatusKind_OK);
            break;
        }

        case LG_LogicalOpcode_Contract: {
            LG_StatusKind status = lg_create_contraction_space(
                &ctx->expr->nodes[i].y_physical,
                &ctx->expr->nodes[i].x0_physical,         
                &ctx->expr->nodes[i].x1_physical,
                ctx->expr->nodes[i].meta_as.contract.n_batch_axes
            );
            lg_assert(status == LG_StatusKind_OK);
            break;
        }

        case LG_LogicalOpcode_ReLU:
        case LG_LogicalOpcode_StableSoftmax:
        case LG_LogicalOpcode_Sigmoid:
        case LG_LogicalOpcode_LN:
        case LG_LogicalOpcode_Hadamard:
        case LG_LogicalOpcode_MSELoss:
        case LG_LogicalOpcode_CrossEntropyLoss:
            lg_unreachable("TODO");
        }
    }
}

LG_StatusKind 
lg_pass_sort_axes(LG_Expr *expr) {
    LG_StatusKind status;
    for (size_t i = 0; i < expr->nodes_len; i++) {
        status = lg_sort_axes((LG_StridedDesc*[]){
            &expr->nodes[i].y_physical,
            &expr->nodes[i].x0_physical,
            &expr->nodes[i].x1_physical,
        }, 3);
        if (status != LG_StatusKind_OK) {
            return status;
        }
    }
    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_pass_coalesce_axes(LG_Expr *expr) {
    LG_StatusKind status;
    for (size_t i = 0; i < expr->nodes_len; i++) {
        status = lg_coalesce_axes((LG_StridedDesc*[]){
            &expr->nodes[i].y_physical,
            &expr->nodes[i].x0_physical,
            &expr->nodes[i].x1_physical,
        }, 3);
        if (status != LG_StatusKind_OK) {
            return status;
        }
    }
    return LG_StatusKind_OK;
}

LG_StatusKind 
lg_compile_expr(
    LG_CompilationContext *ctx,
    size_t mem_align
) {
    if (ctx->last_status != LG_StatusKind_OK) {
        return ctx->last_status;
    }

    LG_StatusKind status;

    status = lg_pass_validate_expr_structure(ctx);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    status = lg_pass_infer_dims(ctx);
    if (status != LG_StatusKind_OK) {
        return status;
    }
    
    lg_pass_assign_layouts(ctx, LG_LayoutKind_RowMajor /* TODO */, mem_align);

    {
        status = lg_buftab_insert(&ctx->expr->buftab, 0);
        if (status != LG_StatusKind_OK) {
            return status;
        }

        LG_MapIter iter;
        lg_map_iter_init(&iter, &ctx->expr->buftab.map);

        while (lg_map_iter_advance(&iter)) {
            const uint32_t buf_id = iter.key;
            status = lg_pass_bufferize(ctx, buf_id, mem_align);
            if (status != LG_StatusKind_OK) {
                return status;
            }
        }
    }

    // TODO: these functions really need better names
    lg_pass_decorate_nodes(ctx);
    lg_pass_decorate_with_maps(ctx);
    return LG_StatusKind_OK;
}

// TODO: should this be `lg_expr_init`?
LG_StatusKind 
lg_alloc_expr(
    LG_Allocator alloc,
    LG_Expr *expr,
    size_t nodes_cap,
    size_t buftab_cap
) {
    LG_StatusKind status = lg_buftab_init(&expr->buftab, alloc, buftab_cap);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    LG_ExprNode *nodes = (LG_ExprNode*)lg_alloc_zero(alloc, nodes_cap * sizeof(LG_ExprNode));
    if (nodes == NULL) {
        lg_map_deinit(&expr->buftab.map, alloc);
        return LG_StatusKind_OutOfMemory;
    }

    expr->nodes = nodes;
    expr->nodes_cap = nodes_cap;
    expr->nodes_len = 0;

    return LG_StatusKind_OK;
}

void 
lg_free_expr(LG_Allocator allocator, LG_Expr *expr) {
    allocator->free(allocator->ctx, expr->nodes);
    expr->nodes_cap = 0;
    expr->nodes_len = 0;
    expr->nodes = NULL;
}
