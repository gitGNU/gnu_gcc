#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "tm_p.h"
#include "basic-block.h"
#include "flags.h"
#include "function.h"
#include "tree-inline.h"
#include "gimple.h"
#include "target.h"
#include "tree-iterator.h"
#include "tree-flow.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "hashtab.h"
#include "diagnostic.h"
#include "demangle.h"
#include "langhooks.h"
#include "ggc.h"
#include "cgraph.h"
#include "gimple.h"

static unsigned int pl_execute (void);
static bool pl_gate (void);

static void pl_fix_function_decl (void);
static void pl_init (void);
static void pl_fini (void);
static void pl_register_bounds (tree ptr, tree bnd);
static tree pl_get_registered_bounds (tree ptr);
static basic_block pl_get_entry_block (void);
static tree pl_get_zero_bounds (void);
static void pl_transform_function (void);
static tree pl_get_bound_for_parm (tree parm);
static tree pl_build_bndldx (tree addr, tree ptr, gimple_stmt_iterator gsi);
static void pl_build_bndstx (tree addr, tree ptr, tree bounds,
			     gimple_stmt_iterator gsi);
static tree pl_compute_bounds_for_assignment (tree node, gimple assign);
static tree pl_make_bounds (tree lb, tree size, gimple_stmt_iterator *iter);
static tree pl_make_addressed_object_bounds (tree obj,
					     gimple_stmt_iterator *iter);
static tree pl_get_bounds_for_var_decl (tree var);
static tree pl_get_bounds_for_string_cst (tree cst);
static tree pl_get_bounds_by_definition (tree node, gimple def_stmt,
					 gimple_stmt_iterator *iter);
static tree pl_find_bounds (tree ptr, gimple_stmt_iterator *iter);
static void pl_check_mem_access (tree first, tree last, tree bounds,
				 gimple_stmt_iterator *instr_gsi,
				 location_t location, tree dirflag);
static void pl_parse_array_and_component_ref (tree node, tree *ptr,
					      tree *elt, bool *component,
					      bool *bitfield);
static void pl_process_stmt(gimple_stmt_iterator *iter, tree *tp,
			    location_t loc, tree dirflag);

static GTY (()) tree pl_arg_bnd_fndecl;
static GTY (()) tree pl_bndldx_fndecl;
static GTY (()) tree pl_bndstx_fndecl;
static GTY (()) tree pl_checkl_fndecl;
static GTY (()) tree pl_checku_fndecl;
static GTY (()) tree pl_bndmk_fndecl;
static GTY (()) tree pl_ret_bnd_fndecl;

static GTY (()) tree pl_bound_type;
static GTY (()) tree pl_uintptr_type;

static basic_block entry_block;
static tree zero_bounds;

static void
pl_transform_function (void)
{
  basic_block bb, next;
  gimple_stmt_iterator i;
  int saved_last_basic_block = last_basic_block;
  enum gimple_rhs_class grhs_class;

  bb = ENTRY_BLOCK_PTR ->next_bb;
  do
    {
      next = bb->next_bb;
      for (i = gsi_start_bb (bb); !gsi_end_p (i); gsi_next (&i))
        {
          gimple s = gsi_stmt (i);

          switch (gimple_code (s))
            {
            case GIMPLE_ASSIGN:
	      pl_process_stmt (&i, gimple_assign_lhs_ptr (s),
			       gimple_location (s), integer_one_node);
	      pl_process_stmt (&i, gimple_assign_rhs1_ptr (s),
			       gimple_location (s), integer_zero_node);
	      grhs_class = get_gimple_rhs_class (gimple_assign_rhs_code (s));
	      if (grhs_class == GIMPLE_BINARY_RHS)
		pl_process_stmt (&i, gimple_assign_rhs2_ptr (s),
				 gimple_location (s), integer_zero_node);
              break;

            case GIMPLE_RETURN:
              if (gimple_return_retval (s) != NULL_TREE)
                {
                  pl_process_stmt (&i, gimple_return_retval_ptr (s),
				   gimple_location (s),
				   integer_zero_node);
                }
              break;

            default:
              ;
            }
        }
      bb = next;
    }
  while (bb && bb->index <= saved_last_basic_block);
}

