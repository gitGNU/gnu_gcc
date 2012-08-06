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

#define SFRA (*((volatile BitField*)0x7f14))
unsigned char *pA = (unsigned char *) 0x7f14;
#define SFRB (*((volatile BitField*)0x7f10))
unsigned char *pB = (unsigned char *) 0x7f10;

char *
Do (void)
{
  if (SFRA.b0)
    {
      if (SFRB.b0)
	return "Fail";
      else
	return "Success";
    }
  else
    return "Fail";
}

int
main (void)
{
  *pA = 0xcb;
  *pB = 0x34;
  return Do ()[0] == 'F';
}

/* { dg-final { scan-file "22_if1_sfrb_bitfield_0.s" "b\[np\] " } } */

