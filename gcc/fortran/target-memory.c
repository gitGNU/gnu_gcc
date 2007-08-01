/* Simulate storage of variables into target memory.
   Copyright (C) 2007
   Free Software Foundation, Inc.
   Contributed by Paul Thomas and Brooks Moses

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "flags.h"
#include "machmode.h"
#include "tree.h"
#include "gfortran.h"
#include "arith.h"
#include "trans.h"
#include "trans-const.h"
#include "trans-types.h"
#include "target-memory.h"

/* --------------------------------------------------------------- */ 
/* Calculate the size of an expression.  */

static size_t
size_array (gfc_expr *e)
{
  mpz_t array_size;
  size_t elt_size = gfc_target_expr_size (e->value.constructor->expr);

  gfc_array_size (e, &array_size);
  return (size_t)mpz_get_ui (array_size) * elt_size;
}

static size_t
size_integer (int kind)
{
  return GET_MODE_SIZE (TYPE_MODE (gfc_get_int_type (kind)));;
}


static size_t
size_float (int kind)
{
  return GET_MODE_SIZE (TYPE_MODE (gfc_get_real_type (kind)));;
}


static size_t
size_complex (int kind)
{
  return 2 * size_float (kind);
}


static size_t
size_logical (int kind)
{
  return GET_MODE_SIZE (TYPE_MODE (gfc_get_logical_type (kind)));;
}


static size_t
size_character (int length)
{
  return length;
}


size_t
gfc_target_expr_size (gfc_expr *e)
{
  tree type;

  gcc_assert (e != NULL);

  if (e->expr_type == EXPR_ARRAY)
    return size_array (e);

  switch (e->ts.type)
    {
    case BT_INTEGER:
      return size_integer (e->ts.kind);
    case BT_REAL:
      return size_float (e->ts.kind);
    case BT_COMPLEX:
      return size_complex (e->ts.kind);
    case BT_LOGICAL:
      return size_logical (e->ts.kind);
    case BT_CHARACTER:
      return size_character (e->value.character.length);
    case BT_HOLLERITH:
      return e->representation.length;
    case BT_DERIVED:
      type = gfc_typenode_for_spec (&e->ts);
      return int_size_in_bytes (type);
    default:
      gfc_internal_error ("Invalid expression in gfc_target_expr_size.");
      return 0;
    }
}


/* The encode_* functions export a value into a buffer, and 
   return the number of bytes of the buffer that have been
   used.  */

static int
encode_array (gfc_expr *expr, unsigned char *buffer, size_t buffer_size)
{
  mpz_t array_size;
  int i;
  int ptr = 0;

  gfc_array_size (expr, &array_size);
  for (i = 0; i < (int)mpz_get_ui (array_size); i++)
    {
      ptr += gfc_target_encode_expr (gfc_get_array_element (expr, i),
				     &buffer[ptr], buffer_size - ptr);
    }

  mpz_clear (array_size);
  return ptr;
}


static int
encode_integer (int kind, mpz_t integer, unsigned char *buffer,
		size_t buffer_size)
{
  return native_encode_expr (gfc_conv_mpz_to_tree (integer, kind),
			     buffer, buffer_size);
}


static int
encode_float (int kind, mpfr_t real, unsigned char *buffer, size_t buffer_size)
{
  return native_encode_expr (gfc_conv_mpfr_to_tree (real, kind), buffer,
			     buffer_size);
}


static int
encode_complex (int kind, mpfr_t real, mpfr_t imaginary, unsigned char *buffer,
		size_t buffer_size)
{
  int size;
  size = encode_float (kind, real, &buffer[0], buffer_size);
  size += encode_float (kind, imaginary, &buffer[size], buffer_size - size);
  return size;
}


static int
encode_logical (int kind, int logical, unsigned char *buffer, size_t buffer_size)
{
  return native_encode_expr (build_int_cst (gfc_get_logical_type (kind),
					    logical),
			     buffer, buffer_size);
}


static int
encode_character (int length, char *string, unsigned char *buffer,
		  size_t buffer_size)
{
  gcc_assert (buffer_size >= size_character (length));
  memcpy (buffer, string, length);
  return length;
}


