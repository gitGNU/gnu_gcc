/* { dg-options { -nostartfiles below100.o -Tbelow100.ld -O2 } } */

typedef struct
{
  unsigned char b0:1;
  unsigned char b1:1;
  unsigned char b2:1;
  unsigned char b3:1;
  unsigned char b4:1;
  unsigned char b5:1;
  unsigned char b6:1;
  unsigned char b7:1;
} BitField;

char acDummy[0xf0] __attribute__ ((__BELOW100__));
BitField B100 __attribute__ ((__BELOW100__)) =
{
1, 1, 0, 1, 0, 0, 1, 1};
unsigned char *p = (unsigned char *) &B100;

void
Do (void)
{
  B100.b7 = 0;
}

int
main (void)
{
  Do ();
  return (*p == 0x4b) ? 0 : 1;
}

/* { dg-final { scan-file "21_clr_b100b_bitfield_7.s" "clr1 B100,#7" } } */

