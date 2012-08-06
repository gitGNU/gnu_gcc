/* { dg-options { -nostartfiles below100.o -Tbelow100.ld -O2 } } */

char acDummy[0xf0] __attribute__ ((__BELOW100__));
unsigned short B100A __attribute__ ((__BELOW100__)) = 0x1234;
unsigned short *pA = &B100A;
unsigned short B100B __attribute__ ((__BELOW100__)) = 0xedcb;
unsigned short *pB = &B100B;

char *
Do (void)
{
  if (!(B100A & 0x8000))
    {
      if (!(B100B & 0x8000))
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
  return Do ()[0] == 'F';
}

/* { dg-final { scan-file "17_if0_b100w_bit_f.s" "b\[np\] B100A\\+1,#7," } } */
/* { dg-final { scan-file "17_if0_b100w_bit_f.s" "b\[np\] B100B\\+1,#7," } } */
