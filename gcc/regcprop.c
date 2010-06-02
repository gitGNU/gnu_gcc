/* Copy propagation on hard registers for the GNU compiler.
   Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009,
   2010  Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tm_p.h"
#include "insn-config.h"
#include "regs.h"
#include "addresses.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "reload.h"
#include "output.h"
#include "function.h"
#include "recog.h"
#include "flags.h"
#include "toplev.h"
#include "obstack.h"
#include "timevar.h"
#include "tree-pass.h"
#include "df.h"

/* The following code does forward propagation of hard register copies.
   The object is to eliminate as many dependencies as possible, so that
   we have the most scheduling freedom.  As a side effect, we also clean
   up some silly register allocation decisions made by reload.  This
   code may be obsoleted by a new register allocator.  */

/* DEBUG_INSNs aren't changed right away, as doing so might extend the
   lifetime of a register and get the DEBUG_INSN subsequently reset.
   So they are queued instead, and updated only when the register is
   used in some subsequent real insn before it is set.  */
struct queued_debug_insn_change
{
  struct queued_debug_insn_change *next;
  rtx insn;
  rtx *loc;
  rtx new_rtx;
};

/* For each register, we have a list of registers that contain the same
   value.  The OLDEST_REGNO field points to the head of the list, and
   the NEXT_REGNO field runs through the list.  The MODE field indicates
   what mode the data is known to be in; this field is VOIDmode when the
   register is not known to contain valid data.  */

struct value_data_entry
{
  enum machine_mode mode;
  unsigned int oldest_regno;
  unsigned int next_regno;
  struct queued_debug_insn_change *debug_insn_changes;
};

struct value_data
{
  struct value_data_entry e[FIRST_PSEUDO_REGISTER];
  unsigned int max_value_regs;
  unsigned int n_debug_insn_changes;
};

static alloc_pool debug_insn_changes_pool;

static void kill_value_one_regno (unsigned, struct value_data *);
static void kill_value_regno (unsigned, unsigned, struct value_data *);
static void kill_value (rtx, struct value_data *);
static void set_value_regno (unsigned, enum machine_mode, struct value_data *);
static void init_value_data (struct value_data *);
static void kill_clobbered_value (rtx, const_rtx, void *);
static void kill_set_value (rtx, const_rtx, void *);
static int kill_autoinc_value (rtx *, void *);
static void copy_value (rtx, rtx, struct value_data *);
static bool mode_change_ok (enum machine_mode, enum machine_mode,
			    unsigned int);
static rtx maybe_mode_change (enum machine_mode, enum machine_mode,
			      enum machine_mode, unsigned int, unsigned int);
static rtx find_oldest_value_reg (enum reg_class, rtx, struct value_data *);
static bool replace_oldest_value_reg (rtx *, enum reg_class, rtx,
				      struct value_data *);
static bool replace_oldest_value_addr (rtx *, enum reg_class,
				       enum machine_mode, rtx,
				       struct value_data *);
static bool replace_oldest_value_mem (rtx, rtx, struct value_data *);
static bool copyprop_hardreg_forward_1 (basic_block, struct value_data *);
extern void debug_value_data (struct value_data *);
#ifdef ENABLE_CHECKING
static void validate_value_data (struct value_data *);
#endif

/* Free all queued updates for DEBUG_INSNs that change some reg to
   register REGNO.  */

static void
free_debug_insn_changes (struct value_data *vd, unsigned int regno)
{
  struct queued_debug_insn_change *cur, *next;
  for (cur = vd->e[regno].debug_insn_changes; cur; cur = next)
    {
      next = cur->next;
      --vd->n_debug_insn_changes;
      pool_free (debug_insn_changes_pool, cur);
    }
  vd->e[regno].debug_insn_changes = NULL;
}

/* Kill register REGNO.  This involves removing it from any value
   lists, and resetting the value mode to VOIDmode.  This is only a
   helper function; it does not handle any hard registers overlapping
   with REGNO.  */

static void
kill_value_one_regno (unsigned int regno, struct value_data *vd)
{
  unsigned int i, next;

  if (vd->e[regno].oldest_regno != regno)
    {
      for (i = vd->e[regno].oldest_regno;
	   vd->e[i].next_regno != regno;
	   i = vd->e[i].next_regno)
	continue;
      vd->e[i].next_regno = vd->e[regno].next_regno;
    }
  else if ((next = vd->e[regno].next_regno) != INVALID_REGNUM)
    {
      for (i = next; i != INVALID_REGNUM; i = vd->e[i].next_regno)
	vd->e[i].oldest_regno = next;
    }

  vd->e[regno].mode = VOIDmode;
  vd->e[regno].oldest_regno = regno;
  vd->e[regno].next_regno = INVALID_REGNUM;
  if (vd->e[regno].debug_insn_changes)
    free_debug_insn_changes (vd, regno);

#ifdef ENABLE_CHECKING
  validate_value_data (vd);
#endif
}

/* Kill the value in register REGNO for NREGS, and any other registers
   whose values overlap.  */

