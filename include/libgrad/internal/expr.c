#include "libgrad/internal/base.h"
#include "libgrad/internal/linalg.h"
#include <libgrad/internal/expr.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// private logical expression building utilities
///
////////////////////////////////////////////////////////////////////////////////

#define lg_lbuilder_append(ctx, builder, opcode, ...) lg_lbuilder_append_((ctx), (builder), (opcode), (LG_LogicalBuilderAppendOptions){__VA_ARGS__})

typedef struct
LG_LogicalBuilderAppendOptions {
    LG_LogicalSymbol x0;
    LG_LogicalSymbol x1;

    LG_LogicalSymbolFlags y_flags;
    LG_LogicalMeta meta_as;
} LG_LogicalBuilderAppendOptions;

LG_LogicalSymbol
lg_lbuilder_append_(
    LG_Context *ctx,
    LG_LogicalBuilder *builder,
    LG_LogicalOpcode opcode,
    LG_LogicalBuilderAppendOptions opts
) {
    LG_LogicalBuilderNode *node = lg_arena_alloc_struct(&ctx->arena, LG_LogicalBuilderNode);
    if (node == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory appending to logical expr"));
        return lg_nil(LG_LogicalSymbol);
    }

    builder->next_symbol_id++; // first valid symbol id is 1
    LG_LogicalSymbol y = (LG_LogicalSymbol){ .id = builder->next_symbol_id };
    
    node->node.opcode = opcode;
    node->node.x0 = opts.x0;
    node->node.x1 = opts.x1;
    node->node.y = y;
    node->node.y_flags = opts.y_flags;
    node->node.meta_as = opts.meta_as;

    if (builder->ir_tail != NULL) {
        node->prev = builder->ir_tail;
    }
    builder->ir_tail = node;

    return y;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// public logical expression building utilities
///
////////////////////////////////////////////////////////////////////////////////

LG_LogicalSymbol
lg_param(LG_Context *ctx, LG_LogicalBuilder *builder, LG_LogicalShape shape) {
    return lg_lbuilder_append(ctx, builder, LG_LogicalOpcode_Param, .meta_as.param.y_shape = shape);
}

void
lg_pin(LG_Context *ctx, LG_LogicalBuilder *builder, LG_LogicalSymbol sym) {
    LG_LogicalBuilderNode *iter_node = builder->ir_tail;
    while (iter_node != NULL) {
        if (iter_node->node.y.id != sym.id) {
            iter_node = iter_node->prev;
        } else {
            iter_node->node.y_flags |= LG_LogicalSymbolFlag_Pin;
            return;
        }
    }

    lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
        lg_str8_lit("did not find symbol with id %{i64} while attempting to pin it"), sym.id
    );
}

LG_LogicalSymbol
lg_add(LG_Context *ctx, LG_LogicalBuilder *builder, LG_LogicalSymbol x0, LG_LogicalSymbol x1) {
    return lg_lbuilder_append(ctx, builder, LG_LogicalOpcode_Add, .x0 = x0, .x1 = x1);
}