static void
pl_check_mem_access (tree first, tree last, tree bounds,
		     gimple_stmt_iterator *instr_gsi,
		     location_t location, tree dirflag)
{
  gimple_seq seq, stmts;
  gimple stmt;
  tree node;

  seq = gimple_seq_alloc ();

  node = force_gimple_operand (first, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  stmt = gimple_build_call (pl_checkl_fndecl, 2, bounds, node);
  gimple_seq_add_stmt (&seq, stmt);

  node = force_gimple_operand (last, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  stmt = gimple_build_call (pl_checku_fndecl, 2, bounds, node);
  gimple_seq_add_stmt (&seq, stmt);

  gsi_insert_seq_before (instr_gsi, seq, GSI_SAME_STMT);
}

static GTY ((if_marked ("tree_map_marked_p"), param_is (struct tree_map)))
     htab_t pl_reg_bounds;

static void
pl_register_bounds (tree ptr, tree bnd)
{
  struct tree_map **slot, *map;

  map = ggc_alloc_tree_map ();
  map->hash = htab_hash_pointer (ptr);
  map->base.from = ptr;
  map->to = bnd;

  slot = (struct tree_map **)
    htab_find_slot_with_hash (pl_reg_bounds, map, map->hash, INSERT);
  *slot = map;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Regsitered bound ");
      print_generic_expr (dump_file, bnd, 0);
      fprintf (dump_file, " for pointer ");
      print_generic_expr (dump_file, ptr, 0);
      fprintf (dump_file, "\n");
    }
}

static tree
pl_get_registered_bounds (tree ptr)
{
  struct tree_map *res, in;
  in.base.from = ptr;
  in.hash = htab_hash_pointer (ptr);

  res = (struct tree_map *) htab_find_with_hash (pl_reg_bounds,
						 &in, in.hash);

  return res ? res->to : NULL_TREE;
}

static basic_block
pl_get_entry_block (void)
{
  if (!entry_block)
    {
      basic_block prev_entry = ENTRY_BLOCK_PTR->next_bb;
      edge e = find_edge (ENTRY_BLOCK_PTR, prev_entry);
      entry_block = split_edge (e);
    }

  return entry_block;
}

static tree
pl_get_zero_bounds (void)
{
  if (zero_bounds)
    return zero_bounds;

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "Creating zero bounds...");

  zero_bounds = pl_make_bounds (integer_zero_node,
				integer_minus_one_node,
				NULL);

  return zero_bounds;
}

static tree
pl_build_returned_bound (gimple call)
{
  gimple_stmt_iterator gsi = gsi_for_stmt (call);
  tree bounds;
  gimple stmt;

  stmt = gimple_build_call (pl_ret_bnd_fndecl, 0);
  gsi_insert_after (&gsi, stmt, GSI_SAME_STMT);

  bounds = create_tmp_reg (pl_bound_type, NULL);
  add_referenced_var (bounds);
  bounds = make_ssa_name (bounds, stmt);
  gimple_call_set_lhs (stmt, bounds);

  update_stmt (stmt);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Built returned bounds (");
      print_generic_expr (dump_file, bounds, 0);
      fprintf (dump_file, ") for call: ");
      print_gimple_stmt (dump_file, call, 0, TDF_VOPS|TDF_MEMSYMS);
    }

  pl_register_bounds (gimple_call_lhs (call), bounds);

  return bounds;
}

static tree
pl_get_bound_for_parm (tree parm)
{
  tree bounds;

  bounds = pl_get_registered_bounds (parm);

  /* NULL bounds mean parm is not a pointer and
     zero bounds should be returned.  */
  if (!bounds)
    {
      gcc_assert (!POINTER_TYPE_P (TREE_TYPE (parm)));
      bounds = pl_get_zero_bounds ();
    }

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Using bounds ");
      print_generic_expr (dump_file, bounds, 0);
      fprintf (dump_file, " for parm ");
      print_generic_expr (dump_file, parm, 0);
      fprintf (dump_file, " of type ");
      print_generic_expr (dump_file, TREE_TYPE (parm), 0);
      fprintf (dump_file, ".\n");
    }

  return bounds;
}

