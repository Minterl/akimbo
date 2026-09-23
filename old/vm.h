#ifndef AK_VM_H_
#define AK_VM_H_

#include <akimbo/internal/core.h>
#include <akimbo/internal/alloc.h>
#include <akimbo/internal/vm_symtab.h>
#include <akimbo/internal/strings.h>
#include <akimbo/internal/map.h>

#define AK_MAX_ERR_LEN 1024

/// Discriminator for an operation.
///
/// The integer representations of opcodes are not designed
/// to be stable and should not be serialized.
typedef enum
AK_LogicalOpcode {

    //////////////////////////////////
    // ~~ Unary Operations ~~

#   define AK_FIRST_UNARY_OP AK_LogicalOpcode_Sink
    AK_LogicalOpcode_Sink,

    // Constructive operations create new symbols,
    // while non-constructive ones do not.
#   define AK_FIRST_CONSTRUCTIVE_OP AK_LogicalOpcode_Source

    AK_LogicalOpcode_Param,
    /// Element-wise ReLU
    AK_LogicalOpcode_ReLU,
    /// Element-wise stable softmax
    AK_LogicalOpcode_StableSoftmax,
    /// Element-wise sigmoid
    AK_LogicalOpcode_Sigmoid,
    /// Element-wise natural log
    AK_LogicalOpcode_LN,

#   define AK_LAST_UNARY_OP AK_LogicalOpcode_LN


    //////////////////////////////////
    // ~~ Binary Operations ~~

#   define AK_FIRST_BINARY_OP AK_LogicalOpcode_Add

    /// Element-wise tensor addition
    AK_LogicalOpcode_Add,
    /// Element-wise tensor subtraction
    AK_LogicalOpcode_Sub,
    /// Generalized tensor contraction i.e
    /// dot-product over strided dimensions.
    /// Is generalizable to N-rank tensors.
    AK_LogicalOpcode_Contract,
    /// Hadamard product
    AK_LogicalOpcode_Hadamard,
    /// Mean Squared Error loss
    AK_LogicalOpcode_MSELoss,
    /// Cross-entropy loss
    AK_LogicalOpcode_CrossEntropyLoss,

#   define AK_LAST_BINARY_OP AK_LogicalOpcode_CrossEntropyLoss
#   define AK_LAST_CONSTRUCTIVE_OP AK_LAST_BINARY_OP
} AK_LogicalOpcode;

_Static_assert(AK_LAST_UNARY_OP + 1 == AK_FIRST_BINARY_OP, "opcodes must be contigugous");

#define ak_opcode_creates_symbol(op) ((AK_FIRST_CONSTRUCTIVE_OP <= (op)) && ((op) <= AK_LAST_CONSTRUCTIVE_OP))
#define ak_opcode_is_unary(op) ((AK_FIRST_UNARY_OP <= (op)) && ((op) <= AK_LAST_UNARY_OP))
#define ak_opcode_is_binary(op) ((AK_FIRST_BINARY_OP <= (op)) && ((op) <= AK_LAST_BINARY_OP))

// typedef union
// AK_ExprNodeMeta {
//     struct {
//         size_t n_contracted_axes;
//         size_t n_batch_axes;
//     } contract;
// } AK_ExprNodeMeta;

/// An IR node in an expr.
// typedef struct
// AK_ExprNode {
//     AK_LogicalOpcode            opcode;

//     AK_LogicalSymbol            y_logical;
//     AK_StridedDesc       y_physical;
//     uint32_t             y_buf_id;
//     size_t               y_offset;

//     AK_LogicalSymbol            x0_logical;
//     AK_StridedDesc       x0_physical;
//     uint32_t             x0_buf_id;
//     size_t               x0_offset;

//     AK_LogicalSymbol            x1_logical;
//     AK_StridedDesc       x1_physical;
//     uint32_t             x1_buf_id;
//     size_t               x1_offset;

//     AK_ExprNodeMeta      meta_as;
// } AK_ExprNode;

typedef struct
AK_BufferTableEntry {
    size_t size_in_bytes;
} AK_BufferTableEntry;

