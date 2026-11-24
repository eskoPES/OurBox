/* Tiny assertion helpers for JackBox loader test titles. These titles are
 * Windows-ABI x86-64 programs built with clang (MSVC-style codegen for TLS and
 * SEH). They validate the loader and runtime; they are NOT evidence of Xbox
 * console compatibility. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
static int jb_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); jb_failures++; } } while (0)
#define DONE() do { printf(jb_failures ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", jb_failures); fflush(stdout); return jb_failures ? 1 : 0; } while (0)