static tree
pl_build_bndldx (tree addr, tree ptr, gimple_stmt_iterator gsi)
{
  gimple_seq seq, stmts;
  gimple stmt;
  tree bounds;

  seq = gimple_seq_alloc ();

  addr = force_gimple_operand (addr, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  stmt = gimple_build_call (pl_bndldx_fndecl, 2, addr, ptr);
  bounds = create_tmp_reg (pl_bound_type, NULL);
  add_referenced_var (bounds);
  bounds = make_ssa_name (bounds, stmt);
  gimple_call_set_lhs (stmt, bounds);

  gimple_seq_add_stmt (&seq, stmt);

  gsi_insert_seq_after (&gsi, seq, GSI_SAME_STMT);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Generated bndldx for pointer ");
      print_generic_expr (dump_file, ptr, 0);
      fprintf (dump_file, ": ");
      print_gimple_stmt (dump_file, stmt, 0, TDF_VOPS|TDF_MEMSYMS);
    }

  return bounds;
}

static void
pl_build_bndstx (tree addr, tree ptr, tree bounds,
		 gimple_stmt_iterator gsi)
{
  gimple_seq seq, stmts;
  gimple stmt;

  seq = gimple_seq_alloc ();

  addr = force_gimple_operand (addr, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  ptr = force_gimple_operand (ptr, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  stmt = gimple_build_call (pl_bndstx_fndecl, 3, addr, ptr, bounds);

  gimple_seq_add_stmt (&seq, stmt);

  gsi_insert_seq_after (&gsi, seq, GSI_SAME_STMT);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Generated bndstx for pointer store ");
      print_gimple_stmt (dump_file, gsi_stmt (gsi), 0, TDF_VOPS|TDF_MEMSYMS);
      print_gimple_stmt (dump_file, stmt, 2, TDF_VOPS|TDF_MEMSYMS);
    }
}

static tree
pl_compute_bounds_for_assignment (tree node, gimple assign)
{
  enum tree_code rhs_code = gimple_assign_rhs_code (assign);
  location_t loc = gimple_location (assign);
  tree rhs1 = gimple_assign_rhs1 (assign);
  tree bounds = NULL_TREE;
  gimple_stmt_iterator iter;
  tree ptr;
  tree addr;
  tree offs;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Computing bounds for assignment: ");
      print_gimple_stmt (dump_file, assign, 0, TDF_VOPS|TDF_MEMSYMS);
    }

  switch (rhs_code)
    {
    case MEM_REF:
      gcc_assert (node);

      ptr = TREE_OPERAND (rhs1, 0);
      offs = TREE_OPERAND (rhs1, 1);

      addr = fold_build_pointer_plus_loc (loc, ptr, offs);
      bounds = pl_build_bndldx (addr, node, gsi_for_stmt (assign));
      break;

    case ARRAY_REF:
    case COMPONENT_REF:
      {
	tree elt;
	bool component;
	bool bitfield;

	pl_parse_array_and_component_ref (rhs1, &ptr, &elt, &component,
					  &bitfield);
	iter = gsi_for_stmt (assign);
	if (component)
	  bounds = pl_find_bounds (ptr, &iter);
	else
	  {
	    addr = fold_build1 (ADDR_EXPR, build_pointer_type (TREE_TYPE (rhs1)), rhs1);
	    bounds = pl_build_bndldx (addr, node, gsi_for_stmt (assign));
	  }
      }
      break;

    case SSA_NAME:
    case ADDR_EXPR:
    case POINTER_PLUS_EXPR:
      iter = gsi_for_stmt (assign);
      bounds = pl_find_bounds (rhs1, &iter);
      break;

    case INTEGER_CST:
      bounds = pl_get_zero_bounds ();
      break;

    default:
      internal_error ("pl_compute_bounds_for_assignment: Unexpected RHS code %s",
		      tree_code_name[rhs_code]);
    }

  gcc_assert (bounds);

  if (node)
    pl_register_bounds (node, bounds);

  return bounds;
}

