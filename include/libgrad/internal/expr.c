#include <libgrad/internal/expr.h>

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// 
/// error reporting shennanigans
///
/// TODO: move this with the context stuff to a context.(h|c)
///
////////////////////////////////////////////////////////////////////////////////

void
lg_perror(LG_Context *ctx, LG_Writer *writer) {
    lg_str8 as_string = (lg_str8){.len = ctx->err_msg_len, .p = ctx->err_msg_backing_buf};
    lg_printf(writer, as_string);
    lg_printf(writer, lg_str8_lit("\n"));
}

size_t 
lg_report_error_write(void *ctx_, lg_str8 str) {
    LG_Context *ctx = ctx_;
    size_t bytes_written = lg_strcpy((lg_str8){
        .len = LG_MAX_ERR_LEN - ctx->err_msg_len,
        .p = ctx->err_msg_backing_buf + ctx->err_msg_len,
    }, str);
    ctx->err_msg_len += bytes_written;
    return bytes_written;
}

/// Reports error on a best-effort basis, filling the buffer as much as possible.
/// Does nothing if the error has already been set
void 
lg_report_error(LG_Context *ctx, LG_StatusKind status, lg_str8 fmt, ...) {
    if (ctx->last_status != LG_StatusKind_OK) {
        return;
    }

    lg_assert(status != LG_StatusKind_OK);

    ctx->err_msg_len = 0;
    ctx->last_status = status;

    LG_Writer w = {
        .ctx = (void*)ctx,
        .write = lg_report_error_write,
    };

    va_list ap;
    va_start(ap, fmt);
    LG_StatusKind vprintf_status = lg_vprintf(&w, fmt, ap);
    (void)vprintf_status;
    va_end(ap);
}


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
    LG_ExprNodeMeta meta_as;
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
    LG_Allocator *artifact_allocator,
    LG_LogicalExpr *out_lexpr
) {
    if (ctx->last_status != LG_StatusKind_OK) {
        return ctx->last_status;
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
    
    LG_LogicalExprNode *lexpr_nodes = (LG_LogicalExprNode*)lg_alloc_zero(artifact_allocator, lexpr_len * sizeof(LG_LogicalExprNode));
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
    out_lexpr->nodes = lexpr_nodes;

    return LG_StatusKind_OK;
}

void
lg_lexpr_destroy(LG_LogicalExpr *lexpr, LG_Allocator *artifact_allocator) {
    lg_free(artifact_allocator, lexpr->nodes);
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
/// hedral expression builder
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
LG_HedralBuilderNode {
    struct LG_HedralBuilderNode  *prev;
    LG_HedralExprNode             node;
} LG_HedralBuilderNode;

typedef struct
LG_HedralBuilder {
    LG_HedralBuilderNode  *ir_tail;
    uint32_t               next_symbol_id;
} LG_HedralBuilder;

typedef struct
LG_HedralAddressTriple {
    LG_HedralSymbol y, x0, x1;
} LG_HedralAddressTriple;

LG_HedralSymbol
lg_hbuilder_append(
    LG_Context *ctx,
    LG_HedralBuilder *builder,
    LG_HedralOpcode opcode,
    LG_HedralOperands operands
) {
    LG_HedralBuilderNode *node = lg_arena_alloc_struct(&ctx->arena, LG_HedralBuilderNode);
    if (node == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory appending to hedral expr"));
        return lg_nil(LG_HedralSymbol);
    }

    builder->next_symbol_id++;
    LG_HedralSymbol y = (LG_HedralSymbol){
        .id = builder->next_symbol_id,
        .type = lg_hedral_op_get_return_type(opcode),
    };

    node->node.opcode = opcode;
    node->node.y = y;
    node->node.as = operands;

    // TODO: make lg_sll_prepend
    if (builder->ir_tail != NULL) {
        node->prev = builder->ir_tail;
    }
    builder->ir_tail = node;

    return y;
}

#define lg_hexpr_node_assert_return_type(node) \
    lg_assert( \
        node != NULL && \
        LG_HEDRAL_OPERATION_TABLE[(node)->opcode].return_type == (node)->y.type \
    )\

lg_force_inline LG_HedralSymbol
lg_hbuilder_add(LG_Context *ctx, LG_HedralBuilder *builder, LG_HedralSymbol x0, LG_HedralSymbol x1) {
    lg_assert(x0.type == LG_HedralType_Scalar);
    lg_assert(x1.type == LG_HedralType_Scalar);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, builder, LG_HedralOpcode_Add, (LG_HedralOperands){
        .add = {x0, x1}
    });
    lg_hexpr_node_assert_return_type(&builder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol
lg_hbuilder_multiply(LG_Context *ctx, LG_HedralBuilder *builder, LG_HedralSymbol x0, LG_HedralSymbol x1) {
    lg_assert(x0.type == LG_HedralType_Scalar);
    lg_assert(x1.type == LG_HedralType_Scalar);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, builder, LG_HedralOpcode_Multiply, (LG_HedralOperands){
        .add = {x0, x1}
    });
    lg_hexpr_node_assert_return_type(&builder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol
lg_hbuilder_begin_iter_domain(
    LG_Context *ctx,
    LG_HedralBuilder *builder,
    LG_Polyhedron *domain
) {
    LG_HedralSymbol y = lg_hbuilder_append(ctx, builder, LG_HedralOpcode_BeginIterationDomain, (LG_HedralOperands){
        .begin_iteration_domain = domain 
    });
    lg_hexpr_node_assert_return_type(&builder->ir_tail->node);
    return y;
}

lg_force_inline void
lg_hbuilder_end_iter_domain(LG_Context *ctx, LG_HedralBuilder *hbuilder) {
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_EndIterationDomain, lg_nil(LG_HedralOperands));
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    lg_assert(y.type == LG_HedralType_Unit);
    return;
}

lg_force_inline LG_HedralSymbol 
lg_hbuilder_construct_atran(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_AffineTransform *atran) {
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_ConstructAffineTransform, (LG_HedralOperands){
        .construct_affine_transform = atran,
    });
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol 
lg_hbuilder_apply_atran(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_HedralSymbol atran, LG_HedralSymbol coord) {
    lg_assert(atran.type == LG_HedralType_AffineTransform);
    lg_assert(coord.type == LG_HedralType_Coordinate);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_ApplyAffineTransform, (LG_HedralOperands){
        .apply_affine_transform = {atran, coord}
    });
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol 
lg_hbuilder_construct_affine_addr_op(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_AffineTransform *addr_op) {
    lg_assert(lg_atran_is_valid_address_operator(addr_op));
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_ConstructAffineAddressOperator, (LG_HedralOperands){
        .construct_affine_address_operator = addr_op,
    });
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol 
lg_hbuilder_apply_affine_addr_op(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_HedralSymbol addr_op, LG_HedralSymbol coord) {
    lg_assert(addr_op.type == LG_HedralType_AffineAddressOperator);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_ApplyAffineAddressOperator, (LG_HedralOperands){
        .apply_affine_address_operator = {addr_op, coord},
    });
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return y;
}

lg_force_inline LG_HedralSymbol
lg_hbuilder_access(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_HedralSymbol addr) {
    lg_assert(addr.type == LG_HedralType_Address);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_Access, (LG_HedralOperands){
        .access = addr,
    });
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return y;
}

lg_force_inline void
lg_hbuilder_yield_assign(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_HedralSymbol addr, LG_HedralSymbol scalar) {
    lg_assert(addr.type == LG_HedralType_Address);
    lg_assert(scalar.type == LG_HedralType_Scalar);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_YieldAssign, (LG_HedralOperands){
        .yield_accumulate = {addr, scalar}
    });
    (void)y;
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return;
}

