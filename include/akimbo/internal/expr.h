#ifndef AK_EXPR_H_
#define AK_EXPR_H_

#include <akimbo/internal/base.h>
#include <akimbo/internal/context.h>
#include <akimbo/internal/linalg.h>

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// common expr types etc.
///
////////////////////////////////////////////////////////////////////////////////

/// Discriminator for an operation.
///
/// The integer representations of opcodes are not designed
/// to be stable and should not be serialized.
typedef enum
AK_LogicalOpcode {
#   define AK_LOPCODE_FIRST_CTOR AK_LogicalOpcode_Param

    AK_LogicalOpcode_Param,

#   define AK_LOPCODE_FIRST_UNARY_OP AK_LogicalOpcode_ReLU

    AK_LogicalOpcode_ReLU,
    AK_LogicalOpcode_LN,

#   define AK_LOPCODE_FIRST_BINARY_OP AK_LogicalOpcode_Add

    AK_LogicalOpcode_Add,
    AK_LogicalOpcode_Sub,
    AK_LogicalOpcode_Contract,
    AK_LogicalOpcode_Hadamard,

} AK_LogicalOpcode;

#define ak_lopcode_is_ctor(op) ((AK_LOPCODE_FIRST_CTOR <= (op)) && ((op) < AK_LOPCODE_FIRST_UNARY_OP))
#define ak_lopcode_is_unary(op) ((AK_LOPCODE_FIRST_UNARY_OP <= (op)) && ((op) < AK_LOPCODE_FIRST_BINARY_OP))
#define ak_lopcode_is_binary(op) (AK_LOPCODE_FIRST_BINARY_OP <= (op))

typedef uint32_t 
AK_LogicalSymbolFlags;
enum {
    AK_LogicalSymbolFlag_Pin = UINT32_C(0x1),
};

typedef struct
AK_LogicalSymbol {
    uint32_t id;
} AK_LogicalSymbol;

typedef union
AK_LogicalMeta {
    struct {
        size_t n_contracted_axes;
        size_t n_batch_axes;
    } contract;

    struct {
        AK_LogicalShape y_shape; 
    } param;
} AK_LogicalMeta;

typedef struct
AK_LogicalInst {
    AK_LogicalOpcode       opcode;

    AK_LogicalSymbol       y;
    AK_LogicalSymbol       x0;
    AK_LogicalSymbol       x1;

    AK_LogicalSymbolFlags  y_flags;
    AK_LogicalMeta         meta_as;
} AK_LogicalInst;

typedef struct 
AK_LogicalExpr {
    uint32_t            max_symbol_id;
    size_t              len;
    AK_LogicalInst     *insts ak_check_bounds(len);
} AK_LogicalExpr;


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// expr builder
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
AK_LogicalBuilderNode {
    /// Nodes are stored in reverse-chronological order, so we only have a
    /// prev pointer
    struct AK_LogicalBuilderNode *prev;
    AK_LogicalInst node;
} AK_LogicalBuilderNode;

typedef struct
AK_LogicalBuilder {
    AK_LogicalBuilderNode  *ir_tail;
    uint32_t                next_symbol_id;
} AK_LogicalBuilder;

typedef uint32_t 
AK_LogicalExprLoweringFlags;
enum {
    AK_LogicalExprLoweringFlag_NoStructuralInvariantValidation = (0x1),
};

AK_StatusKind
ak_lbuilder_finish(
    AK_Context *ctx,
    AK_LogicalBuilder *builder,
    AK_Allocator artifact_allocator,
    AK_LogicalExpr *out_lexpr
);

void
ak_lexpr_destroy(AK_LogicalExpr *lexpr, AK_Allocator artifact_allocator);

AK_LogicalSymbol
ak_param(AK_Context *ctx, AK_LogicalBuilder *lexpr, AK_LogicalShape shape);

void
ak_pin(AK_Context *ctx, AK_LogicalBuilder *lexpr, AK_LogicalSymbol sym);

void
ak_force_layout(AK_Context *ctx, AK_LogicalBuilder *lexpr, AK_LogicalSymbol sym, AK_LayoutKind layout);

AK_LogicalSymbol
ak_add(AK_Context *ctx, AK_LogicalBuilder *lexpr, AK_LogicalSymbol x0, AK_LogicalSymbol x1);

AK_LogicalSymbol
ak_contract(
    AK_Context *ctx,
    AK_LogicalBuilder *lexpr,
    AK_LogicalSymbol x0,
    AK_LogicalSymbol x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);

#endif // AK_EXPR_H_
