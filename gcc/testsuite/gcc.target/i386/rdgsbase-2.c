/* { dg-do compile { target { x32 || lp64 } } } */
/* { dg-options "-O2 -mfsgsbase" } */
/* { dg-final { scan-assembler "rdgsbase\[ \t]+(%|)rax" } } */

#include <immintrin.h>

unsigned long long
read_gs_base64 (void)
{
  return _readgsbase_u64 ();
}