static int
encode_derived (gfc_expr *source, unsigned char *buffer, size_t buffer_size)
{
  gfc_constructor *ctr;
  gfc_component *cmp;
  int ptr;
  tree type;

  type = gfc_typenode_for_spec (&source->ts);

  ctr = source->value.constructor;
  cmp = source->ts.derived->components;
  for (;ctr; ctr = ctr->next, cmp = cmp->next)
    {
      gcc_assert (cmp);
      if (!ctr->expr)
	continue;
      ptr = TREE_INT_CST_LOW(DECL_FIELD_OFFSET(cmp->backend_decl))
	    + TREE_INT_CST_LOW(DECL_FIELD_BIT_OFFSET(cmp->backend_decl))/8;
      gfc_target_encode_expr (ctr->expr, &buffer[ptr],
			      buffer_size - ptr);
    }

  return int_size_in_bytes (type);
}


/* Write a constant expression in binary form to a buffer.  */
int
gfc_target_encode_expr (gfc_expr *source, unsigned char *buffer,
			size_t buffer_size)
{
  if (source == NULL)
    return 0;

  if (source->expr_type == EXPR_ARRAY)
    return encode_array (source, buffer, buffer_size);

  gcc_assert (source->expr_type == EXPR_CONSTANT
	      || source->expr_type == EXPR_STRUCTURE);

  /* If we already have a target-memory representation, we use that rather 
     than recreating one.  */
  if (source->representation.string)
    {
      memcpy (buffer, source->representation.string,
	      source->representation.length);
      return source->representation.length;
    }

  switch (source->ts.type)
    {
    case BT_INTEGER:
      return encode_integer (source->ts.kind, source->value.integer, buffer,
			     buffer_size);
    case BT_REAL:
      return encode_float (source->ts.kind, source->value.real, buffer,
			   buffer_size);
    case BT_COMPLEX:
      return encode_complex (source->ts.kind, source->value.complex.r,
			     source->value.complex.i, buffer, buffer_size);
    case BT_LOGICAL:
      return encode_logical (source->ts.kind, source->value.logical, buffer,
			     buffer_size);
    case BT_CHARACTER:
      return encode_character (source->value.character.length, 
			       source->value.character.string, buffer,
			       buffer_size);
    case BT_DERIVED:
      return encode_derived (source, buffer, buffer_size);
    default:
      gfc_internal_error ("Invalid expression in gfc_target_encode_expr.");
      return 0;
    }
}


static int
interpret_array (unsigned char *buffer, size_t buffer_size, gfc_expr *result)
{
  int array_size = 1;
  int i;
  int ptr = 0;
  gfc_constructor *head = NULL, *tail = NULL;

  /* Calculate array size from its shape and rank.  */
  gcc_assert (result->rank > 0 && result->shape);

  for (i = 0; i < result->rank; i++)
    array_size *= (int)mpz_get_ui (result->shape[i]);

  /* Iterate over array elements, producing constructors.  */
  for (i = 0; i < array_size; i++)
    {
      if (head == NULL)
	head = tail = gfc_get_constructor ();
      else
	{
	  tail->next = gfc_get_constructor ();
	  tail = tail->next;
	}

      tail->where = result->where;
      tail->expr = gfc_constant_result (result->ts.type,
					  result->ts.kind, &result->where);
      tail->expr->ts = result->ts;

      if (tail->expr->ts.type == BT_CHARACTER)
	tail->expr->value.character.length = result->value.character.length;

      ptr += gfc_target_interpret_expr (&buffer[ptr], buffer_size - ptr,
					tail->expr);
    }
  result->value.constructor = head;

  return ptr;
}


int
gfc_interpret_integer (int kind, unsigned char *buffer, size_t buffer_size,
		   mpz_t integer)
{
  mpz_init (integer);
  gfc_conv_tree_to_mpz (integer,
			native_interpret_expr (gfc_get_int_type (kind),
					       buffer, buffer_size));
  return size_integer (kind);
}


