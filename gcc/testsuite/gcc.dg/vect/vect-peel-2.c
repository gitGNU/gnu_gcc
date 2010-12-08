/* { dg-require-effective-target vect_int } */
/* { dg-add-options quad_vectors } */

#include <stdarg.h>
#include "tree-vect.h"

#define N 128

/* unaligned store.  */

int ib[N+7];

__attribute__ ((noinline))
int main1 ()
{
  int i;
  int ia[N+1];

  /* The store is aligned and the loads are misaligned with the same 
     misalignment. Cost model is disabled. If misaligned stores are supported,
     we peel according to the loads to align them.  */
  for (i = 0; i <= N; i++)
    {
      ia[i] = ib[i+2] + ib[i+6];
    }

  /* check results:  */
  for (i = 1; i <= N; i++)
    {
      if (ia[i] != ib[i+2] + ib[i+6])
        abort ();
    }

  return 0;
}

int main (void)
{ 
  int i;

  check_vect ();

  for (i = 0; i <= N+6; i++)
    ib[i] = i;

  return main1 ();
}

/* { dg-final { scan-tree-dump-times "vectorized 1 loops" 2 "vect" { xfail  vect_no_align } } } */
/* { dg-final { scan-tree-dump-times "Vectorizing an unaligned access" 1 "vect" { target vect_element_align  } } } */
/* { dg-final { scan-tree-dump-times "Vectorizing an unaligned access" 2 "vect" { xfail { vect_no_align || vect_element_align } } } } */
/* { dg-final { scan-tree-dump-times "Alignment of access forced using peeling" 1 "vect" { target vect_element_align } } } */
/* { dg-final { cleanup-tree-dump "vect" } } */