lg_force_inline void
lg_hbuilder_yield_accumulate(LG_Context *ctx, LG_HedralBuilder *hbuilder, LG_HedralSymbol addr, LG_HedralSymbol scalar) {
    lg_assert(addr.type == LG_HedralType_Address);
    lg_assert(scalar.type == LG_HedralType_Scalar);
    LG_HedralSymbol y = lg_hbuilder_append(ctx, hbuilder, LG_HedralOpcode_YieldAccumulate, (LG_HedralOperands){
        .yield_accumulate = {addr, scalar}
    });
    (void)y;
    lg_hexpr_node_assert_return_type(&hbuilder->ir_tail->node);
    return;
}

LG_HedralAddressTriple
lg_hbuilder_template_get_binop_addrs(
    LG_Context *ctx,
    LG_HedralBuilder *hbuilder,

    LG_Polyhedron *iter_domain,

    LG_AffineTransform *y_atran,
    LG_AffineTransform *x0_atran,
    LG_AffineTransform *x1_atran,

    LG_AffineTransform *y_addr_op,
    LG_AffineTransform *x0_addr_op,
    LG_AffineTransform *x1_addr_op
) {
    LG_HedralSymbol induction_vector = lg_hbuilder_begin_iter_domain(ctx, hbuilder, iter_domain);

    LG_HedralSymbol y_atran_s = lg_hbuilder_construct_atran(ctx, hbuilder, y_atran);
    LG_HedralSymbol y_addr_op_s = lg_hbuilder_construct_affine_addr_op(ctx, hbuilder, y_addr_op);
    LG_HedralSymbol y_coord_s = lg_hbuilder_apply_atran(ctx, hbuilder, y_atran_s, induction_vector);
    LG_HedralSymbol y_addr_s = lg_hbuilder_apply_affine_addr_op(ctx, hbuilder, y_addr_op_s, y_coord_s);

    LG_HedralSymbol x0_atran_s = lg_hbuilder_construct_atran(ctx, hbuilder, x0_atran);
    LG_HedralSymbol x0_addr_op_s = lg_hbuilder_construct_affine_addr_op(ctx, hbuilder, x0_addr_op);
    LG_HedralSymbol x0_coord_s = lg_hbuilder_apply_atran(ctx, hbuilder, x0_atran_s, induction_vector);
    LG_HedralSymbol x0_addr_s = lg_hbuilder_apply_affine_addr_op(ctx, hbuilder, x0_addr_op_s, x0_coord_s);

    LG_HedralSymbol x1_atran_s = lg_hbuilder_construct_atran(ctx, hbuilder, x1_atran);
    LG_HedralSymbol x1_addr_op_s = lg_hbuilder_construct_affine_addr_op(ctx, hbuilder, x1_addr_op);
    LG_HedralSymbol x1_coord_s = lg_hbuilder_apply_atran(ctx, hbuilder, x1_atran_s, induction_vector);
    LG_HedralSymbol x1_addr_s = lg_hbuilder_apply_affine_addr_op(ctx, hbuilder, x1_addr_op_s, x1_coord_s);

    return (LG_HedralAddressTriple){
        .y = y_addr_s,
        .x0 = x0_addr_s,
        .x1 = x1_addr_s,
    };
}