int
gfc_interpret_float (int kind, unsigned char *buffer, size_t buffer_size,
		 mpfr_t real)
{
  mpfr_init (real);
  gfc_conv_tree_to_mpfr (real,
			 native_interpret_expr (gfc_get_real_type (kind),
						buffer, buffer_size));

  return size_float (kind);
}


int
gfc_interpret_complex (int kind, unsigned char *buffer, size_t buffer_size,
		   mpfr_t real, mpfr_t imaginary)
{
  int size;
  size = gfc_interpret_float (kind, &buffer[0], buffer_size, real);
  size += gfc_interpret_float (kind, &buffer[size], buffer_size - size, imaginary);
  return size;
}


int
gfc_interpret_logical (int kind, unsigned char *buffer, size_t buffer_size,
		   int *logical)
{
  tree t = native_interpret_expr (gfc_get_logical_type (kind), buffer,
				  buffer_size);
  *logical = double_int_zero_p (tree_to_double_int (t))
	     ? 0 : 1;
  return size_logical (kind);
}


int
gfc_interpret_character (unsigned char *buffer, size_t buffer_size, gfc_expr *result)
{
  if (result->ts.cl && result->ts.cl->length)
    result->value.character.length =
      (int)mpz_get_ui (result->ts.cl->length->value.integer);

  gcc_assert (buffer_size >= size_character (result->value.character.length));
  result->value.character.string =
    gfc_getmem (result->value.character.length + 1);
  memcpy (result->value.character.string, buffer,
	  result->value.character.length);
  result->value.character.string [result->value.character.length] = '\0';

  return result->value.character.length;
}


int
gfc_interpret_derived (unsigned char *buffer, size_t buffer_size, gfc_expr *result)
{
  gfc_component *cmp;
  gfc_constructor *head = NULL, *tail = NULL;
  int ptr;
  tree type;

  /* The attributes of the derived type need to be bolted to the floor.  */
  result->expr_type = EXPR_STRUCTURE;

  type = gfc_typenode_for_spec (&result->ts);
  cmp = result->ts.derived->components;

  /* Run through the derived type components.  */
  for (;cmp; cmp = cmp->next)
    {
      if (head == NULL)
	head = tail = gfc_get_constructor ();
      else
	{
	  tail->next = gfc_get_constructor ();
	  tail = tail->next;
	}

      /* The constructor points to the component.  */
      tail->n.component = cmp;

      tail->expr = gfc_constant_result (cmp->ts.type, cmp->ts.kind,
					&result->where);
      tail->expr->ts = cmp->ts;

      /* Copy shape, if needed.  */
      if (cmp->as && cmp->as->rank)
	{
	  int n;

	  tail->expr->expr_type = EXPR_ARRAY;
	  tail->expr->rank = cmp->as->rank;

	  tail->expr->shape = gfc_get_shape (tail->expr->rank);
	  for (n = 0; n < tail->expr->rank; n++)
	     {
	       mpz_init_set_ui (tail->expr->shape[n], 1);
	       mpz_add (tail->expr->shape[n], tail->expr->shape[n],
			cmp->as->upper[n]->value.integer);
	       mpz_sub (tail->expr->shape[n], tail->expr->shape[n],
			cmp->as->lower[n]->value.integer);
	     }
	}

      ptr = TREE_INT_CST_LOW (DECL_FIELD_OFFSET (cmp->backend_decl));
      gfc_target_interpret_expr (&buffer[ptr], buffer_size - ptr,
				 tail->expr);

      result->value.constructor = head;
    }
    
  return int_size_in_bytes (type);
}