static tree
pl_get_bounds_by_definition (tree node, gimple def_stmt, gimple_stmt_iterator *iter)
{
  tree var, bounds;
  enum gimple_code code = gimple_code (def_stmt);
  gimple stmt;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Searching for bounds for node: ");
      print_generic_expr (dump_file, node, 0);

      fprintf (dump_file, " using its definition: ");
      print_gimple_stmt (dump_file, def_stmt, 0, TDF_VOPS|TDF_MEMSYMS);
    }

  switch (code)
    {
    case GIMPLE_NOP:
      var = SSA_NAME_VAR (node);
      switch (TREE_CODE (var))
	{
	case PARM_DECL:
	  bounds = pl_get_bound_for_parm (var);
	  pl_register_bounds (node, bounds);
	  break;

	default:
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "Unexpected var with no definition\n");
	      print_generic_expr (dump_file, var, 0);
	    }
	  internal_error ("pl_get_bounds_by_definition: Unexpected var of type %s",
			  tree_code_name[(int) TREE_CODE (var)]);
	}
      break;

    case GIMPLE_ASSIGN:
      bounds = pl_compute_bounds_for_assignment (node, def_stmt);
      break;

    case GIMPLE_CALL:
      bounds = pl_build_returned_bound (def_stmt);
      break;

    case GIMPLE_PHI:
      bounds = create_tmp_reg (pl_bound_type, NULL);
      add_referenced_var (bounds);

      stmt = create_phi_node (bounds, gimple_bb (def_stmt));
      bounds = gimple_phi_result (stmt);
      *iter = gsi_for_stmt (stmt);

      pl_register_bounds (node, bounds);
      break;

    default:
      internal_error ("pl_get_bounds_by_definition: Unexpected GIMPLE code %s",
		      gimple_code_name[code]);
    }

  return bounds;
}

static tree
pl_make_bounds (tree lb, tree size, gimple_stmt_iterator *iter)
{
  gimple_seq seq, stmts;
  gimple_stmt_iterator gsi;
  gimple stmt;
  tree bounds;

  if (iter)
    gsi = *iter;
  else
    gsi = gsi_start_bb (pl_get_entry_block ());

  seq = gimple_seq_alloc ();

  lb = force_gimple_operand (lb, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  size = force_gimple_operand (size, &stmts, true, NULL_TREE);
  gimple_seq_add_seq (&seq, stmts);

  stmt = gimple_build_call (pl_bndmk_fndecl, 2, lb, size);

  bounds = create_tmp_reg (pl_bound_type, NULL);
  add_referenced_var (bounds);
  bounds = make_ssa_name (bounds, stmt);
  gimple_call_set_lhs (stmt, bounds);

  gimple_seq_add_stmt (&seq, stmt);

  gsi_insert_seq_before (&gsi, seq, GSI_SAME_STMT);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Made bounds: ");
      print_gimple_stmt (dump_file, stmt, 0, TDF_VOPS|TDF_MEMSYMS);
      if (iter)
	{
	  fprintf (dump_file, "  Inserted before statement: ");
	  print_gimple_stmt (dump_file, gsi_stmt (gsi), 0, TDF_VOPS|TDF_MEMSYMS);
	}
      else
	fprintf (dump_file, "At function entry\n");
    }

  /* update_stmt (stmt); */

  return bounds;
}

static tree
pl_get_bounds_for_var_decl (tree var)
{
  tree bounds;
  tree lb;
  tree size;

  gcc_assert (TREE_CODE (var) == VAR_DECL);

  bounds = pl_get_registered_bounds (var);

  if (bounds)
    return bounds;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Building bounds for var decl ");
      print_generic_expr (dump_file, var, 0);
    }

  lb = fold_build1 (ADDR_EXPR, build_pointer_type (TREE_TYPE (var)), var);
  /* We need size in bytes rounded up.  */
  size = build_int_cst (size_type_node,
			(tree_low_cst (DECL_SIZE (var), 1) + 7) / 8);
  bounds = pl_make_bounds (lb, size, NULL);

  pl_register_bounds (var, bounds);

  return bounds;
}

static tree
pl_get_bounds_for_string_cst (tree cst)
{
  tree bounds;
  tree lb;
  tree size;

  gcc_assert (TREE_CODE (cst) == STRING_CST);

  bounds = pl_get_registered_bounds (cst);

  if (bounds)
    return bounds;

  lb = fold_build1 (ADDR_EXPR, build_pointer_type (TREE_TYPE (cst)), cst);
  size = build_int_cst (pl_uintptr_type, TREE_STRING_LENGTH (cst));
  bounds = pl_make_bounds (lb, size, NULL);

  pl_register_bounds (cst, bounds);

  return bounds;
}

