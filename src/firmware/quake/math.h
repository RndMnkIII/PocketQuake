/* math.h shim for PocketQuake - redirects to our libc */
#ifndef _MATH_H_SHIM
#define _MATH_H_SHIM

#include "../libc/libc.h"

#define M_PI        3.14159265358979323846f
#define M_PI_2      1.57079632679489661923f
#define HUGE_VAL    (1.0/0.0)

#endif