static void
kill_value_regno (unsigned int regno, unsigned int nregs,
		  struct value_data *vd)
{
  unsigned int j;

  /* Kill the value we're told to kill.  */
  for (j = 0; j < nregs; ++j)
    kill_value_one_regno (regno + j, vd);

  /* Kill everything that overlapped what we're told to kill.  */
  if (regno < vd->max_value_regs)
    j = 0;
  else
    j = regno - vd->max_value_regs;
  for (; j < regno; ++j)
    {
      unsigned int i, n;
      if (vd->e[j].mode == VOIDmode)
	continue;
      n = hard_regno_nregs[j][vd->e[j].mode];
      if (j + n > regno)
	for (i = 0; i < n; ++i)
	  kill_value_one_regno (j + i, vd);
    }
}

/* Kill X.  This is a convenience function wrapping kill_value_regno
   so that we mind the mode the register is in.  */

static void
kill_value (rtx x, struct value_data *vd)
{
  rtx orig_rtx = x;

  if (GET_CODE (x) == SUBREG)
    {
      x = simplify_subreg (GET_MODE (x), SUBREG_REG (x),
			   GET_MODE (SUBREG_REG (x)), SUBREG_BYTE (x));
      if (x == NULL_RTX)
	x = SUBREG_REG (orig_rtx);
    }
  if (REG_P (x))
    {
      unsigned int regno = REGNO (x);
      unsigned int n = hard_regno_nregs[regno][GET_MODE (x)];

      kill_value_regno (regno, n, vd);
    }
}

/* Remember that REGNO is valid in MODE.  */

static void
set_value_regno (unsigned int regno, enum machine_mode mode,
		 struct value_data *vd)
{
  unsigned int nregs;

  vd->e[regno].mode = mode;

  nregs = hard_regno_nregs[regno][mode];
  if (nregs > vd->max_value_regs)
    vd->max_value_regs = nregs;
}

/* Initialize VD such that there are no known relationships between regs.  */

static void
init_value_data (struct value_data *vd)
{
  int i;
  for (i = 0; i < FIRST_PSEUDO_REGISTER; ++i)
    {
      vd->e[i].mode = VOIDmode;
      vd->e[i].oldest_regno = i;
      vd->e[i].next_regno = INVALID_REGNUM;
      vd->e[i].debug_insn_changes = NULL;
    }
  vd->max_value_regs = 0;
  vd->n_debug_insn_changes = 0;
}

/* Called through note_stores.  If X is clobbered, kill its value.  */

static void
kill_clobbered_value (rtx x, const_rtx set, void *data)
{
  struct value_data *const vd = (struct value_data *) data;
  if (GET_CODE (set) == CLOBBER)
    kill_value (x, vd);
}

/* Called through note_stores.  If X is set, not clobbered, kill its
   current value and install it as the root of its own value list.  */

static void
kill_set_value (rtx x, const_rtx set, void *data)
{
  struct value_data *const vd = (struct value_data *) data;
  if (GET_CODE (set) != CLOBBER)
    {
      kill_value (x, vd);
      if (REG_P (x))
	set_value_regno (REGNO (x), GET_MODE (x), vd);
    }
}

/* Called through for_each_rtx.  Kill any register used as the base of an
   auto-increment expression, and install that register as the root of its
   own value list.  */

static int
kill_autoinc_value (rtx *px, void *data)
{
  rtx x = *px;
  struct value_data *const vd = (struct value_data *) data;

  if (GET_RTX_CLASS (GET_CODE (x)) == RTX_AUTOINC)
    {
      x = XEXP (x, 0);
      kill_value (x, vd);
      set_value_regno (REGNO (x), GET_MODE (x), vd);
      return -1;
    }

  return 0;
}

/* Assert that SRC has been copied to DEST.  Adjust the data structures
   to reflect that SRC contains an older copy of the shared value.  */