static tree
pl_make_addressed_object_bounds (tree obj, gimple_stmt_iterator *iter)
{
  tree bounds;

  switch (TREE_CODE (obj))
    {
    case VAR_DECL:
      bounds = pl_get_bounds_for_var_decl (obj);
      break;

    case STRING_CST:
      bounds = pl_get_bounds_for_string_cst (obj);
      break;

    default:
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "pl_make_addressed_object_bounds: "
		   "unexpected object of type %s\n",
		   tree_code_name[TREE_CODE (obj)]);
	  print_node (dump_file, "", obj, 0);
	}
      internal_error ("pl_make_addressed_object_bounds: Unexpected tree code %s",
		      tree_code_name[TREE_CODE (obj)]);
    }

  return bounds;
}

static tree
pl_find_bounds (tree ptr, gimple_stmt_iterator *iter)
{
  tree bounds = NULL_TREE;

  switch (TREE_CODE (ptr))
    {
    case SSA_NAME:
      bounds = pl_get_registered_bounds (ptr);
      if (!bounds)
	{
	  gimple def_stmt = SSA_NAME_DEF_STMT (ptr);

	  bounds = pl_get_bounds_by_definition(ptr, def_stmt, iter);

	  gcc_assert (bounds);

	  if (gimple_code (def_stmt) == GIMPLE_PHI)
	    {
	      gimple phi_bnd = gsi_stmt (*iter);
	      unsigned i;

	      for (i = 0; i < gimple_phi_num_args (def_stmt); i++)
		{
		  tree arg = gimple_phi_arg_def (def_stmt, i);
		  tree arg_bnd;

		  gcc_assert (arg && (TREE_CODE (arg) == SSA_NAME));

		  arg_bnd = pl_find_bounds (arg, iter);

		  add_phi_arg (phi_bnd, arg_bnd,
			       gimple_phi_arg_edge (def_stmt, i),
			       UNKNOWN_LOCATION);
		}
	    }

	  gcc_assert (bounds == pl_get_registered_bounds (ptr));
	}
      break;

    case ADDR_EXPR:
      bounds = pl_make_addressed_object_bounds (TREE_OPERAND (ptr, 0), iter);
      break;

    default:
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "pl_find_bounds: unexpected ptr of type %s\n",
		   tree_code_name[TREE_CODE (ptr)]);
	  print_node (dump_file, "", ptr, 0);
	}
      internal_error ("pl_find_bounds: Unexpected tree code %s",
		      tree_code_name[TREE_CODE (ptr)]);
    }

  if (!bounds)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (stderr, "pl_find_bounds: cannot find bounds for pointer\n");
	  print_node (dump_file, "", ptr, 0);
	}
      internal_error ("pl_find_bounds: Cannot find bounds for pointer");
    }

  return bounds;
}

static void
pl_parse_array_and_component_ref (tree node, tree *ptr,
				  tree *elt, bool *component,
				  bool *bitfield)
{
  tree var = TREE_OPERAND (node, 0);

  *component = (TREE_CODE (node) == COMPONENT_REF);
  *bitfield = (TREE_CODE (node) == COMPONENT_REF
	       && DECL_BIT_FIELD_TYPE (TREE_OPERAND (node, 1)));
  *elt = NULL_TREE;

  while (true)
    {
      if (*bitfield && *elt == NULL_TREE
	  && (TREE_CODE (var) == ARRAY_REF
	      || TREE_CODE (var) == COMPONENT_REF))
	*elt = var;

      if (TREE_CODE (var) == ARRAY_REF)
	{
	  *component = false;
	  var = TREE_OPERAND (var, 0);
	}
      else if (TREE_CODE (var) == COMPONENT_REF)
	var = TREE_OPERAND (var, 0);
      else if (INDIRECT_REF_P (var)
	       || TREE_CODE (var) == MEM_REF)
	{
	  *ptr = TREE_OPERAND (var, 0);
	  break;
	}
      else if (TREE_CODE (var) == VIEW_CONVERT_EXPR)
	{
	  var = TREE_OPERAND (var, 0);
	  if (CONSTANT_CLASS_P (var)
	      && TREE_CODE (var) != STRING_CST)
	    return;
	}
      else
	{
	  gcc_assert (TREE_CODE (var) == VAR_DECL
		      || TREE_CODE (var) == PARM_DECL
		      || TREE_CODE (var) == RESULT_DECL
		      || TREE_CODE (var) == STRING_CST);

	  if (*component)
	    return;
	  else
	    {
	      *ptr = build1 (ADDR_EXPR,
			     build_pointer_type (TREE_TYPE (var)), var);
	      break;
	    }
	}
    }
}