LG_LogicalSymbol
lg_contract(
    LG_Context *ctx,
    LG_LogicalBuilder *builder,
    LG_LogicalSymbol x0,
    LG_LogicalSymbol x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
) {
    return lg_lbuilder_append(
        ctx, builder, LG_LogicalOpcode_Contract,
        .x0 = x0,
        .x1 = x1,
        .meta_as.contract.n_contracted_axes = n_contracted_axes,
        .meta_as.contract.n_batch_axes = n_batch_axes
    );
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// freeze a logical expression builder into contiguous memory &
/// perform early validation
///
/// also destroy it eventually
///
////////////////////////////////////////////////////////////////////////////////

LG_StatusKind
lg_lbuilder_finish(
    LG_Context *ctx,
    LG_LogicalBuilder *builder,
    LG_Allocator artifact_allocator,
    LG_LogicalExpr *out_lexpr
) {
    LG_StatusKind status = lg_check_error(ctx);
    if (status != LG_StatusKind_OK) {
        return status;
    }

    if (builder->next_symbol_id == 0 || builder->ir_tail == NULL) {
        lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit("attempted to finish empty builder"));
        return LG_StatusKind_InvalidArgument;
    }

    ////////////////////////////////////////////////////////////////////////////////
    // ~~ find the length of the expr & validate two invariants: ~~
    // 1) no symbol id > the current ctx.next_symbol_id
    //    - we do this b/c lets us use raw integer indices for the symbol table
    // 2) there are no cycles in the sll

    size_t lexpr_len = 0;
    const size_t max_symbol_id = builder->next_symbol_id;
    {
        bool is_first_iteration = true;
        LG_LogicalBuilderNode *tortoise = builder->ir_tail;
        LG_LogicalBuilderNode *hare = builder->ir_tail;
        for (; tortoise != NULL; tortoise = tortoise->prev, lexpr_len++) {
            if (hare != NULL) {
                hare = hare->prev;
            }
            if (hare != NULL) {
                hare = hare->prev;
            }

            if (lg_unlikely(!is_first_iteration && hare != NULL && tortoise == hare)) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit("detected cycle in expr builder node list"));
                return LG_StatusKind_InvalidArgument;
            }

            if (lg_unlikely(tortoise->node.y.id > max_symbol_id)) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.y.id, lexpr_len
                );
                return LG_StatusKind_InvalidArgument;
            } else if (lg_unlikely(tortoise->node.x0.id > max_symbol_id)) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.x0.id, lexpr_len
                );
                return LG_StatusKind_InvalidArgument;
            } else if (lg_unlikely(tortoise->node.x1.id > max_symbol_id)) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.x1.id, lexpr_len
                );
                return LG_StatusKind_InvalidArgument;
            }

            is_first_iteration = false;
        }
    }


    /////////////////////////////////////////////////
    // ~~ copy the data into a contigous array ~~
    
    LG_LogicalInst *lexpr_nodes = (LG_LogicalInst*)lg_alloc_zero(artifact_allocator, lexpr_len * sizeof(LG_LogicalInst));
    if (lexpr_nodes == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory allocating logical expr nodes"));
        return LG_StatusKind_OutOfMemory;
    }

    LG_LogicalBuilderNode *iter_node = builder->ir_tail;
    for (
        size_t i_rev = 0;
        iter_node != NULL;
        iter_node = iter_node->prev, i_rev++
    ) { 
        lg_assert(i_rev < lexpr_len);
        const size_t i = lexpr_len - 1 - i_rev;
        
        lexpr_nodes[i].opcode  = iter_node->node.opcode;
        lexpr_nodes[i].y       = iter_node->node.y;
        lexpr_nodes[i].x0      = iter_node->node.x0;
        lexpr_nodes[i].x1      = iter_node->node.x1;
        lexpr_nodes[i].y_flags = iter_node->node.y_flags;
        lexpr_nodes[i].meta_as = iter_node->node.meta_as;
    }


    ////////////////////////////////////////
    // ~~ fin ~~

    out_lexpr->max_symbol_id = max_symbol_id;
    out_lexpr->len = lexpr_len;
    out_lexpr->insts = lexpr_nodes;

    return LG_StatusKind_OK;
}