static void
copy_value (rtx dest, rtx src, struct value_data *vd)
{
  unsigned int dr = REGNO (dest);
  unsigned int sr = REGNO (src);
  unsigned int dn, sn;
  unsigned int i;

  /* ??? At present, it's possible to see noop sets.  It'd be nice if
     this were cleaned up beforehand...  */
  if (sr == dr)
    return;

  /* Do not propagate copies to the stack pointer, as that can leave
     memory accesses with no scheduling dependency on the stack update.  */
  if (dr == STACK_POINTER_REGNUM)
    return;

  /* Likewise with the frame pointer, if we're using one.  */
  if (frame_pointer_needed && dr == HARD_FRAME_POINTER_REGNUM)
    return;

  /* Do not propagate copies to fixed or global registers, patterns
     can be relying to see particular fixed register or users can
     expect the chosen global register in asm.  */
  if (fixed_regs[dr] || global_regs[dr])
    return;

  /* If SRC and DEST overlap, don't record anything.  */
  dn = hard_regno_nregs[dr][GET_MODE (dest)];
  sn = hard_regno_nregs[sr][GET_MODE (dest)];
  if ((dr > sr && dr < sr + sn)
      || (sr > dr && sr < dr + dn))
    return;

  /* If SRC had no assigned mode (i.e. we didn't know it was live)
     assign it now and assume the value came from an input argument
     or somesuch.  */
  if (vd->e[sr].mode == VOIDmode)
    set_value_regno (sr, vd->e[dr].mode, vd);

  /* If we are narrowing the input to a smaller number of hard regs,
     and it is in big endian, we are really extracting a high part.
     Since we generally associate a low part of a value with the value itself,
     we must not do the same for the high part.
     Note we can still get low parts for the same mode combination through
     a two-step copy involving differently sized hard regs.
     Assume hard regs fr* are 32 bits bits each, while r* are 64 bits each:
     (set (reg:DI r0) (reg:DI fr0))
     (set (reg:SI fr2) (reg:SI r0))
     loads the low part of (reg:DI fr0) - i.e. fr1 - into fr2, while:
     (set (reg:SI fr2) (reg:SI fr0))
     loads the high part of (reg:DI fr0) into fr2.

     We can't properly represent the latter case in our tables, so don't
     record anything then.  */
  else if (sn < (unsigned int) hard_regno_nregs[sr][vd->e[sr].mode]
	   && (GET_MODE_SIZE (vd->e[sr].mode) > UNITS_PER_WORD
	       ? WORDS_BIG_ENDIAN : BYTES_BIG_ENDIAN))
    return;

  /* If SRC had been assigned a mode narrower than the copy, we can't
     link DEST into the chain, because not all of the pieces of the
     copy came from oldest_regno.  */
  else if (sn > (unsigned int) hard_regno_nregs[sr][vd->e[sr].mode])
    return;

  /* Link DR at the end of the value chain used by SR.  */

  vd->e[dr].oldest_regno = vd->e[sr].oldest_regno;

  for (i = sr; vd->e[i].next_regno != INVALID_REGNUM; i = vd->e[i].next_regno)
    continue;
  vd->e[i].next_regno = dr;

#ifdef ENABLE_CHECKING
  validate_value_data (vd);
#endif
}

/* Return true if a mode change from ORIG to NEW is allowed for REGNO.  */

static bool
mode_change_ok (enum machine_mode orig_mode, enum machine_mode new_mode,
		unsigned int regno ATTRIBUTE_UNUSED)
{
  if (GET_MODE_SIZE (orig_mode) < GET_MODE_SIZE (new_mode))
    return false;

#ifdef CANNOT_CHANGE_MODE_CLASS
  return !REG_CANNOT_CHANGE_MODE_P (regno, orig_mode, new_mode);
#endif

  return true;
}

/* Register REGNO was originally set in ORIG_MODE.  It - or a copy of it -
   was copied in COPY_MODE to COPY_REGNO, and then COPY_REGNO was accessed
   in NEW_MODE.
   Return a NEW_MODE rtx for REGNO if that's OK, otherwise return NULL_RTX.  */

static rtx
maybe_mode_change (enum machine_mode orig_mode, enum machine_mode copy_mode,
		   enum machine_mode new_mode, unsigned int regno,
		   unsigned int copy_regno ATTRIBUTE_UNUSED)
{
  if (GET_MODE_SIZE (copy_mode) < GET_MODE_SIZE (orig_mode)
      && GET_MODE_SIZE (copy_mode) < GET_MODE_SIZE (new_mode))
    return NULL_RTX;

  if (orig_mode == new_mode)
    return gen_rtx_raw_REG (new_mode, regno);
  else if (mode_change_ok (orig_mode, new_mode, regno))
    {
      int copy_nregs = hard_regno_nregs[copy_regno][copy_mode];
      int use_nregs = hard_regno_nregs[copy_regno][new_mode];
      int copy_offset
	= GET_MODE_SIZE (copy_mode) / copy_nregs * (copy_nregs - use_nregs);
      int offset
	= GET_MODE_SIZE (orig_mode) - GET_MODE_SIZE (new_mode) - copy_offset;
      int byteoffset = offset % UNITS_PER_WORD;
      int wordoffset = offset - byteoffset;

      offset = ((WORDS_BIG_ENDIAN ? wordoffset : 0)
		+ (BYTES_BIG_ENDIAN ? byteoffset : 0));
      return gen_rtx_raw_REG (new_mode,
			      regno + subreg_regno_offset (regno, orig_mode,
							   offset,
							   new_mode));
    }
  return NULL_RTX;
}

/* Find the oldest copy of the value contained in REGNO that is in
   register class CL and has mode MODE.  If found, return an rtx
   of that oldest register, otherwise return NULL.  */

