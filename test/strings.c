// TODO: once the rest of the lib builds, just use the main header
#include "akimbo/internal/core.h"
#include <akimbo/internal/strings.h>

#ifndef TEST_IMPLEMENTATION
#define TEST_IMPLEMENTATION
#endif // TEST_IMPLEMENTATION
#include "testing.h"

#include <stdio.h>

size_t write_to_stdout(void *ctx, const ak_str8 str) {
    (void)ctx;
    size_t i = 0;
    for (; i < str.len; i++) {
        putchar(str.p[i]);
    }
    return i;
}

static AK_Writer stdout_writer = {
    .write = write_to_stdout,
};

test_status test_printf() {
    test_assert(ak_printf(&stdout_writer, ak_str8_lit("asdf: %{i64}\n"), 13) == AK_StatusKind_OK, "int failed to print");
    test_assert(ak_printf(&stdout_writer, ak_str8_lit("asdf: %{str}\n"), ak_str8_lit("asdfasdf")) == AK_StatusKind_OK, "string failed to print");
    test_assert(ak_printf(&stdout_writer, ak_str8_lit("asdf: %{status}\n"), AK_StatusKind_Overflow) == AK_StatusKind_OK, "status failed to print");
    test_assert(ak_printf(&stdout_writer, ak_str8_lit("asdf: %{cstr}\n"), "asdfasdf") == AK_StatusKind_OK, "cstring failed to print");
    test_assert(ak_printf(&stdout_writer, ak_str8_lit("asdf: %{cstr\n"), "asdfasdf") == AK_StatusKind_InvalidArgument, "unterminated fmtspec didn't fail");
    test_assert(
        ak_printf(&stdout_writer, ak_str8_lit("asdf: %{i64} %{cstr}\n"), 14, "asdfasdf") == AK_StatusKind_OK,
        "mixed failed to print"
    );
    return TEST_STATUS_OK;
}

int main() {
    test_run(printf);
    return 0;
}

#include <akimbo/internal/strings.c>
#include <akimbo/internal/debug.c>