void
lg_lexpr_destroy(LG_LogicalExpr *lexpr, LG_Allocator artifact_allocator) {
    lg_free(artifact_allocator, lexpr->insts);
    lg_memzero(lexpr, sizeof(LG_LogicalExpr));
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// bit vector utilities (quite useful for the following passes)
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
LG_BitVector {
    size_t n_blocks;
    uint64_t *blocks lg_check_bounds(n_blocks);
} LG_BitVector;

lg_force_inline LG_StatusKind
lg_bv_init(LG_BitVector *bv, LG_Arena *arena, size_t set_width) {
    lg_memzero(bv, sizeof(LG_BitVector));

    size_t n_blocks = lg_align_up(set_width, 64) / 64;
    uint64_t *blocks = (uint64_t*)lg_arena_alloc(arena, n_blocks * sizeof(uint64_t), 4);
    if (blocks == NULL) {
        return LG_StatusKind_OutOfMemory;
    }

    bv->n_blocks = n_blocks;
    bv->blocks = blocks;

    return LG_StatusKind_OK;
}

lg_force_inline void 
lg_bv_set_bit(LG_BitVector *bv, bool bit, size_t offset_rtl) {
    const size_t idx = bv->n_blocks - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;

    if (bit) {
        bv->blocks[idx] |= UINT64_C(0x1) << shift;
    } else {
        bv->blocks[idx] &= ~(UINT64_C(0x1) << shift);
    }
}

lg_force_inline bool 
lg_bv_get_bit(const LG_BitVector *bv, size_t offset_rtl) {
    const size_t idx = bv->n_blocks - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;

    return (bv->blocks[idx] & (UINT64_C(0x1) << shift)) != 0;
}

lg_force_inline void 
lg_bv_or(LG_BitVector *bv, uint64_t *b) {
    for (size_t i = 0; i < bv->n_blocks; i++) {
        bv->blocks[i] |= b[i];
    }
}

lg_force_inline void 
lg_bv_and(LG_BitVector *bv, uint64_t *b) {
    for (size_t i = 0; i < bv->n_blocks; i++) {
        bv->blocks[i] &= b[i];
    }
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// passes for lowering a logical expression
///
////////////////////////////////////////////////////////////////////////////////

/// validate functional structure of the expression
LG_StatusKind
lg_validate_lexpr_structure(LG_Context *ctx, LG_LogicalExpr *lexpr) {
    LG_StatusKind status = LG_StatusKind_OK;
    LG_Scope scope = lg_push_scope(&ctx->arena);

    //////////////////////////////////////////////////////////////
    // ~~ param dominance validation ~~
    // this is very easy b/c there' no control flow to speak of
    
    {
        bool params_begin = false;
        bool params_end = false;
            
        for (size_t i = 0; i < lexpr->len; i++) {
            if (lexpr->insts[i].opcode == LG_LogicalOpcode_Param && !params_begin) {
                if (i != 0) {
                    lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                        "found the first param declaration at node index %{i64}\n"
                        "note: param declarations must be the first thing in the expr"
                    ), i);
                    status = LG_StatusKind_InvalidArgument;
                    goto out;
                }
                params_begin = true;
            } else if (lexpr->insts[i].opcode == LG_LogicalOpcode_Param && params_end) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                    "found a param declaration after a non-param operation at node index %{i64}\n"
                    "note: param declarations must happen one after another"
                ), i);
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (lexpr->insts[i].opcode != LG_LogicalOpcode_Param && params_begin) {
                params_end = true;
            }
        }
    }


    //////////////////////////////////////////////
    // ~~ scope validation ~~

    {
        LG_BitVector seen_set;
        status = lg_bv_init(&seen_set, &ctx->arena, lexpr->max_symbol_id);
        if (status == LG_StatusKind_OutOfMemory) {
            lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory allocating scratch space for validation"));
            goto out;
        } else if (status != LG_StatusKind_OK) {
            lg_report_error(ctx, status, lg_str8_lit("failed to initialize a scratch structure during validation"));
            goto out;
        }

        for (size_t i = 0; i < lexpr->len; i++) {
            const uint32_t x0_id = lexpr->insts[i].x0.id;
            const uint32_t x1_id = lexpr->insts[i].x1.id;
            const uint32_t new_id = lexpr->insts[i].y.id;

            const bool found_x0 = lg_bv_get_bit(&seen_set, x0_id);
            const bool found_x1 = lg_bv_get_bit(&seen_set, x1_id);
            const bool found_y  = lg_bv_get_bit(&seen_set, new_id);

            if (new_id == 0) {
                lg_report_error(ctx,LG_StatusKind_InvalidArgument, lg_str8_lit(
                    "found a symbol with id 0 at node index %{i64}\n"
                    "0 is a reserved symbol id (invalid)"
                ));
                status = LG_StatusKind_InvalidArgument;
                goto out;
            }

            if (found_y) {
                lg_report_error(
                    ctx, 
                    LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("symbol %{i64} was born for the second time at node index %{i64}, violating SSA"),
                    lexpr->insts[i].y.id, i
                );
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x0 && !lg_lopcode_is_ctor(lexpr->insts[i].opcode))  {
                lg_report_error(
                    ctx, 
                    LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("use of unknown symbol with id %{i64} as the first operand at node index %{i64}"),
                    x0_id, i
                );
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x1 && lg_lopcode_is_binary(lexpr->insts[i].opcode)) {
                lg_report_error(
                    ctx, 
                    LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("use of unknown symbol with id %{i64} as the second operand at node index %{i64}"),
                    x1_id, i
                );
                status = LG_StatusKind_InvalidArgument;
                goto out;
            }

            lg_bv_set_bit(&seen_set, 1, new_id);
        }
    }

out:
    lg_pop_scope(&ctx->arena, scope);
    return status;
}

