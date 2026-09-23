#ifndef AK_TESTING_H_
#define AK_TESTING_H_

#include <stdio.h>

typedef enum test_status {
    TEST_STATUS_OK,
    TEST_STATUS_FAIL,
} test_status;

#define test_run(func) do { \
   if ((test_##func)() != TEST_STATUS_OK) { \
       fprintf(stderr, "\x1b[31m[TEST FAILED]\x1b[0m (%s)\n", #func); \
   } else { \
       printf("\x1b[32m[TEST PASSED]\x1b[0m (%s)\n", #func); \
   } \
} while (0)

#define test_assert(cond, ...) do { \
    if (!(cond)) {\
        __test_assert_fail(#cond, __FILE__, __LINE__, __VA_ARGS__); \
        return TEST_STATUS_FAIL; \
    } \
} while(0)

#define test_assert_array_eq(want, got, len, fmtspec) do { \
    for (int __TEST_MACRO_ITER__ = 0; __TEST_MACRO_ITER__ < len; __TEST_MACRO_ITER__++) { \
        if ((want)[__TEST_MACRO_ITER__] != (got)[__TEST_MACRO_ITER__]) { \
            __test_assert_fail( \
                #want" == "#got, \
                    __FILE__, \
                    __LINE__, \
                    "at index %d: wanted "fmtspec", got "fmtspec, \
                    __TEST_MACRO_ITER__, \
                    (want)[__TEST_MACRO_ITER__], \
                    (got)[__TEST_MACRO_ITER__] \
            ); \
            return TEST_STATUS_FAIL; \
        } \
    } \
} while(0)

void __test_assert_fail(const char *cond_str, const char *file, int line, const char *fmt, ...);

#endif // AK_TESTING_H_

#ifdef TEST_IMPLEMENTATION
#undef TEST_IMPLEMENTATION

#include <stdio.h>
#include <stdarg.h>

void __test_assert_fail(const char *cond_str, const char *file, int line, const char *fmt, ...) {
    fprintf(stderr, "\x1b[31m[ASSERTION FAILED]\x1b[0m (%s) at %s:%d -- ", cond_str, file, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#endif // TEST_IMPLEMENTATION
