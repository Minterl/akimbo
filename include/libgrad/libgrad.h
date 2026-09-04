#ifndef LIBGRAD_H_
#define LIBGRAD_H_

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <libgrad/internal/expr.h>

#ifdef LIBGRAD_IMPLEMENTATION
#undef LIBGRAD_IMPLEMENTATION
#   include <libgrad/internal/base.c>
#   include <libgrad/internal/context.c>
#   include <libgrad/internal/expr.c>
#   include <libgrad/internal/linalg.c>
#endif // LIBGRAD_IMPLEMENTATION

#ifdef __cplusplus
}
#endif // __cplusplus
       
#endif // LIBGRAD_H_