static rtx
find_oldest_value_reg (enum reg_class cl, rtx reg, struct value_data *vd)
{
  unsigned int regno = REGNO (reg);
  enum machine_mode mode = GET_MODE (reg);
  unsigned int i;

  /* If we are accessing REG in some mode other that what we set it in,
     make sure that the replacement is valid.  In particular, consider
	(set (reg:DI r11) (...))
	(set (reg:SI r9) (reg:SI r11))
	(set (reg:SI r10) (...))
	(set (...) (reg:DI r9))
     Replacing r9 with r11 is invalid.  */
  if (mode != vd->e[regno].mode)
    {
      if (hard_regno_nregs[regno][mode]
	  > hard_regno_nregs[regno][vd->e[regno].mode])
	return NULL_RTX;
    }

  for (i = vd->e[regno].oldest_regno; i != regno; i = vd->e[i].next_regno)
    {
      enum machine_mode oldmode = vd->e[i].mode;
      rtx new_rtx;

      if (!in_hard_reg_set_p (reg_class_contents[cl], mode, i))
	return NULL_RTX;

      new_rtx = maybe_mode_change (oldmode, vd->e[regno].mode, mode, i, regno);
      if (new_rtx)
	{
	  ORIGINAL_REGNO (new_rtx) = ORIGINAL_REGNO (reg);
	  REG_ATTRS (new_rtx) = REG_ATTRS (reg);
	  REG_POINTER (new_rtx) = REG_POINTER (reg);
	  return new_rtx;
	}
    }

  return NULL_RTX;
}

/* If possible, replace the register at *LOC with the oldest register
   in register class CL.  Return true if successfully replaced.  */

static bool
replace_oldest_value_reg (rtx *loc, enum reg_class cl, rtx insn,
			  struct value_data *vd)
{
  rtx new_rtx = find_oldest_value_reg (cl, *loc, vd);
  if (new_rtx)
    {
      if (DEBUG_INSN_P (insn))
	{
	  struct queued_debug_insn_change *change;

	  if (dump_file)
	    fprintf (dump_file, "debug_insn %u: queued replacing reg %u with %u\n",
		     INSN_UID (insn), REGNO (*loc), REGNO (new_rtx));

	  change = (struct queued_debug_insn_change *)
		   pool_alloc (debug_insn_changes_pool);
	  change->next = vd->e[REGNO (new_rtx)].debug_insn_changes;
	  change->insn = insn;
	  change->loc = loc;
	  change->new_rtx = new_rtx;
	  vd->e[REGNO (new_rtx)].debug_insn_changes = change;
	  ++vd->n_debug_insn_changes;
	  return true;
	}
      if (dump_file)
	fprintf (dump_file, "insn %u: replaced reg %u with %u\n",
		 INSN_UID (insn), REGNO (*loc), REGNO (new_rtx));

      validate_change (insn, loc, new_rtx, 1);
      return true;
    }
  return false;
}

/* Similar to replace_oldest_value_reg, but *LOC contains an address.
   Adapted from find_reloads_address_1.  CL is INDEX_REG_CLASS or
   BASE_REG_CLASS depending on how the register is being considered.  */

static bool
replace_oldest_value_addr (rtx *loc, enum reg_class cl,
			   enum machine_mode mode, rtx insn,
			   struct value_data *vd)
{
  rtx x = *loc;
  RTX_CODE code = GET_CODE (x);
  const char *fmt;
  int i, j;
  bool changed = false;