static void
pl_process_stmt (gimple_stmt_iterator *iter, tree *tp,
		 location_t loc, tree dirflag)
{
  tree node = *tp;
  tree node_type = TREE_TYPE (node);
  tree size = TYPE_SIZE_UNIT (node_type);
  tree addr_first = NULL_TREE; /* address of the first accessed byte */
  tree addr_end = NULL_TREE; /* address of the byte past the last accessed byte */
  tree addr_last = NULL_TREE; /* address of the last accessed byte */
  tree ptr = NULL_TREE; /* a pointer used for dereference */
  tree bounds;
  bool safe = false;

  // TODO: check we need to instrument this node

  switch (TREE_CODE (node))
    {
    case ARRAY_REF:
    case COMPONENT_REF:
      {
	bool bitfield;
	bool component;
	tree elt;

	pl_parse_array_and_component_ref (node, &ptr, &elt, &safe,
					  &bitfield);

	/* Break if there is no dereference and operation is safe.  */
	if (safe)
	  {
	    /* We may still need addr_first for bndstx in case
	       write a pointer into memory.  */
	    addr_first = fold_build1_loc (loc, ADDR_EXPR,
					  build_pointer_type (node_type),
					  node);
	    break;
	  }

	if (bitfield)
          {
            tree field = TREE_OPERAND (node, 1);

            if (TREE_CODE (DECL_SIZE_UNIT (field)) == INTEGER_CST)
              size = DECL_SIZE_UNIT (field);

	    if (elt)
	      elt = build1 (ADDR_EXPR, build_pointer_type (TREE_TYPE (elt)),
			    elt);
            addr_first = fold_convert_loc (loc, ptr_type_node, elt ? elt : ptr);
            addr_first = fold_build_pointer_plus_loc (loc,
						      addr_first,
						      byte_position (field));
          }
        else
          addr_first = build1 (ADDR_EXPR, build_pointer_type (node_type), node);

        addr_last = fold_build2_loc (loc, MINUS_EXPR, pl_uintptr_type,
                             fold_build2_loc (loc, PLUS_EXPR, pl_uintptr_type,
					  fold_convert (pl_uintptr_type, addr_first),
					  size),
                             size_one_node);
      }
      break;

    case INDIRECT_REF:
      ptr = TREE_OPERAND (node, 0);
      addr_first = ptr;
      addr_end = fold_build_pointer_plus_loc (loc, addr_first, size);
      addr_last = fold_build_pointer_plus_hwi_loc (loc, addr_end, -1);
      break;

    case MEM_REF:
      ptr = TREE_OPERAND (node, 0);
      addr_first = fold_build_pointer_plus_loc (loc, ptr,
						TREE_OPERAND (node, 1));
      addr_end = fold_build_pointer_plus_loc (loc, addr_first, size);
      addr_last = fold_build_pointer_plus_hwi_loc (loc, addr_end, -1);
      break;

    case TARGET_MEM_REF:
      printf("TARGET_MEM_REF\n");
      debug_gimple_stmt(gsi_stmt(*iter));
      debug_tree(node);
      gcc_unreachable ();
      break;

    case ARRAY_RANGE_REF:
      printf("ARRAY_RANGE_REF\n");
      debug_gimple_stmt(gsi_stmt(*iter));
      debug_tree(node);
      gcc_unreachable ();
      break;

    case BIT_FIELD_REF:
      printf("BIT_FIELD_REF\n");
      debug_gimple_stmt(gsi_stmt(*iter));
      debug_tree(node);
      gcc_unreachable ();
      break;

    default:
      return;
    }

  if (!safe)
    {
      bounds = pl_find_bounds (ptr, iter);
      pl_check_mem_access (addr_first, addr_last, bounds, iter, loc, dirflag);
    }

  /* We need to generate bndstx in case pointer is stored.  */
  if (dirflag == integer_one_node && POINTER_TYPE_P (node_type))
    {
      gimple stmt = gsi_stmt (*iter);

      gcc_assert ( gimple_code(stmt) == GIMPLE_ASSIGN);

      bounds = pl_compute_bounds_for_assignment (NULL_TREE, stmt);
      pl_build_bndstx (addr_first, gimple_assign_rhs1 (stmt), bounds, *iter);
    }
}

