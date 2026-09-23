#include "akimbo/internal/base.h"
#include "akimbo/internal/linalg.h"
#include <akimbo/internal/expr.h>


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// private logical expression building utilities
///
////////////////////////////////////////////////////////////////////////////////

#define ak_lbuilder_append(ctx, builder, opcode, ...) ak_lbuilder_append_((ctx), (builder), (opcode), (AK_LogicalBuilderAppendOptions){__VA_ARGS__})

typedef struct
AK_LogicalBuilderAppendOptions {
    AK_LogicalSymbol x0;
    AK_LogicalSymbol x1;

    AK_LogicalSymbolFlags y_flags;
    AK_LogicalMeta meta_as;
} AK_LogicalBuilderAppendOptions;

AK_LogicalSymbol
ak_lbuilder_append_(
    AK_Context *ctx,
    AK_LogicalBuilder *builder,
    AK_LogicalOpcode opcode,
    AK_LogicalBuilderAppendOptions opts
) {
    AK_LogicalBuilderNode *node = ak_arena_alloc_struct(&ctx->arena, AK_LogicalBuilderNode);
    if (node == NULL) {
        ak_report_error(ctx, AK_StatusKind_OutOfMemory, ak_str8_lit("ran out of memory appending to logical expr"));
        return ak_nil(AK_LogicalSymbol);
    }

    builder->next_symbol_id++; // first valid symbol id is 1
    AK_LogicalSymbol y = (AK_LogicalSymbol){ .id = builder->next_symbol_id };
    
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

AK_LogicalSymbol
ak_param(AK_Context *ctx, AK_LogicalBuilder *builder, AK_LogicalShape shape) {
    return ak_lbuilder_append(ctx, builder, AK_LogicalOpcode_Param, .meta_as.param.y_shape = shape);
}

void
ak_pin(AK_Context *ctx, AK_LogicalBuilder *builder, AK_LogicalSymbol sym) {
    AK_LogicalBuilderNode *iter_node = builder->ir_tail;
    while (iter_node != NULL) {
        if (iter_node->node.y.id != sym.id) {
            iter_node = iter_node->prev;
        } else {
            iter_node->node.y_flags |= AK_LogicalSymbolFlag_Pin;
            return;
        }
    }

    ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
        ak_str8_lit("did not find symbol with id %{i64} while attempting to pin it"), sym.id
    );
}

AK_LogicalSymbol
ak_add(AK_Context *ctx, AK_LogicalBuilder *builder, AK_LogicalSymbol x0, AK_LogicalSymbol x1) {
    return ak_lbuilder_append(ctx, builder, AK_LogicalOpcode_Add, .x0 = x0, .x1 = x1);
}

AK_LogicalSymbol
ak_contract(
    AK_Context *ctx,
    AK_LogicalBuilder *builder,
    AK_LogicalSymbol x0,
    AK_LogicalSymbol x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
) {
    return ak_lbuilder_append(
        ctx, builder, AK_LogicalOpcode_Contract,
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

AK_StatusKind
ak_lbuilder_finish(
    AK_Context *ctx,
    AK_LogicalBuilder *builder,
    AK_Allocator artifact_allocator,
    AK_LogicalExpr *out_lexpr
) {
    AK_StatusKind status = ak_check_error(ctx);
    if (status != AK_StatusKind_OK) {
        return status;
    }

    if (builder->next_symbol_id == 0 || builder->ir_tail == NULL) {
        ak_report_error(ctx, AK_StatusKind_InvalidArgument, ak_str8_lit("attempted to finish empty builder"));
        return AK_StatusKind_InvalidArgument;
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
        AK_LogicalBuilderNode *tortoise = builder->ir_tail;
        AK_LogicalBuilderNode *hare = builder->ir_tail;
        for (; tortoise != NULL; tortoise = tortoise->prev, lexpr_len++) {
            if (hare != NULL) {
                hare = hare->prev;
            }
            if (hare != NULL) {
                hare = hare->prev;
            }

            if (ak_unlikely(!is_first_iteration && hare != NULL && tortoise == hare)) {
                ak_report_error(ctx, AK_StatusKind_InvalidArgument, ak_str8_lit("detected cycle in expr builder node list"));
                return AK_StatusKind_InvalidArgument;
            }

            if (ak_unlikely(tortoise->node.y.id > max_symbol_id)) {
                ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.y.id, lexpr_len
                );
                return AK_StatusKind_InvalidArgument;
            } else if (ak_unlikely(tortoise->node.x0.id > max_symbol_id)) {
                ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.x0.id, lexpr_len
                );
                return AK_StatusKind_InvalidArgument;
            } else if (ak_unlikely(tortoise->node.x1.id > max_symbol_id)) {
                ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("found invalid, discontiguous symbol id %{i64} at expr node %{i64}"), tortoise->node.x1.id, lexpr_len
                );
                return AK_StatusKind_InvalidArgument;
            }

            is_first_iteration = false;
        }
    }


    /////////////////////////////////////////////////
    // ~~ copy the data into a contigous array ~~
    
    AK_LogicalInst *lexpr_nodes = (AK_LogicalInst*)ak_alloc_zero(artifact_allocator, lexpr_len * sizeof(AK_LogicalInst));
    if (lexpr_nodes == NULL) {
        ak_report_error(ctx, AK_StatusKind_OutOfMemory, ak_str8_lit("ran out of memory allocating logical expr nodes"));
        return AK_StatusKind_OutOfMemory;
    }

    AK_LogicalBuilderNode *iter_node = builder->ir_tail;
    for (
        size_t i_rev = 0;
        iter_node != NULL;
        iter_node = iter_node->prev, i_rev++
    ) { 
        ak_assert(i_rev < lexpr_len);
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

    return AK_StatusKind_OK;
}

