/* { dg-do run { target { x32 || lp64 } } } */
/* { dg-require-effective-target avx } */
/* { dg-options "-O2 -mavx" } */

#define CHECK_H "avx-check.h"
#define TEST avx_test

#include "sse4_1-pinsrq.c"
