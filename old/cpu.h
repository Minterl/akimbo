#ifndef AK_CPU_H_
#define AK_CPU_H_

#include <akimbo/internal/vm.h>
#include <akimbo/internal/core.h>

AK_StatusKind ak_rt_cpu_exec_expr(AK_Expr expr);

/// Tensors must be sorted & broadcasted
void ak_rt_cpu_add(
    const AK_StridedDesc y_desc, ak_scalar *restrict y,
    const AK_StridedDesc x0_desc, const ak_scalar *restrict x0,
    const AK_StridedDesc x1_desc, const ak_scalar *restrict x1
);
void ak_rt_cpu_contract(
    const AK_StridedDesc y_desc, ak_scalar *restrict y,
    const AK_StridedDesc x0_desc, const ak_scalar *restrict x0,
    const AK_StridedDesc x1_desc, const ak_scalar *restrict x1
);

#endif // AK_CPU_H_


#ifdef AK_CPU_IMPLEMENTATION
#undef AK_CPU_IMPLEMENTATION

// TODO: attach runtime context
// AK_StatusKind AK_RT_CPU_ExecExpr(AK_Expr expr) {
//     for (size_t i = 0; i < expr.len; i++) {
//         switch (expr.nodes[i].opcode) {
//         case AK_OPCODE_NOP:
//             break;
//         case AK_OPCODE_ADD:
//             AK_RT_CPU_Add(
//                 expr.nodes[i].y_physical, expr.nodes[i].y_data,
//                 expr.nodes[i].x0_physical, expr.nodes[i].x0.data,
//                 expr.nodes[i].x1_physical, expr.nodes[i].x1.data
//             );
//             break;
//         case AK_OPCODE_CONTRACT:
//             AK_RT_CPU_Contract(
//                 expr.nodes[i].y.desc, expr.nodes[i].y.data,
//                 expr.nodes[i].x0.desc, expr.nodes[i].x0.data,
//                 expr.nodes[i].x1.desc, expr.nodes[i].x1.data
//             );
//             break;
//         case AK_OPCODE_SUB:
//         case AK_OPCODE_HADAMARD:
//         case AK_OPCODE_LOSS_MSE:
//         case AK_OPCODE_LOSS_CROSS_ENTROPY:
//         case AK_OPCODE_RELU:
//         case AK_OPCODE_STABLE_SOFTMAX:
//         case AK_OPCODE_SIGMOID:
//         case AK_OPCODE_LN:
//         default:
//             return AK_STATUS_UNSUPPORTED_OPCODE;
//         }
//     }

//     return AK_StatusKind_OK;
// }

void AK_RT_CPU_Add(
    const AK_StridedDesc y_desc, ak_scalar *restrict y,
    const AK_StridedDesc x0_desc, const ak_scalar *restrict x0,
    const AK_StridedDesc x1_desc, const ak_scalar *restrict x1
) {
    AK_NDIter iter = {
        .descs = {y_desc, x0_desc, x1_desc},
        .n_tracked_dims = y_desc.rank,
    };

    do {
        const size_t y_idx = iter.indices[0];
        const size_t x0_idx = iter.indices[1];
        const size_t x1_idx = iter.indices[2];

        y[y_idx] += x0[x0_idx] + x1[x1_idx];
   } while (ak_nditer_increment(&iter, y_desc.rank - 1));
}

void AK_RT_CPU_Contract(
    const AK_StridedDesc y_desc, ak_scalar *restrict y,
    const AK_StridedDesc x0_desc, const ak_scalar *restrict x0,
    const AK_StridedDesc x1_desc, const ak_scalar *restrict x1
) {
    AK_NDIter iter = {
        .descs = {y_desc, x0_desc, x1_desc},
        .n_tracked_dims = y_desc.rank,
    };

    do {
        const size_t y_idx = iter.indices[0];
        const size_t x0_idx = iter.indices[1];
        const size_t x1_idx = iter.indices[2];

        y[y_idx] += x0[x0_idx] * x1[x1_idx];
    } while (ak_nditer_increment(&iter, y_desc.rank - 1));
}

#endif // AK_CPU_IMPLEMENTATION