// TODO: this is just pasted from above. this should be factored out
LG_StatusKind
lg_hbuilder_finish(
    LG_Context *ctx,
    LG_HedralBuilder *hbuilder,
    LG_Allocator *artifact_allocator,
    LG_HedralExpr *out_hexpr
) {
    if (hbuilder->next_symbol_id == 0 || hbuilder->ir_tail == NULL) {
        lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit("attempted to finish empty builder"));
        return LG_StatusKind_InvalidArgument;
    }

    ////////////////////////////////////////////////////////////////////////////////
    // ~~ find the length of the expr & validate two invariants ~~
    // 1) no symbol id > the current ctx.next_symbol_id
    //    - we do this b/c lets us use raw integer indices for the symbol table
    // 2) there are no cycles in the sll

    size_t metadata_size = 0;
    size_t hexpr_len = 0;
    const size_t max_symbol_id = hbuilder->next_symbol_id;
    {
        bool is_first_iteration = true;
        LG_HedralBuilderNode *tortoise = hbuilder->ir_tail;
        LG_HedralBuilderNode *hare = hbuilder->ir_tail;
        for (; tortoise != NULL; tortoise = tortoise->prev, hexpr_len++) {
            if (hare != NULL) {
                hare = hare->prev;
            }
            if (hare != NULL) {
                hare = hare->prev;
            }

            lg_assert(is_first_iteration || hare == NULL || tortoise != hare);
            lg_assert(tortoise->node.y.id <= max_symbol_id);

            is_first_iteration = false;

            switch (tortoise->node.opcode) {
            case LG_HedralOpcode_BeginIterationDomain:
                metadata_size += sizeof(LG_Polyhedron);
                break;

            case LG_HedralOpcode_ConstructAffineTransform:
            case LG_HedralOpcode_ConstructAffineAddressOperator:
                metadata_size += sizeof(LG_AffineTransform);
                break;

            default:;
            }
        }
    }


    LG_HedralExprNode *hexpr_nodes = (LG_HedralExprNode*)lg_alloc_zero(artifact_allocator, hexpr_len * sizeof(LG_HedralExprNode));
    if (hexpr_nodes == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory allocating hedral expr nodes"));
        return LG_StatusKind_OutOfMemory;
    }

    LG_HedralBuilderNode *iter_node = hbuilder->ir_tail;
    for (
        size_t i_rev = 0;
        iter_node != NULL;
        iter_node = iter_node->prev, i_rev++
    ) { 
        lg_assert(i_rev < hexpr_len);
        hexpr_nodes[hexpr_len - 1 - i_rev] = iter_node->node;
    }


    /////////////////////////////////////////////////////////////
    /// ~~ copy/compact metadata and retarget pointers ~~
    
    size_t metadata_current_offset = 0;
    uint8_t *metadata = lg_alloc_zero(artifact_allocator, metadata_size);
    if (metadata == NULL) {
        lg_report_error(ctx, LG_StatusKind_OutOfMemory, lg_str8_lit("ran out of memory allocating hedral expr metadata"));
        return LG_StatusKind_OutOfMemory;
    }

    for (size_t i = 0; i < hexpr_len; i++) {
        switch (hexpr_nodes[i].opcode) {
        case LG_HedralOpcode_BeginIterationDomain: {
            lg_deep_copy_struct(
                LG_Polyhedron,
                metadata + metadata_current_offset,
                &hexpr_nodes[i].as.begin_iteration_domain
            );
            metadata_current_offset += sizeof(LG_Polyhedron);
            break;
        }

        // both union members use the same type
        case LG_HedralOpcode_ConstructAffineTransform:
        case LG_HedralOpcode_ConstructAffineAddressOperator: {
            lg_deep_copy_struct(
                LG_AffineTransform,
                metadata + metadata_current_offset,
                &hexpr_nodes[i].as.construct_affine_address_operator
            );
            metadata_current_offset += sizeof(LG_AffineTransform);
            break;
        }

        default:;
        }
    }

    lg_assert(metadata_current_offset == metadata_size);


    ////////////////////////////////////////
    // ~~ fin ~~

    out_hexpr->max_symbol_id = max_symbol_id;
    out_hexpr->len = hexpr_len;
    out_hexpr->nodes = hexpr_nodes;
    out_hexpr->metadata_size = metadata_size;
    out_hexpr->metadata_block = metadata;

    return LG_StatusKind_OK;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// passes for lowering a logical expression into a physical one
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
            // Param declarations must be the first section of the lexpr,
            // while sink declarations must be at the end.
            if (lexpr->nodes[i].opcode == LG_LogicalOpcode_Param && !params_begin) {
                if (i != 0) {
                    lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                        "found the first param declaration at node index %{i64}\n"
                        "note: param declarations must be the first thing in the expr"
                    ), i);
                    status = LG_StatusKind_InvalidArgument;
                    goto out;
                }
                params_begin = true;
            } else if (lexpr->nodes[i].opcode == LG_LogicalOpcode_Param && params_end) {
                lg_report_error(ctx, LG_StatusKind_InvalidArgument, lg_str8_lit(
                    "found a param declaration after a non-param operation at node index %{i64}\n"
                    "note: param declarations must happen one after another"
                ), i);
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (lexpr->nodes[i].opcode != LG_LogicalOpcode_Param && params_begin) {
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
            const uint32_t new_id = lexpr->nodes[i].y.id;
            const bool found_x0 = lg_bv_get_bit(&seen_set, lexpr->nodes[i].x0.id);
            const bool found_x1 = lg_bv_get_bit(&seen_set, lexpr->nodes[i].x1.id);
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
                    lexpr->nodes[i].y.id, i
                );
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x0)  {
                lg_report_error(
                    ctx, 
                    LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("use of unknown symbol with id as operand x0 %{i64} at node index %{i64}"),
                    lexpr->nodes[i].x0.id, i
                );
                status = LG_StatusKind_InvalidArgument;
                goto out;
            } else if (!found_x1 && lg_opcode_is_binary(lexpr->nodes[i].opcode)) {
                lg_report_error(
                    ctx, 
                    LG_StatusKind_InvalidArgument, 
                    lg_str8_lit("use of unknown symbol with id as operand x1 %{i64} at node index %{i64}"),
                    lexpr->nodes[i].x1.id, i
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
lg_infer_y_shape(LG_Context *ctx, const LG_LogicalExprNode *node, LG_LogicalShape *inout_shapes) {
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
        break;
    }

    case LG_LogicalOpcode_Sink:
    case LG_LogicalOpcode_Hadamard:
    case LG_LogicalOpcode_MSELoss:
    case LG_LogicalOpcode_CrossEntropyLoss:
    case LG_LogicalOpcode_ReLU:
    case LG_LogicalOpcode_StableSoftmax:
    case LG_LogicalOpcode_Sigmoid:
    case LG_LogicalOpcode_LN:
        lg_unreachable("TODO");
    }

    return LG_StatusKind_OK;
}