/* Add input bound arguments declaration to the current
   function declaration.  */
static void
pl_fix_function_decl (void)
{
  tree decl = cfun->decl;
  tree arg = DECL_ARGUMENTS (decl);
  tree prev_arg = NULL_TREE;
  int bnd_no = 0;

  /* Nothing to do if function has no input arguments.  */
  if (!arg)
    return;

  /* Go through all input pointers and create bound
     declaration for each of them.  Bound declaration
     is placed right before pointer arg.  Also make
     and register ssa name for each bound.  */
  while (arg)
    {
      if (POINTER_TYPE_P (TREE_TYPE (arg)))
	{
	  char name_buf[20];
	  tree name;
	  tree bounds;

	  sprintf (name_buf, "__arg_bnd.%d", bnd_no++);
	  name = get_identifier (name_buf);

	  bounds = build_decl (UNKNOWN_LOCATION, PARM_DECL, name,
				 pl_bound_type);
	  DECL_ARG_TYPE (bounds) = pl_bound_type;

	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "Built bounds '");
	      print_generic_expr (dump_file, bounds, 0);
	      fprintf (dump_file, "' for arg '");
	      print_generic_expr (dump_file, arg, 0);
	      fprintf (dump_file, "' of type '");
	      print_generic_expr (dump_file, TREE_TYPE (arg), 0);
	      fprintf (dump_file, "'\n");
	    }

	  if (prev_arg)
	    {
	      TREE_CHAIN (prev_arg) = bounds;
	      TREE_CHAIN (bounds) = arg;
	    }
	  else
	    {
	      TREE_CHAIN (bounds) = DECL_ARGUMENTS (decl);
	      DECL_ARGUMENTS (decl) = bounds;
	    }

	  bounds = make_ssa_name (bounds, gimple_build_nop ());
	  SSA_NAME_IS_DEFAULT_DEF (bounds) = 1;

	  pl_register_bounds (arg, bounds);
	}

      prev_arg = arg;
      arg = TREE_CHAIN (arg);
    }
}

static void
pl_init (void)
{
  /* Allocate hash table for bounds.  */
  pl_reg_bounds = htab_create_ggc (31, tree_map_hash, tree_map_eq,
				   NULL);

  entry_block = NULL;
  zero_bounds = NULL_TREE;

  pl_bound_type = TARGET_64BIT ? bound64_type_node : bound32_type_node;
  pl_uintptr_type = lang_hooks.types.type_for_mode (ptr_mode, true);

  /* Build declarations for builtin functions.  */
  pl_arg_bnd_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDARG);
  pl_bndldx_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDLDX);
  pl_bndstx_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDSTX);
  pl_checkl_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDCL);
  pl_checku_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDCU);
  pl_bndmk_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDMK);
  pl_ret_bnd_fndecl = targetm.builtin_pl_function (BUILT_IN_PL_BNDRET);
}

static void
pl_fini (void)
{

}

static unsigned int
pl_execute (void)
{
  //TODO: check we need to instrument this function
  pl_init ();

  pl_fix_function_decl ();
  pl_transform_function ();

  pl_fini ();

  return 0;
}

static bool
pl_gate (void)
{
  return flag_pl != 0;
}

struct gimple_opt_pass pass_pl =
{
 {
  GIMPLE_PASS,
  "pl",                                 /* name */
  pl_gate,                              /* gate */
  pl_execute,                           /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  TV_NONE,                              /* tv_id */
  PROP_ssa | PROP_cfg,                  /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_verify_flow | TODO_verify_stmts
  | TODO_update_ssa                     /* todo_flags_finish */
 }
};