typedef struct
AK_BufferTable {
    AK_Map map;
    AK_BufferTableEntry *entries;
} AK_BufferTable;

/// The intermediate representation of a program.
///
/// As of right now, the exprs themselves do not support any control flow.
// typedef struct
// AK_OldExpr {
//     size_t          nodes_cap;
//     size_t          nodes_len;
//     AK_ExprNode    *nodes ak_check_bounds(nodes_len);

//     AK_BufferTable  buftab;
// } AK_Expr;

typedef struct
AK_CompilationContext {
    AK_Expr              *expr;
    AK_Arena             *arena;
    AK_LogicalSymbolTable        symtab;

    AK_StatusKind         last_status;
    size_t                err_msg_len;
    uint8_t               err_msg_backing_buf[AK_MAX_ERR_LEN];

    uint32_t              next_symbol_id;
} AK_CompilationContext;

void
ak_print_compilation_error(AK_CompilationContext *ctx, AK_Writer *writer);

AK_LogicalSymbol
ak_declare_source(
    AK_CompilationContext *ctx,
    AK_StridedDesc physical_desc,
    uint32_t buf_id
);

void
ak_declare_sink(AK_CompilationContext *ctx, AK_LogicalSymbol sym);

AK_StatusKind
ak_get_sink_location(
    uint32_t *ak_nullable out_buf_id,
    size_t *ak_nullable out_offset,
    AK_StridedDesc *ak_nullable out_desc,
    AK_LogicalSymbol sym,
    AK_Expr *expr
);

AK_StatusKind
ak_buftab_insert(AK_BufferTable *buftab, uint32_t id);

AK_StatusKind
ak_buftab_get(AK_BufferTable *buftab, AK_BufferTableEntry *ak_nullable out_entry, uint32_t id);

AK_StatusKind
ak_buftab_update(AK_BufferTable *buftab, uint32_t id, AK_BufferTableEntry new_entry);

/// Gets the last physical location of the tensor `x` and populates
/// its `data` pointer if found.
///
/// This does not guarantee that the value will actually exist at the end
/// of execution.
///
/// If you want to make sure that is the case, append a NOP using `lgvm_Nop` to
/// the end of the expr.
AK_StatusKind
ak_append_nop(AK_Expr *expr, AK_LogicalSymbol x);

AK_LogicalSymbol
ak_append_add(AK_CompilationContext *ctx, const AK_LogicalSymbol x0, const AK_LogicalSymbol x1);
// AK_StatusKind
// ak_append_sub(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol x0, const AK_LogicalSymbol x1);
AK_LogicalSymbol
ak_append_contract(
    AK_CompilationContext *ctx,
    AK_LogicalSymbol x0,
    AK_LogicalSymbol x1,
    size_t n_contracted_axes,
    size_t n_batch_axes
);
// AK_StatusKind
// ak_append_hadamard(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol x0, const AK_LogicalSymbol x1);

// AK_StatusKind
// ak_append_mse_loss(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol x0, const AK_LogicalSymbol x1);
// AK_StatusKind
// ak_append_cross_entropy_loss(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol x0, const AK_LogicalSymbol x1);

// AK_StatusKind
// ak_append_relu(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol in);
// AK_StatusKind
// ak_append_stable_softmax(AK_Expr *expr, const AK_LogicalSymbol y, const AK_LogicalSymbol in);
// AK_StatusKind
// ak_append_sigmoid(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol in);
// AK_StatusKind
// ak_append_ln(AK_Expr *expr, AK_LogicalSymbol y, const AK_LogicalSymbol in);

/// "Compiles" an expr.
AK_StatusKind
ak_compile_expr(
    AK_CompilationContext *ctx,
    size_t mem_align
);

/// Allocate the memory necessary for an expr with the given capacities,
/// and assign offsets into the buffer for each field.
AK_StatusKind
ak_alloc_expr(
    AK_Allocator alloc,
    AK_Expr *expr,
    size_t nodes_cap,
    size_t bufmap_cap
);

/// Frees the memory required for an expr.
void
ak_free_expr(AK_Allocator allocator, AK_Expr *expr);

#endif // AK_VM_H_
