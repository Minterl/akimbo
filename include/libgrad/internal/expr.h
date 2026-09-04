#ifndef LG_EXPR_H_
#define LG_EXPR_H_

#include <libgrad/internal/base.h>
#include <libgrad/internal/linalg.h>

#define LG_MAX_ERR_LEN 1024

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// common expr types etc.
///
////////////////////////////////////////////////////////////////////////////////

// TODO: this should be moved to another file, maybe
typedef struct
LG_Context {
    LG_Arena         arena;

    LG_StatusKind    last_status;
    size_t           err_msg_len;
    uint8_t          err_msg_backing_buf[LG_MAX_ERR_LEN];
} LG_Context;

void
lg_perror(LG_Context *ctx, LG_Writer *writer);

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
LG_ExprNodeMeta {
    struct {
        size_t n_contracted_axes;
        size_t n_batch_axes;
    } contract;

    struct {
        LG_LogicalShape y_shape; 
    } param;
} LG_ExprNodeMeta;

typedef struct
LG_LogicalExprNode {
    LG_LogicalOpcode        opcode;

    LG_LogicalSymbol        y;
    LG_LogicalSymbol        x0;
    LG_LogicalSymbol        x1;

    LG_LogicalSymbolFlags   y_flags;
    LG_ExprNodeMeta  meta_as;
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


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
///
/// hedral expr stuff
///
////////////////////////////////////////////////////////////////////////////////
///
/// The hedral language deals with both index arithmetic and scalar arithmetic simulatneously.
/// The program is composed of blocks (like the one below).
///
/// It is strongly (semantically) typed.
///
/// Instead of standard structured control flow, hedral defines three core primitives for the
/// "whoami" part of a kernel:
/// 1) The iteration domain defines loop boundaries as constants. `induction_vector` 
///    is the current coodinate in the iteration domain.
/// 2) Affine transforms map the iteration domain to the index domain.
/// 3) Address operators map the index domain to the address domain.
///
/// The "whatdoido" part of a kernel is defined using SSA scalar arithmetic. Side effects
/// (such as accumulation semantics) are represented via the Yield instruction,
/// which has Assign and Accumulate variants.
///
/// This makes codegen really easy, because all you have to do is inline the operations
/// and the constants, and choose which loops you're going to schedule in parallel and 
/// which loops you're going to serialize (hint: accumulation semantics almost always
/// mean serialization).
///
/// Below is a very simple block describing what may be an einsum/reduction/contraction.
///
/// *note*: the BeginIterationDomain and EndIterationDomain ops have been replaced 
/// with the ForIterationDomain notation for clarity
///
/// ForIterationDomain induction_vector: Coordinate = (
///     0 <= j <= m,
///     0 <= k <= n,
///     0 <= l <= o,
///     ...
/// ) {
///     %y_transform:  AffineTransform = ConstructAffineTransform(...);
///     %y_coord:      Coordinate      = ApplyAffineTransform(induction_vector, y_transform);
///     %y_addr:       Address         = ApplyAddressOperator(y_coord, y_transform);
///
///     %x0_transform: AffineTransform = ConstructAffineTransform(...);
///     %x0_coord:     Coordinate      = ApplyAffineTransform(induction_vector, x0_transform);
///     %x0_addr:      Address         = ApplyAddressOperator(x0_coord, x0_transform);
///
///     %x1_transform: AffineTransform = ConstructAffineTransform(...);
///     %x1_coord:     Coordinate      = ApplyAffineTransform(induction_vector, x1_transform);
///     %x1_addr:      Address         = ApplyAffineAddressOperator(x1_coord, x1_transform);
///
///     %x0: Scalar = Access(x0_addr);
///     %x1: Scalar = Access(x1_addr);
///     %y:  Scalar = Multiply(%x0, %x1);
///      
///     YieldAccumulate(y_addr, y);
/// }

typedef uint32_t 
LG_LogicalExprLoweringFlags;
enum {
    LG_LogicalExprLoweringFlag_NoStructuralInvariantValidation = (0x1),
};

typedef uint8_t 
LG_HedralType;
enum {
    LG_HedralType_Unit,
    LG_HedralType_Address,
    LG_HedralType_Index,
    LG_HedralType_Scalar,
    LG_HedralType_Coordinate,
    LG_HedralType_Domain,
    LG_HedralType_AffineTransform,
    LG_HedralType_AffineAddressOperator,
};

////////////////////////////////////////////////////////////////////////////////////////////////////
//       Opcode                             Return Type                           Operands
#define LG_HEDRAL_OPERATIONS \
    LG_X(NOP,                               LG_HedralType_Unit,                   LG_HedralType_Unit), \
    LG_X(Add,                               LG_HedralType_Scalar,                 LG_HedralType_Scalar, LG_HedralType_Scalar), \
    LG_X(Multiply,                          LG_HedralType_Scalar,                 LG_HedralType_Scalar, LG_HedralType_Scalar), \
    LG_X(BeginIterationDomain,              LG_HedralType_Coordinate,             LG_HedralType_Domain), \
    LG_X(EndIterationDomain,                LG_HedralType_Unit,                   LG_HedralType_Unit), \
    LG_X(ConstructAffineTransform,          LG_HedralType_AffineTransform,        LG_HedralType_AffineTransform), \
    LG_X(ConstructAffineAddressOperator,    LG_HedralType_AffineAddressOperator,  LG_HedralType_AffineAddressOperator), \
    LG_X(ApplyAffineTransform,              LG_HedralType_Coordinate,             LG_HedralType_AffineTransform, LG_HedralType_Coordinate), \
    LG_X(ApplyAffineAddressOperator,        LG_HedralType_Address,                LG_HedralType_AffineAddressOperator, LG_HedralType_Address), \
    LG_X(Access,                            LG_HedralType_Scalar,                 LG_HedralType_Address), \
    LG_X(YieldAssign,                       LG_HedralType_Unit,                   LG_HedralType_Address, LG_HedralType_Scalar), \
    LG_X(YieldAccumulate,                   LG_HedralType_Unit,                   LG_HedralType_Address, LG_HedralType_Scalar),
      
typedef uint8_t
LG_HedralOpcode;
enum {
#   define LG_X(opcode, ...) LG_HedralOpcode_##opcode
    LG_HEDRAL_OPERATIONS
#   undef LG_X
};

const struct {
    LG_HedralType   return_type;
    LG_HedralType   operand_types[2];
    const lg_str8   string_name;
} LG_HEDRAL_OPERATION_TABLE[] = {
#   define LG_X(opcode, return_type_, ...) [LG_HedralOpcode_##opcode] = { \
        .return_type   = (return_type_), \
        .operand_types = {__VA_ARGS__}, \
        .string_name   = lg_str8_lit(#opcode), \
    }
    LG_HEDRAL_OPERATIONS
#   undef LG_X
};

typedef struct
LG_HedralSymbol {
    uint32_t id;
    LG_HedralType type;
} LG_HedralSymbol;

// TODO: use anonymous structs with names instead
typedef union 
LG_HedralOperands {
    LG_HedralSymbol      add[2];
    LG_HedralSymbol      multiply[2];

    LG_Polyhedron       *begin_iteration_domain;
    LG_HedralSymbol      end_iteration_domain;
    LG_AffineTransform  *construct_affine_transform;
    LG_AffineTransform  *construct_affine_address_operator;

    LG_HedralSymbol      apply_affine_transform[2];
    LG_HedralSymbol      apply_affine_address_operator[2];

    LG_HedralSymbol      access;

    LG_HedralSymbol      yield_assign[2];
    LG_HedralSymbol      yield_accumulate[2];
} LG_HedralOperands;

typedef struct
LG_HedralExprNode {
    LG_HedralOpcode   opcode;
    LG_HedralSymbol   y;
    LG_HedralOperands as;
} LG_HedralExprNode;

typedef struct
LG_HedralExpr {
    uint32_t            max_symbol_id;

    size_t              len;
    LG_HedralExprNode  *nodes lg_check_bounds(len);

    size_t              metadata_size;
    uint8_t            *metadata_block;
} LG_HedralExpr;

#define lg_hedral_op_get_return_type(opcode) (LG_HEDRAL_OPERATION_TABLE[(opcode)].return_type)

#endif // LG_EXPR_H_