LG_StatusKind
lg_lexpr_node_to_hbuilder(
    LG_Context *ctx,
    LG_HedralBuilder *hbuilder,
    LG_LogicalExprNode *lnode,
    const LG_LogicalShape *shapes,
    LG_AffineTransform *const *addr_ops
) {
    LG_StatusKind status = LG_StatusKind_OK;

    switch (lnode->opcode) {
    case LG_LogicalOpcode_Add:
    case LG_LogicalOpcode_Sub: {
        LG_MappedSpace iter_space = {0};
        LG_StatusKind status = lg_create_broadcasted_iteration_space(
            &ctx->arena,
            &shapes[lnode->y.id], &shapes[lnode->x0.id], &shapes[lnode->x1.id],
            &iter_space
        );
        if (status != LG_StatusKind_OK) {
            goto oom;
        }

        LG_AffineTransform *const y_addr_op = addr_ops[lnode->y.id];
        LG_AffineTransform *const x0_addr_op = addr_ops[lnode->x0.id];
        LG_AffineTransform *const x1_addr_op = addr_ops[lnode->x1.id];
        
        LG_HedralAddressTriple addrs = lg_hbuilder_template_get_binop_addrs(
            ctx, hbuilder,
            iter_space.iteration_domain,
            iter_space.to_y_coords, iter_space.to_x0_coords, iter_space.to_x1_coords, 
            y_addr_op, x0_addr_op, x1_addr_op
        );

        LG_HedralSymbol x0_value = lg_hbuilder_access(ctx, hbuilder, addrs.x0);
        LG_HedralSymbol x1_value = lg_hbuilder_access(ctx, hbuilder, addrs.x1);
        LG_HedralSymbol y_value = lg_hbuilder_add(ctx, hbuilder, x0_value, x1_value);

        lg_hbuilder_yield_assign(ctx, hbuilder, addrs.y, y_value);

        break;
    }

    case LG_LogicalOpcode_Contract: {
        LG_MappedSpace iter_space = {0};
        LG_StatusKind status = lg_create_contracted_iteration_space(
            &ctx->arena,
            &shapes[lnode->y.id], &shapes[lnode->x0.id], &shapes[lnode->x1.id],
            lnode->meta_as.contract.n_batch_axes,
            &iter_space
        );
        if (status != LG_StatusKind_OK) {
            goto oom;
        }

        LG_AffineTransform *const y_addr_op = addr_ops[lnode->y.id];
        LG_AffineTransform *const x0_addr_op = addr_ops[lnode->x0.id];
        LG_AffineTransform *const x1_addr_op = addr_ops[lnode->x1.id];

        LG_HedralAddressTriple addrs = lg_hbuilder_template_get_binop_addrs(
            ctx, hbuilder,
            iter_space.iteration_domain,
            iter_space.to_y_coords, iter_space.to_x0_coords, iter_space.to_x1_coords, 
            y_addr_op, x0_addr_op, x1_addr_op
        );

        LG_HedralSymbol x0_value = lg_hbuilder_access(ctx, hbuilder, addrs.x0);
        LG_HedralSymbol x1_value = lg_hbuilder_access(ctx, hbuilder, addrs.x1);
        LG_HedralSymbol y_value = lg_hbuilder_multiply(ctx, hbuilder, x0_value, x1_value);

        lg_hbuilder_yield_accumulate(ctx, hbuilder, addrs.y, y_value);

        break;
    }
        
    case LG_LogicalOpcode_Sink:
    case LG_LogicalOpcode_Param:
    case LG_LogicalOpcode_ReLU:
    case LG_LogicalOpcode_StableSoftmax:
    case LG_LogicalOpcode_Sigmoid:
    case LG_LogicalOpcode_LN:
    case LG_LogicalOpcode_Hadamard:
    case LG_LogicalOpcode_MSELoss:
    case LG_LogicalOpcode_CrossEntropyLoss:
        lg_unreachable("TODO"); 
        break;
    }

    return LG_StatusKind_OK;

oom:
    // this should not fail for any other reason;
    // we literally just generated the shapes
    lg_assert(status == LG_StatusKind_OutOfMemory);
    lg_report_error(ctx, status, lg_str8_lit("ran out of memory allocating a scratch structure"));
    return status;
}

