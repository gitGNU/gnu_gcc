// { dg-xfail-if "ICE" { "*-*-*" } { "-fpph-map=pph.map" } }
// { dg-bogus "a0tmplfuncinln_g.h:17:23: internal compiler error: in instantiate_decl" "" { xfail *-*-* } 0 }
// { dg-excess-errors "Template list problems" }

#include "x0tmplfuncinln1.h"
#include "x0tmplfuncinln2.h"
#include "a0tmplfuncinln_u.h"
