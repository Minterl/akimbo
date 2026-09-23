#ifndef AKIMBO_H_
#define AKIMBO_H_

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <akimbo/internal/expr.h>

#ifdef AKIMBO_IMPLEMENTATION
#undef AKIMBO_IMPLEMENTATION
#   include <akimbo/internal/base.c>
#   include <akimbo/internal/context.c>
#   include <akimbo/internal/expr.c>
#   include <akimbo/internal/linalg.c>
#endif // AKIMBO_IMPLEMENTATION

#ifdef __cplusplus
}
#endif // __cplusplus
       
#endif // AKIMBO_H_
