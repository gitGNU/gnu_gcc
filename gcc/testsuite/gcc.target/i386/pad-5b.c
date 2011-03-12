/* { dg-do compile { target { x32 || lp64 } } } */
/* { dg-options "-O2 -fomit-frame-pointer -march=atom" } */
/* { dg-final { scan-assembler-times "nop" 4 } } */
/* { dg-final { scan-assembler-not "rep" } } */

int
foo (int x, int y, int z)
{
   return x + y + z;
}