void
ak_lexpr_destroy(AK_LogicalExpr *lexpr, AK_Allocator artifact_allocator) {
    ak_free(artifact_allocator, lexpr->insts);
    ak_memzero(lexpr, sizeof(AK_LogicalExpr));
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// bit vector utilities (quite useful for the following passes)
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
AK_BitVector {
    size_t n_blocks;
    uint64_t *blocks ak_check_bounds(n_blocks);
} AK_BitVector;

ak_force_inline AK_StatusKind
ak_bv_init(AK_BitVector *bv, AK_Arena *arena, size_t set_width) {
    ak_memzero(bv, sizeof(AK_BitVector));

    size_t n_blocks = ak_align_up(set_width, 64) / 64;
    uint64_t *blocks = (uint64_t*)ak_arena_alloc(arena, n_blocks * sizeof(uint64_t), 4);
    if (blocks == NULL) {
        return AK_StatusKind_OutOfMemory;
    }

    bv->n_blocks = n_blocks;
    bv->blocks = blocks;

    return AK_StatusKind_OK;
}

ak_force_inline void 
ak_bv_set_bit(AK_BitVector *bv, bool bit, size_t offset_rtl) {
    const size_t idx = bv->n_blocks - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;

    if (bit) {
        bv->blocks[idx] |= UINT64_C(0x1) << shift;
    } else {
        bv->blocks[idx] &= ~(UINT64_C(0x1) << shift);
    }
}

ak_force_inline bool 
ak_bv_get_bit(const AK_BitVector *bv, size_t offset_rtl) {
    const size_t idx = bv->n_blocks - 1 - (offset_rtl / 64);
    const size_t shift = offset_rtl % 64;

    return (bv->blocks[idx] & (UINT64_C(0x1) << shift)) != 0;
}

ak_force_inline void 
ak_bv_or(AK_BitVector *bv, uint64_t *b) {
    for (size_t i = 0; i < bv->n_blocks; i++) {
        bv->blocks[i] |= b[i];
    }
}

ak_force_inline void 
ak_bv_and(AK_BitVector *bv, uint64_t *b) {
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
AK_StatusKind
ak_validate_lexpr_structure(AK_Context *ctx, AK_LogicalExpr *lexpr) {
    AK_StatusKind status = AK_StatusKind_OK;
    AK_Scope scope = ak_push_scope(&ctx->arena);

    //////////////////////////////////////////////////////////////
    // ~~ param dominance validation ~~
    // this is very easy b/c there' no control flow to speak of
    
    {
        bool params_begin = false;
        bool params_end = false;
            
        for (size_t i = 0; i < lexpr->len; i++) {
            if (lexpr->insts[i].opcode == AK_LogicalOpcode_Param && !params_begin) {
                if (i != 0) {
                    ak_report_error(ctx, AK_StatusKind_InvalidArgument, ak_str8_lit(
                        "found the first param declaration at node index %{i64}\n"
                        "note: param declarations must be the first thing in the expr"
                    ), i);
                    status = AK_StatusKind_InvalidArgument;
                    goto out;
                }
                params_begin = true;
            } else if (lexpr->insts[i].opcode == AK_LogicalOpcode_Param && params_end) {
                ak_report_error(ctx, AK_StatusKind_InvalidArgument, ak_str8_lit(
                    "found a param declaration after a non-param operation at node index %{i64}\n"
                    "note: param declarations must happen one after another"
                ), i);
                status = AK_StatusKind_InvalidArgument;
                goto out;
            } else if (lexpr->insts[i].opcode != AK_LogicalOpcode_Param && params_begin) {
                params_end = true;
            }
        }
    }


    //////////////////////////////////////////////
    // ~~ scope validation ~~

    {
        AK_BitVector seen_set;
        status = ak_bv_init(&seen_set, &ctx->arena, lexpr->max_symbol_id);
        if (status == AK_StatusKind_OutOfMemory) {
            ak_report_error(ctx, AK_StatusKind_OutOfMemory, ak_str8_lit("ran out of memory allocating scratch space for validation"));
            goto out;
        } else if (status != AK_StatusKind_OK) {
            ak_report_error(ctx, status, ak_str8_lit("failed to initialize a scratch structure during validation"));
            goto out;
        }

        for (size_t i = 0; i < lexpr->len; i++) {
            const uint32_t x0_id = lexpr->insts[i].x0.id;
            const uint32_t x1_id = lexpr->insts[i].x1.id;
            const uint32_t new_id = lexpr->insts[i].y.id;

            const bool found_x0 = ak_bv_get_bit(&seen_set, x0_id);
            const bool found_x1 = ak_bv_get_bit(&seen_set, x1_id);
            const bool found_y  = ak_bv_get_bit(&seen_set, new_id);

            if (new_id == 0) {
                ak_report_error(ctx,AK_StatusKind_InvalidArgument, ak_str8_lit(
                    "found a symbol with id 0 at node index %{i64}\n"
                    "0 is a reserved symbol id (invalid)"
                ));
                status = AK_StatusKind_InvalidArgument;
                goto out;
            }

            if (found_y) {
                ak_report_error(
                    ctx, 
                    AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("symbol %{i64} was born for the second time at node index %{i64}, violating SSA"),
                    lexpr->insts[i].y.id, i
                );
                status = AK_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x0 && !ak_lopcode_is_ctor(lexpr->insts[i].opcode))  {
                ak_report_error(
                    ctx, 
                    AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("use of unknown symbol with id %{i64} as the first operand at node index %{i64}"),
                    x0_id, i
                );
                status = AK_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x1 && ak_lopcode_is_binary(lexpr->insts[i].opcode)) {
                ak_report_error(
                    ctx, 
                    AK_StatusKind_InvalidArgument, 
                    ak_str8_lit("use of unknown symbol with id %{i64} as the second operand at node index %{i64}"),
                    x1_id, i
                );
                status = AK_StatusKind_InvalidArgument;
                goto out;
            }

            ak_bv_set_bit(&seen_set, 1, new_id);
        }
    }

out:
    ak_pop_scope(&ctx->arena, scope);
    return status;
}

/// assumes that there are `expr.max_symbol_id` elements in `out_shapes`,
/// that the memory thereof is zeroed, and that the structural invariants
/// of the SSA form hold s.t shapes will never attempt to infer themselves
/// on other nil shapes
AK_StatusKind
ak_infer_y_shape(AK_Context *ctx, const AK_LogicalInst *node, AK_LogicalShape *inout_shapes) {
    AK_StatusKind status = AK_StatusKind_OK;

    switch (node->opcode) {
    case AK_LogicalOpcode_Param:
        inout_shapes[node->y.id].rank = node->meta_as.param.y_shape.rank;
        ak_memcpy(&inout_shapes[node->y.id], &node->meta_as.param.y_shape.rank, sizeof(size_t) * AK_MAX_RANK);
        break;

    case AK_LogicalOpcode_Add:
    case AK_LogicalOpcode_Sub: {
        AK_LogicalShape y;
        status = ak_infer_broadcasted_dims(&y, (const AK_LogicalShape*[2]){
            &inout_shapes[node->x0.id],
            &inout_shapes[node->x1.id],
        }, 2);
        if (status != AK_StatusKind_OK) {
            ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
                ak_str8_lit("symbols %{i64} and %{i64} could not be broadcasted"),
                node->x0.id, node->x1.id
            );
            return status;
        }
        inout_shapes[node->y.id] = y;
        break;
    }

    case AK_LogicalOpcode_Contract: {
        AK_LogicalShape y;
        status = ak_infer_contracted_dims(
            &y,
            &inout_shapes[node->x0.id],
            &inout_shapes[node->x1.id],
            node->meta_as.contract.n_contracted_axes,
            node->meta_as.contract.n_batch_axes
        );
        if (status != AK_StatusKind_OK) {
            ak_report_error(ctx, AK_StatusKind_InvalidArgument, 
                ak_str8_lit("symbols %{i64} and %{i64} could not be contracted"),
                node->x0.id, node->x1.id
            );
            return status;
        }
        inout_shapes[node->y.id] = y;
        break;
    }

    case AK_LogicalOpcode_Hadamard:
    case AK_LogicalOpcode_ReLU:
    case AK_LogicalOpcode_LN:
        ak_unreachable("TODO");
    }

    return AK_StatusKind_OK;
}

AK_StatusKind
ak_lower_lexpr(
    AK_Context *ctx,
    AK_Allocator artifact_allocator,
    AK_LogicalExpr *lexpr,
    AK_LogicalExprLoweringFlags flags
) {

    // TODO: the layout assignment and alignment needs to be smarter than this
    static const AK_LayoutKind DEFAULT_LAYOUT = AK_LayoutKind_RowMajor;
    static const AK_LayoutKind DEFAULT_ALIGN = 16;

    AK_StatusKind status = AK_StatusKind_OK;
    AK_Scope scope = ak_push_scope(&ctx->arena);


    /////////////////////////////////////////////////////////////////
    // ~~ validate SSA invariants ~~

    if (!(flags & AK_LogicalExprLoweringFlag_NoStructuralInvariantValidation)) {
        status = ak_validate_lexpr_structure(ctx, lexpr);
        if (status != AK_StatusKind_OK) {
            goto out;
        }
    }
    

    /////////////////////////////////////////////////////////////////
    // ~~ shape inference ~~

    AK_LogicalShape *shapes = ak_arena_alloc_array(&ctx->arena, AK_LogicalShape, lexpr->max_symbol_id);
    if (shapes == NULL) {
        status = AK_StatusKind_OutOfMemory;
        ak_report_error(ctx, status, ak_str8_lit("ran out of memory allocating a scratch structure"));
        goto out;
    }
    for (size_t i = 0; i < lexpr->len; i++) {
        status = ak_infer_y_shape(ctx, &lexpr->insts[i], shapes);
        if (status != AK_StatusKind_OK) {
            goto out;
        }
    }


    /////////////////////////////////////////////////////////////////
    // ~~ calculate address operators ~~
    
    AK_AffineTransform **addr_ops = ak_arena_alloc_array(&ctx->arena, AK_AffineTransform*, lexpr->max_symbol_id);
    if (addr_ops == NULL) {
        status = AK_StatusKind_OutOfMemory;
        ak_report_error(ctx, status, ak_str8_lit("ran out of memory allocating a scratch structure"));
        goto out;
    }
    for (size_t i = 0; i < lexpr->max_symbol_id; i++) {
        status = ak_atran_strided_projection_from_shape(
            &ctx->arena,
            &shapes[i],
            DEFAULT_LAYOUT, DEFAULT_ALIGN,
            &addr_ops[i]
        );
        if (status != AK_StatusKind_OK) {
            ak_assert(status == AK_StatusKind_OutOfMemory);
            ak_report_error(ctx, status, ak_str8_lit("ran out of memory allocating a scratch structure"));
            goto out;
        }

        ak_assert(ak_atran_is_valid_address_operator(addr_ops[i]));
    }


    /////////////////////////////////////////////////////////////////
    // ~~ fin ~~

    (void)artifact_allocator;

out:
    ak_pop_scope(&ctx->arena, scope);
    return status;
}