/// assumes that there are `expr.max_symbol_id` elements in `out_shapes`,
/// that the memory thereof is zeroed, and that the structural invariants
/// of the SSA form hold s.t shapes will never attempt to infer themselves
/// on other nil shapes
LG_StatusKind
lg_infer_y_shape(LG_Context *ctx, const LG_LogicalInst *node, LG_LogicalShape *inout_shapes) {
    LG_StatusKind status = LG_StatusKind_OK;

    switch (node->opcode) {
    case LG_LogicalOpcode_Param:
        inout_shapes[node->y.id].rank = node->meta_as.param.y_shape.rank;
        lg_memcpy(&inout_shapes[node->y.id], &node->meta_as.param.y_shape.rank, sizeof(size_t) * LG_MAX_RANK);
        break;

    case LG_LogicalOpcode_Add:
    case LG_LogicalOpcode_Sub: {
        LG_LogicalShape y;
        status = lg_infer_broadcasted_dims(&y, (const LG_LogicalShape*[2]){
            &inout_shapes[node->x0.id],
            &inout_shapes[node->x1.id],
        }, 2);
        if (status != LG_StatusKind_OK) {
            lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
                lg_str8_lit("symbols %{i64} and %{i64} could not be broadcasted"),
                node->x0.id, node->x1.id
            );
            return status;
        }
        inout_shapes[node->y.id] = y;
        break;
    }

    case LG_LogicalOpcode_Contract: {
        LG_LogicalShape y;
        status = lg_infer_contracted_dims(
            &y,
            &inout_shapes[node->x0.id],
            &inout_shapes[node->x1.id],
            node->meta_as.contract.n_contracted_axes,
            node->meta_as.contract.n_batch_axes
        );
        if (status != LG_StatusKind_OK) {
            lg_report_error(ctx, LG_StatusKind_InvalidArgument, 
                lg_str8_lit("symbols %{i64} and %{i64} could not be contracted"),
                node->x0.id, node->x1.id
            );
            return status;
        }
        inout_shapes[node->y.id] = y;
        break;
    }

    case LG_LogicalOpcode_Hadamard:
    case LG_LogicalOpcode_ReLU:
    case LG_LogicalOpcode_LN:
        lg_unreachable("TODO");
    }

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_lower_lexpr(
    LG_Context *ctx,
    LG_Allocator artifact_allocator,
    LG_LogicalExpr *lexpr,
    LG_LogicalExprLoweringFlags flags
) {

    // TODO: the layout assignment and alignment needs to be smarter than this
    static const LG_LayoutKind DEFAULT_LAYOUT = LG_LayoutKind_RowMajor;
    static const LG_LayoutKind DEFAULT_ALIGN = 16;

    LG_StatusKind status = LG_StatusKind_OK;
    LG_Scope scope = lg_push_scope(&ctx->arena);


    /////////////////////////////////////////////////////////////////
    // ~~ validate SSA invariants ~~

    if (!(flags & LG_LogicalExprLoweringFlag_NoStructuralInvariantValidation)) {
        status = lg_validate_lexpr_structure(ctx, lexpr);
        if (status != LG_StatusKind_OK) {
            goto out;
        }
    }
    

    /////////////////////////////////////////////////////////////////
    // ~~ shape inference ~~

    LG_LogicalShape *shapes = lg_arena_alloc_array(&ctx->arena, LG_LogicalShape, lexpr->max_symbol_id);
    if (shapes == NULL) {
        status = LG_StatusKind_OutOfMemory;
        lg_report_error(ctx, status, lg_str8_lit("ran out of memory allocating a scratch structure"));
        goto out;
    }
    for (size_t i = 0; i < lexpr->len; i++) {
        status = lg_infer_y_shape(ctx, &lexpr->insts[i], shapes);
        if (status != LG_StatusKind_OK) {
            goto out;
        }
    }


    /////////////////////////////////////////////////////////////////
    // ~~ calculate address operators ~~
    
    LG_AffineTransform **addr_ops = lg_arena_alloc_array(&ctx->arena, LG_AffineTransform*, lexpr->max_symbol_id);
    if (addr_ops == NULL) {
        status = LG_StatusKind_OutOfMemory;
        lg_report_error(ctx, status, lg_str8_lit("ran out of memory allocating a scratch structure"));
        goto out;
    }
    for (size_t i = 0; i < lexpr->max_symbol_id; i++) {
        status = lg_atran_strided_projection_from_shape(
            &ctx->arena,
            &shapes[i],
            DEFAULT_LAYOUT, DEFAULT_ALIGN,
            &addr_ops[i]
        );
        if (status != LG_StatusKind_OK) {
            lg_assert(status == LG_StatusKind_OutOfMemory);
            lg_report_error(ctx, status, lg_str8_lit("ran out of memory allocating a scratch structure"));
            goto out;
        }

        lg_assert(lg_atran_is_valid_address_operator(addr_ops[i]));
    }


    /////////////////////////////////////////////////////////////////
    // ~~ fin ~~

    (void)artifact_allocator;

out:
    lg_pop_scope(&ctx->arena, scope);
    return status;
}