LG_StatusKind
lg_lower_lexpr(
    LG_Context *ctx,
    LG_Allocator *artifact_allocator,
    LG_LogicalExpr *lexpr,
    LG_LogicalExprLoweringFlags flags,
    LG_HedralExpr *out_hexpr
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
        status = lg_infer_y_shape(ctx, &lexpr->nodes[i], shapes);
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
    for (size_t i = 0; i < lexpr->len; i++) {
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
    // ~~ build the hexpr ~~

    LG_HedralBuilder hbuilder = {0};
    for (size_t i = 0; i < lexpr->len; i++) {
        status = lg_lexpr_node_to_hbuilder(ctx, &hbuilder, &lexpr->nodes[i], shapes, addr_ops);
        if (status != LG_StatusKind_OK) {
            lg_assert(status == LG_StatusKind_OutOfMemory);
            lg_report_error(ctx, status, lg_str8_lit("ran out of memory allocating a scratch structure"));
            goto out;
        }
    }

    status = lg_hbuilder_finish(ctx, &hbuilder, artifact_allocator, out_hexpr);
    if (status != LG_StatusKind_OK) {
        lg_assert(status == LG_StatusKind_OutOfMemory);
        goto out;
    }


    /////////////////////////////////////////////////////////////////
    // ~~ fin ~~

out:
    lg_pop_scope(&ctx->arena, scope);
    return status;
}