/* Read a binary buffer to a constant expression.  */
int
gfc_target_interpret_expr (unsigned char *buffer, size_t buffer_size,
			   gfc_expr *result)
{
  if (result->expr_type == EXPR_ARRAY)
    return interpret_array (buffer, buffer_size, result);

  switch (result->ts.type)
    {
    case BT_INTEGER:
      result->representation.length = 
        gfc_interpret_integer (result->ts.kind, buffer, buffer_size,
			       result->value.integer);
      break;

    case BT_REAL:
      result->representation.length = 
        gfc_interpret_float (result->ts.kind, buffer, buffer_size,
    			     result->value.real);
      break;

    case BT_COMPLEX:
      result->representation.length = 
        gfc_interpret_complex (result->ts.kind, buffer, buffer_size,
			       result->value.complex.r,
			       result->value.complex.i);
      break;

    case BT_LOGICAL:
      result->representation.length = 
        gfc_interpret_logical (result->ts.kind, buffer, buffer_size,
			       &result->value.logical);
      break;

    case BT_CHARACTER:
      result->representation.length = 
        gfc_interpret_character (buffer, buffer_size, result);
      break;

    case BT_DERIVED:
      result->representation.length = 
        gfc_interpret_derived (buffer, buffer_size, result);
      break;

    default:
      gfc_internal_error ("Invalid expression in gfc_target_interpret_expr.");
      break;
    }

  if (result->ts.type == BT_CHARACTER)
    result->representation.string = result->value.character.string;
  else
    {
      result->representation.string =
        gfc_getmem (result->representation.length + 1);
      memcpy (result->representation.string, buffer,
	      result->representation.length);
      result->representation.string[result->representation.length] = '\0';
    }

  return result->representation.length;
}


/* --------------------------------------------------------------- */ 
/* Two functions used by trans-common.c to write overlapping
   equivalence initializers to a buffer.  This is added to the union
   and the original initializers freed.  */


/* Writes the values of a constant expression to a char buffer. If another
   unequal initializer has already been written to the buffer, this is an
   error.  */

static size_t
expr_to_char (gfc_expr *e, unsigned char *data, unsigned char *chk, size_t len)
{
  int i;
  int ptr;
  gfc_constructor *ctr;
  gfc_component *cmp;
  unsigned char *buffer;

  if (e == NULL)
    return 0;

  /* Take a derived type, one component at a time, using the offsets from the backend
     declaration.  */
  if (e->ts.type == BT_DERIVED)
    {
      ctr = e->value.constructor;
      cmp = e->ts.derived->components;
      for (;ctr; ctr = ctr->next, cmp = cmp->next)
	{
	  gcc_assert (cmp && cmp->backend_decl);
	  if (!ctr->expr)
	    continue;
	    ptr = TREE_INT_CST_LOW(DECL_FIELD_OFFSET(cmp->backend_decl))
			+ TREE_INT_CST_LOW(DECL_FIELD_BIT_OFFSET(cmp->backend_decl))/8;
	  expr_to_char (ctr->expr, &data[ptr], &chk[ptr], len);
	}
      return len;
    }

  /* Otherwise, use the target-memory machinery to write a bitwise image, appropriate
     to the target, in a buffer and check off the initialized part of the buffer.  */
  len = gfc_target_expr_size (e);
  buffer = (unsigned char*)alloca (len);
  len = gfc_target_encode_expr (e, buffer, len);

    for (i = 0; i < (int)len; i++)
    {
      if (chk[i] && (buffer[i] != data[i]))
	{
	  gfc_error ("Overlapping unequal initializers in EQUIVALENCE "
		     "at %L", &e->where);
	  return 0;
	}
      chk[i] = 0xFF;
    }

  memcpy (data, buffer, len);
  return len;
}


/* Writes the values from the equivalence initializers to a char* array
   that will be written to the constructor to make the initializer for
   the union declaration.  */

size_t
gfc_merge_initializers (gfc_typespec ts, gfc_expr *e, unsigned char *data,
			unsigned char *chk, size_t length)
{
  size_t len = 0;
  gfc_constructor * c;

  switch (e->expr_type)
    {
    case EXPR_CONSTANT:
    case EXPR_STRUCTURE:
      len = expr_to_char (e, &data[0], &chk[0], length);

      break;

    case EXPR_ARRAY:
      for (c = e->value.constructor; c; c = c->next)
	{
	  size_t elt_size = gfc_target_expr_size (c->expr);

	  if (c->n.offset)
	    len = elt_size * (size_t)mpz_get_si (c->n.offset);

	  len = len + gfc_merge_initializers (ts, c->expr, &data[len],
					      &chk[len], length - len);
	}
      break;

    default:
      return 0;
    }

  return len;
}
