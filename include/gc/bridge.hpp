#pragma once

#define _Noreturn [[noreturn]]
#define _Static_assert static_assert
#define _Alignof alignof

extern "C" {
#include <stella/gc.h>
#include <stella/gc_stats.h>
}

#undef _Noreturn
#undef _Static_assert
#undef _Alignof