  switch (code)
    {
    case PLUS:
      if (DEBUG_INSN_P (insn))
	break;

      {
	rtx orig_op0 = XEXP (x, 0);
	rtx orig_op1 = XEXP (x, 1);
	RTX_CODE code0 = GET_CODE (orig_op0);
	RTX_CODE code1 = GET_CODE (orig_op1);
	rtx op0 = orig_op0;
	rtx op1 = orig_op1;
	rtx *locI = NULL;
	rtx *locB = NULL;
	enum rtx_code index_code = SCRATCH;

	if (GET_CODE (op0) == SUBREG)
	  {
	    op0 = SUBREG_REG (op0);
	    code0 = GET_CODE (op0);
	  }

	if (GET_CODE (op1) == SUBREG)
	  {
	    op1 = SUBREG_REG (op1);
	    code1 = GET_CODE (op1);
	  }

	if (code0 == MULT || code0 == SIGN_EXTEND || code0 == TRUNCATE
	    || code0 == ZERO_EXTEND || code1 == MEM)
	  {
	    locI = &XEXP (x, 0);
	    locB = &XEXP (x, 1);
	    index_code = GET_CODE (*locI);
	  }
	else if (code1 == MULT || code1 == SIGN_EXTEND || code1 == TRUNCATE
		 || code1 == ZERO_EXTEND || code0 == MEM)
	  {
	    locI = &XEXP (x, 1);
	    locB = &XEXP (x, 0);
	    index_code = GET_CODE (*locI);
	  }
	else if (code0 == CONST_INT || code0 == CONST
		 || code0 == SYMBOL_REF || code0 == LABEL_REF)
	  {
	    locB = &XEXP (x, 1);
	    index_code = GET_CODE (XEXP (x, 0));
	  }
	else if (code1 == CONST_INT || code1 == CONST
		 || code1 == SYMBOL_REF || code1 == LABEL_REF)
	  {
	    locB = &XEXP (x, 0);
	    index_code = GET_CODE (XEXP (x, 1));
	  }
	else if (code0 == REG && code1 == REG)
	  {
	    int index_op;
	    unsigned regno0 = REGNO (op0), regno1 = REGNO (op1);

	    if (REGNO_OK_FOR_INDEX_P (regno1)
		&& regno_ok_for_base_p (regno0, mode, PLUS, REG))
	      index_op = 1;
	    else if (REGNO_OK_FOR_INDEX_P (regno0)
		     && regno_ok_for_base_p (regno1, mode, PLUS, REG))
	      index_op = 0;
	    else if (regno_ok_for_base_p (regno0, mode, PLUS, REG)
		     || REGNO_OK_FOR_INDEX_P (regno1))
	      index_op = 1;
	    else if (regno_ok_for_base_p (regno1, mode, PLUS, REG))
	      index_op = 0;
	    else
	      index_op = 1;

	    locI = &XEXP (x, index_op);
	    locB = &XEXP (x, !index_op);
	    index_code = GET_CODE (*locI);
	  }
	else if (code0 == REG)
	  {
	    locI = &XEXP (x, 0);
	    locB = &XEXP (x, 1);
	    index_code = GET_CODE (*locI);
	  }
	else if (code1 == REG)
	  {
	    locI = &XEXP (x, 1);
	    locB = &XEXP (x, 0);
	    index_code = GET_CODE (*locI);
	  }

	if (locI)
	  changed |= replace_oldest_value_addr (locI, INDEX_REG_CLASS, mode,
						insn, vd);
	if (locB)
	  changed |= replace_oldest_value_addr (locB,
						base_reg_class (mode, PLUS,
								index_code),
						mode, insn, vd);
	return changed;
      }

    case POST_INC:
    case POST_DEC:
    case POST_MODIFY:
    case PRE_INC:
    case PRE_DEC:
    case PRE_MODIFY:
      return false;

    case MEM:
      return replace_oldest_value_mem (x, insn, vd);

    case REG:
      return replace_oldest_value_reg (loc, cl, insn, vd);

    default:
      break;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    {
      if (fmt[i] == 'e')
	changed |= replace_oldest_value_addr (&XEXP (x, i), cl, mode,
					      insn, vd);
      else if (fmt[i] == 'E')
	for (j = XVECLEN (x, i) - 1; j >= 0; j--)
	  changed |= replace_oldest_value_addr (&XVECEXP (x, i, j), cl,
						mode, insn, vd);
    }

  return changed;
}

/* Similar to replace_oldest_value_reg, but X contains a memory.  */

static bool
replace_oldest_value_mem (rtx x, rtx insn, struct value_data *vd)
{
  enum reg_class cl;

  if (DEBUG_INSN_P (insn))
    cl = ALL_REGS;
  else
    cl = base_reg_class (GET_MODE (x), MEM, SCRATCH);

  return replace_oldest_value_addr (&XEXP (x, 0), cl,
				    GET_MODE (x), insn, vd);
}

/* Apply all queued updates for DEBUG_INSNs that change some reg to
   register REGNO.  */

static void
apply_debug_insn_changes (struct value_data *vd, unsigned int regno)
{
  struct queued_debug_insn_change *change;
  rtx last_insn = vd->e[regno].debug_insn_changes->insn;

  for (change = vd->e[regno].debug_insn_changes;
       change;
       change = change->next)
    {
      if (last_insn != change->insn)
	{
	  apply_change_group ();
	  last_insn = change->insn;
	}
      validate_change (change->insn, change->loc, change->new_rtx, 1);
    }
  apply_change_group ();
}

/* Called via for_each_rtx, for all used registers in a real
   insn apply DEBUG_INSN changes that change registers to the
   used register.  */

static int
cprop_find_used_regs_1 (rtx *loc, void *data)
{
  if (REG_P (*loc))
    {
      struct value_data *vd = (struct value_data *) data;
      if (vd->e[REGNO (*loc)].debug_insn_changes)
	{
	  apply_debug_insn_changes (vd, REGNO (*loc));
	  free_debug_insn_changes (vd, REGNO (*loc));
	}
    }
  return 0;
}

/* Called via note_uses, for all used registers in a real insn
   apply DEBUG_INSN changes that change registers to the used
   registers.  */

static void
cprop_find_used_regs (rtx *loc, void *vd)
{
  for_each_rtx (loc, cprop_find_used_regs_1, vd);
}

/* Perform the forward copy propagation on basic block BB.  */

static bool
copyprop_hardreg_forward_1 (basic_block bb, struct value_data *vd)
{
  bool anything_changed = false;
  rtx insn;

  for (insn = BB_HEAD (bb); ; insn = NEXT_INSN (insn))
    {
      int n_ops, i, alt, predicated;
      bool is_asm, any_replacements;
      rtx set;
      bool replaced[MAX_RECOG_OPERANDS];
      bool changed = false;

      if (!NONDEBUG_INSN_P (insn))
	{
	  if (DEBUG_INSN_P (insn))
	    {
	      rtx loc = INSN_VAR_LOCATION_LOC (insn);
	      if (!VAR_LOC_UNKNOWN_P (loc))
		replace_oldest_value_addr (&INSN_VAR_LOCATION_LOC (insn),
					   ALL_REGS, GET_MODE (loc),
					   insn, vd);
	    }

	  if (insn == BB_END (bb))
	    break;
	  else
	    continue;
	}

      set = single_set (insn);
      extract_insn (insn);
      if (! constrain_operands (1))
	fatal_insn_not_found (insn);
      preprocess_constraints ();
      alt = which_alternative;
      n_ops = recog_data.n_operands;
      is_asm = asm_noperands (PATTERN (insn)) >= 0;

      /* Simplify the code below by rewriting things to reflect
	 matching constraints.  Also promote OP_OUT to OP_INOUT
	 in predicated instructions.  */

      predicated = GET_CODE (PATTERN (insn)) == COND_EXEC;
      for (i = 0; i < n_ops; ++i)
	{
	  int matches = recog_op_alt[i][alt].matches;
	  if (matches >= 0)
	    recog_op_alt[i][alt].cl = recog_op_alt[matches][alt].cl;
	  if (matches >= 0 || recog_op_alt[i][alt].matched >= 0
	      || (predicated && recog_data.operand_type[i] == OP_OUT))
	    recog_data.operand_type[i] = OP_INOUT;
	}

      /* Apply changes to earlier DEBUG_INSNs if possible.  */
      if (vd->n_debug_insn_changes)
	note_uses (&PATTERN (insn), cprop_find_used_regs, vd);

      /* For each earlyclobber operand, zap the value data.  */
      for (i = 0; i < n_ops; i++)
	if (recog_op_alt[i][alt].earlyclobber)
	  kill_value (recog_data.operand[i], vd);

      /* Within asms, a clobber cannot overlap inputs or outputs.
	 I wouldn't think this were true for regular insns, but
	 scan_rtx treats them like that...  */
      note_stores (PATTERN (insn), kill_clobbered_value, vd);

      /* Kill all auto-incremented values.  */
      /* ??? REG_INC is useless, since stack pushes aren't done that way.  */
      for_each_rtx (&PATTERN (insn), kill_autoinc_value, vd);

      /* Kill all early-clobbered operands.  */
      for (i = 0; i < n_ops; i++)
	if (recog_op_alt[i][alt].earlyclobber)
	  kill_value (recog_data.operand[i], vd);

      /* Special-case plain move instructions, since we may well
	 be able to do the move from a different register class.  */
      if (set && REG_P (SET_SRC (set)))
	{
	  rtx src = SET_SRC (set);
	  unsigned int regno = REGNO (src);
	  enum machine_mode mode = GET_MODE (src);
	  unsigned int i;
	  rtx new_rtx;

	  /* If we are accessing SRC in some mode other that what we
	     set it in, make sure that the replacement is valid.  */
	  if (mode != vd->e[regno].mode)
	    {
	      if (hard_regno_nregs[regno][mode]
		  > hard_regno_nregs[regno][vd->e[regno].mode])
		goto no_move_special_case;
	    }

	  /* If the destination is also a register, try to find a source
	     register in the same class.  */
	  if (REG_P (SET_DEST (set)))
	    {
	      new_rtx = find_oldest_value_reg (REGNO_REG_CLASS (regno), src, vd);
	      if (new_rtx && validate_change (insn, &SET_SRC (set), new_rtx, 0))
		{
		  if (dump_file)
		    fprintf (dump_file,
			     "insn %u: replaced reg %u with %u\n",
			     INSN_UID (insn), regno, REGNO (new_rtx));
		  changed = true;
		  goto did_replacement;
		}
	    }

	  /* Otherwise, try all valid registers and see if its valid.  */
	  for (i = vd->e[regno].oldest_regno; i != regno;
	       i = vd->e[i].next_regno)
	    {
	      new_rtx = maybe_mode_change (vd->e[i].mode, vd->e[regno].mode,
				       mode, i, regno);
	      if (new_rtx != NULL_RTX)
		{
		  if (validate_change (insn, &SET_SRC (set), new_rtx, 0))
		    {
		      ORIGINAL_REGNO (new_rtx) = ORIGINAL_REGNO (src);
		      REG_ATTRS (new_rtx) = REG_ATTRS (src);
		      REG_POINTER (new_rtx) = REG_POINTER (src);
		      if (dump_file)
			fprintf (dump_file,
				 "insn %u: replaced reg %u with %u\n",
				 INSN_UID (insn), regno, REGNO (new_rtx));
		      changed = true;
		      goto did_replacement;
		    }
		}
	    }
	}
      no_move_special_case:

      any_replacements = false;

      /* For each input operand, replace a hard register with the
	 eldest live copy that's in an appropriate register class.  */
      for (i = 0; i < n_ops; i++)
	{
	  replaced[i] = false;

	  /* Don't scan match_operand here, since we've no reg class
	     information to pass down.  Any operands that we could
	     substitute in will be represented elsewhere.  */
	  if (recog_data.constraints[i][0] == '\0')
	    continue;

	  /* Don't replace in asms intentionally referencing hard regs.  */
	  if (is_asm && REG_P (recog_data.operand[i])
	      && (REGNO (recog_data.operand[i])
		  == ORIGINAL_REGNO (recog_data.operand[i])))
	    continue;

	  if (recog_data.operand_type[i] == OP_IN)
	    {
	      if (recog_op_alt[i][alt].is_address)
		replaced[i]
		  = replace_oldest_value_addr (recog_data.operand_loc[i],
					       recog_op_alt[i][alt].cl,
					       VOIDmode, insn, vd);
	      else if (REG_P (recog_data.operand[i]))
		replaced[i]
		  = replace_oldest_value_reg (recog_data.operand_loc[i],
					      recog_op_alt[i][alt].cl,
					      insn, vd);
	      else if (MEM_P (recog_data.operand[i]))
		replaced[i] = replace_oldest_value_mem (recog_data.operand[i],
							insn, vd);
	    }
	  else if (MEM_P (recog_data.operand[i]))
	    replaced[i] = replace_oldest_value_mem (recog_data.operand[i],
						    insn, vd);

	  /* If we performed any replacement, update match_dups.  */
	  if (replaced[i])
	    {
	      int j;
	      rtx new_rtx;

	      new_rtx = *recog_data.operand_loc[i];
	      recog_data.operand[i] = new_rtx;
	      for (j = 0; j < recog_data.n_dups; j++)
		if (recog_data.dup_num[j] == i)
		  validate_unshare_change (insn, recog_data.dup_loc[j], new_rtx, 1);

	      any_replacements = true;
	    }
	}

      if (any_replacements)
	{
	  if (! apply_change_group ())
	    {
	      for (i = 0; i < n_ops; i++)
		if (replaced[i])
		  {
		    rtx old = *recog_data.operand_loc[i];
		    recog_data.operand[i] = old;
		  }

	      if (dump_file)
		fprintf (dump_file,
			 "insn %u: reg replacements not verified\n",
			 INSN_UID (insn));
	    }
	  else
	    changed = true;
	}

    did_replacement:
      if (changed)
	anything_changed = true;

      /* Clobber call-clobbered registers.  */
      if (CALL_P (insn))
	for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	  if (TEST_HARD_REG_BIT (regs_invalidated_by_call, i))
	    kill_value_regno (i, 1, vd);

      /* Notice stores.  */
      note_stores (PATTERN (insn), kill_set_value, vd);

      /* Notice copies.  */
      if (set && REG_P (SET_DEST (set)) && REG_P (SET_SRC (set)))
	copy_value (SET_DEST (set), SET_SRC (set), vd);

      if (insn == BB_END (bb))
	break;
    }

  return anything_changed;
}

/* Main entry point for the forward copy propagation optimization.  */

static unsigned int
copyprop_hardreg_forward (void)
{
  struct value_data *all_vd;
  basic_block bb;
  sbitmap visited;
  bool analyze_called = false;

  all_vd = XNEWVEC (struct value_data, last_basic_block);

  visited = sbitmap_alloc (last_basic_block);
  sbitmap_zero (visited);

  if (MAY_HAVE_DEBUG_STMTS)
    debug_insn_changes_pool
      = create_alloc_pool ("debug insn changes pool",
			   sizeof (struct queued_debug_insn_change), 256);

  FOR_EACH_BB (bb)
    {
      SET_BIT (visited, bb->index);

      /* If a block has a single predecessor, that we've already
	 processed, begin with the value data that was live at
	 the end of the predecessor block.  */
      /* ??? Ought to use more intelligent queuing of blocks.  */
      if (single_pred_p (bb)
	  && TEST_BIT (visited, single_pred (bb)->index)
	  && ! (single_pred_edge (bb)->flags & (EDGE_ABNORMAL_CALL | EDGE_EH)))
	{
	  all_vd[bb->index] = all_vd[single_pred (bb)->index];
	  if (all_vd[bb->index].n_debug_insn_changes)
	    {
	      unsigned int regno;

	      for (regno = 0; regno < FIRST_PSEUDO_REGISTER; regno++)
		{
		  if (all_vd[bb->index].e[regno].debug_insn_changes)
		    {
		      all_vd[bb->index].e[regno].debug_insn_changes = NULL;
		      if (--all_vd[bb->index].n_debug_insn_changes == 0)
			break;
		    }
		}
	    }
	}
      else
	init_value_data (all_vd + bb->index);

      copyprop_hardreg_forward_1 (bb, all_vd + bb->index);
    }

  if (MAY_HAVE_DEBUG_STMTS)
    {
      FOR_EACH_BB (bb)
	if (TEST_BIT (visited, bb->index)
	    && all_vd[bb->index].n_debug_insn_changes)
	  {
	    unsigned int regno;
	    bitmap live;

	    if (!analyze_called)
	      {
		df_analyze ();
		analyze_called = true;
	      }
	    live = df_get_live_out (bb);
	    for (regno = 0; regno < FIRST_PSEUDO_REGISTER; regno++)
	      if (all_vd[bb->index].e[regno].debug_insn_changes)
		{
		  if (REGNO_REG_SET_P (live, regno))
		    apply_debug_insn_changes (all_vd + bb->index, regno);
		  if (all_vd[bb->index].n_debug_insn_changes == 0)
		    break;
		}
	  }

      free_alloc_pool (debug_insn_changes_pool);
    }

  sbitmap_free (visited);
  free (all_vd);
  return 0;
}

/* Dump the value chain data to stderr.  */

DEBUG_FUNCTION void
debug_value_data (struct value_data *vd)
{
  HARD_REG_SET set;
  unsigned int i, j;

  CLEAR_HARD_REG_SET (set);

  for (i = 0; i < FIRST_PSEUDO_REGISTER; ++i)
    if (vd->e[i].oldest_regno == i)
      {
	if (vd->e[i].mode == VOIDmode)
	  {
	    if (vd->e[i].next_regno != INVALID_REGNUM)
	      fprintf (stderr, "[%u] Bad next_regno for empty chain (%u)\n",
		       i, vd->e[i].next_regno);
	    continue;
	  }

	SET_HARD_REG_BIT (set, i);
	fprintf (stderr, "[%u %s] ", i, GET_MODE_NAME (vd->e[i].mode));

	for (j = vd->e[i].next_regno;
	     j != INVALID_REGNUM;
	     j = vd->e[j].next_regno)
	  {
	    if (TEST_HARD_REG_BIT (set, j))
	      {
		fprintf (stderr, "[%u] Loop in regno chain\n", j);
		return;
	      }

	    if (vd->e[j].oldest_regno != i)
	      {
		fprintf (stderr, "[%u] Bad oldest_regno (%u)\n",
			 j, vd->e[j].oldest_regno);
		return;
	      }
	    SET_HARD_REG_BIT (set, j);
	    fprintf (stderr, "[%u %s] ", j, GET_MODE_NAME (vd->e[j].mode));
	  }
	fputc ('\n', stderr);
      }

  for (i = 0; i < FIRST_PSEUDO_REGISTER; ++i)
    if (! TEST_HARD_REG_BIT (set, i)
	&& (vd->e[i].mode != VOIDmode
	    || vd->e[i].oldest_regno != i
	    || vd->e[i].next_regno != INVALID_REGNUM))
      fprintf (stderr, "[%u] Non-empty reg in chain (%s %u %i)\n",
	       i, GET_MODE_NAME (vd->e[i].mode), vd->e[i].oldest_regno,
	       vd->e[i].next_regno);
}

#ifdef ENABLE_CHECKING
static void
validate_value_data (struct value_data *vd)
{
  HARD_REG_SET set;
  unsigned int i, j;

  CLEAR_HARD_REG_SET (set);

  for (i = 0; i < FIRST_PSEUDO_REGISTER; ++i)
    if (vd->e[i].oldest_regno == i)
      {
	if (vd->e[i].mode == VOIDmode)
	  {
	    if (vd->e[i].next_regno != INVALID_REGNUM)
	      internal_error ("validate_value_data: [%u] Bad next_regno for empty chain (%u)",
			      i, vd->e[i].next_regno);
	    continue;
	  }

	SET_HARD_REG_BIT (set, i);

	for (j = vd->e[i].next_regno;
	     j != INVALID_REGNUM;
	     j = vd->e[j].next_regno)
	  {
	    if (TEST_HARD_REG_BIT (set, j))
	      internal_error ("validate_value_data: Loop in regno chain (%u)",
			      j);
	    if (vd->e[j].oldest_regno != i)
	      internal_error ("validate_value_data: [%u] Bad oldest_regno (%u)",
			      j, vd->e[j].oldest_regno);

	    SET_HARD_REG_BIT (set, j);
	  }
      }

  for (i = 0; i < FIRST_PSEUDO_REGISTER; ++i)
    if (! TEST_HARD_REG_BIT (set, i)
	&& (vd->e[i].mode != VOIDmode
	    || vd->e[i].oldest_regno != i
	    || vd->e[i].next_regno != INVALID_REGNUM))
      internal_error ("validate_value_data: [%u] Non-empty reg in chain (%s %u %i)",
		      i, GET_MODE_NAME (vd->e[i].mode), vd->e[i].oldest_regno,
		      vd->e[i].next_regno);
}
#endif

static bool
gate_handle_cprop (void)
{
  return (optimize > 0 && (flag_cprop_registers));
}


struct rtl_opt_pass pass_cprop_hardreg =
{
 {
  RTL_PASS,
  "cprop_hardreg",                      /* name */
  gate_handle_cprop,                    /* gate */
  copyprop_hardreg_forward,             /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  TV_CPROP_REGISTERS,                   /* tv_id */
  0,                                    /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_dump_func | TODO_df_finish
  | TODO_verify_rtl_sharing		/* todo_flags_finish */
 }
};
