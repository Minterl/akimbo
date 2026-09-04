#ifndef LG_EXPR_H_
#define LG_EXPR_H_

#include <libgrad/internal/base.h>
#include <libgrad/internal/context.h>
#include <libgrad/internal/linalg.h>

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
LG_LogicalOpcode {

    //////////////////////////////////
    // ~~ Unary Operations ~~

#   define LG_FIRST_UNARY_OP LG_LogicalOpcode_Sink
    LG_LogicalOpcode_Sink,

    // Constructive operations create new symbols,
    // while non-constructive ones do not.
#   define LG_FIRST_CONSTRUCTIVE_OP LG_LogicalOpcode_Source

    LG_LogicalOpcode_Param,
    /// Element-wise ReLU
    LG_LogicalOpcode_ReLU,
    /// Element-wise stable softmax
    LG_LogicalOpcode_StableSoftmax,
    /// Element-wise sigmoid
    LG_LogicalOpcode_Sigmoid,
    /// Element-wise natural log
    LG_LogicalOpcode_LN,

#   define LG_LAST_UNARY_OP LG_LogicalOpcode_LN


    //////////////////////////////////
    // ~~ Binary Operations ~~

#   define LG_FIRST_BINARY_OP LG_LogicalOpcode_Add

    /// Element-wise tensor addition
    LG_LogicalOpcode_Add,
    /// Element-wise tensor subtraction
    LG_LogicalOpcode_Sub,
    /// Generalized tensor contraction i.e
    /// dot-product over strided dimensions.
    /// Is generalizable to N-rank tensors.
    LG_LogicalOpcode_Contract,
    /// Hadamard product
    LG_LogicalOpcode_Hadamard,
    /// Mean Squared Error loss
    LG_LogicalOpcode_MSELoss,
    /// Cross-entropy loss
    LG_LogicalOpcode_CrossEntropyLoss,

#   define LG_LAST_BINARY_OP LG_LogicalOpcode_CrossEntropyLoss
#   define LG_LAST_CONSTRUCTIVE_OP LG_LAST_BINARY_OP
} LG_LogicalOpcode;

lg_static_assert(LG_LAST_UNARY_OP + 1 == LG_FIRST_BINARY_OP);

#define lg_opcode_creates_symbol(op) ((LG_FIRST_CONSTRUCTIVE_OP <= (op)) && ((op) <= LG_LAST_CONSTRUCTIVE_OP))
#define lg_opcode_is_unary(op) ((LG_FIRST_UNARY_OP <= (op)) && ((op) <= LG_LAST_UNARY_OP))
#define lg_opcode_is_binary(op) ((LG_FIRST_BINARY_OP <= (op)) && ((op) <= LG_LAST_BINARY_OP))

typedef uint32_t 
LG_LogicalSymbolFlags;
enum {
    LG_LogicalSymbolFlag_Pin = UINT32_C(0x1),
};

typedef struct
LG_LogicalSymbol {
    uint32_t id;
} LG_LogicalSymbol;

typedef union
LG_LogicalMeta {
    struct {
        size_t n_contracted_axes;
        size_t n_batch_axes;
    } contract;

    struct {
        LG_LogicalShape y_shape; 
    } param;
} LG_LogicalMeta;

typedef struct
LG_LogicalExprNode {
    LG_LogicalOpcode        opcode;

    LG_LogicalSymbol        y;
    LG_LogicalSymbol        x0;
    LG_LogicalSymbol        x1;

    LG_LogicalSymbolFlags   y_flags;
    LG_LogicalMeta  meta_as;
} LG_LogicalExprNode;

typedef struct 
LG_LogicalExpr {
    uint32_t            max_symbol_id;
    size_t              len;
    LG_LogicalExprNode *nodes lg_check_bounds(len);
} LG_LogicalExpr;


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// expr builder
///
////////////////////////////////////////////////////////////////////////////////

typedef struct
LG_LogicalBuilderNode {
    /// Nodes are stored in reverse-chronological order, so we only have a
    /// prev pointer
    struct LG_LogicalBuilderNode *prev;
    LG_LogicalExprNode node;
} LG_LogicalBuilderNode;

typedef struct
LG_LogicalBuilder {
    LG_LogicalBuilderNode  *ir_tail;
    uint32_t                next_symbol_id;
} LG_LogicalBuilder;

typedef uint32_t 
LG_LogicalExprLoweringFlags;
enum {
    LG_LogicalExprLoweringFlag_NoStructuralInvariantValidation = (0x1),
};

LG_StatusKind
lg_lbuilder_finish(
    LG_Context *ctx,
    LG_LogicalBuilder *builder,
    LG_Allocator *artifact_allocator,
    LG_LogicalExpr *out_lexpr
);

void
lg_lexpr_destroy(LG_LogicalExpr *lexpr, LG_Allocator *artifact_allocator);

LG_LogicalSymbol
lg_param(LG_Context *ctx, LG_LogicalBuilder *lexpr, LG_LogicalShape shape);

void
lg_pin(LG_Context *ctx, LG_LogicalBuilder *lexpr, LG_LogicalSymbol sym);

void
lg_force_layout(LG_Context *ctx, LG_LogicalBuilder *lexpr, LG_LogicalSymbol sym, LG_LayoutKind layout);

LG_LogicalSymbol
lg_add(LG_Context *ctx, LG_LogicalBuilder *lexpr, LG_LogicalSymbol x0, LG_LogicalSymbol x1);

LG_LogicalSymbol
lg_contract(
    LG_Context *ctx,
    LG_LogicalBuilder *lexpr,
    LG_LogicalSymbol x0,
    LG_LogicalSymbol x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);

#endif // LG_EXPR_H_
