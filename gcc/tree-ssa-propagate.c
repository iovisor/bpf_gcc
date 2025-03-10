/* Generic SSA value propagation engine.
   Copyright (C) 2004-2015 Free Software Foundation, Inc.
   Contributed by Diego Novillo <dnovillo@redhat.com>

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any
   later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "flags.h"
#include "tm_p.h"
#include "predict.h"
#include "vec.h"
#include "hashtab.h"
#include "hash-set.h"
#include "machmode.h"
#include "hard-reg-set.h"
#include "input.h"
#include "function.h"
#include "dominance.h"
#include "cfg.h"
#include "basic-block.h"
#include "gimple-pretty-print.h"
#include "dumpfile.h"
#include "bitmap.h"
#include "sbitmap.h"
#include "tree-ssa-alias.h"
#include "internal-fn.h"
#include "gimple-fold.h"
#include "tree-eh.h"
#include "gimple-expr.h"
#include "is-a.h"
#include "gimple.h"
#include "gimplify.h"
#include "gimple-iterator.h"
#include "gimple-ssa.h"
#include "tree-cfg.h"
#include "tree-phinodes.h"
#include "ssa-iterators.h"
#include "stringpool.h"
#include "tree-ssanames.h"
#include "tree-ssa.h"
#include "tree-ssa-propagate.h"
#include "langhooks.h"
#include "value-prof.h"
#include "domwalk.h"

/* This file implements a generic value propagation engine based on
   the same propagation used by the SSA-CCP algorithm [1].

   Propagation is performed by simulating the execution of every
   statement that produces the value being propagated.  Simulation
   proceeds as follows:

   1- Initially, all edges of the CFG are marked not executable and
      the CFG worklist is seeded with all the statements in the entry
      basic block (block 0).

   2- Every statement S is simulated with a call to the call-back
      function SSA_PROP_VISIT_STMT.  This evaluation may produce 3
      results:

      	SSA_PROP_NOT_INTERESTING: Statement S produces nothing of
	    interest and does not affect any of the work lists.

	SSA_PROP_VARYING: The value produced by S cannot be determined
	    at compile time.  Further simulation of S is not required.
	    If S is a conditional jump, all the outgoing edges for the
	    block are considered executable and added to the work
	    list.

	SSA_PROP_INTERESTING: S produces a value that can be computed
	    at compile time.  Its result can be propagated into the
	    statements that feed from S.  Furthermore, if S is a
	    conditional jump, only the edge known to be taken is added
	    to the work list.  Edges that are known not to execute are
	    never simulated.

   3- PHI nodes are simulated with a call to SSA_PROP_VISIT_PHI.  The
      return value from SSA_PROP_VISIT_PHI has the same semantics as
      described in #2.

   4- Three work lists are kept.  Statements are only added to these
      lists if they produce one of SSA_PROP_INTERESTING or
      SSA_PROP_VARYING.

   	CFG_BLOCKS contains the list of blocks to be simulated.
	    Blocks are added to this list if their incoming edges are
	    found executable.

	VARYING_SSA_EDGES contains the list of statements that feed
	    from statements that produce an SSA_PROP_VARYING result.
	    These are simulated first to speed up processing.

	INTERESTING_SSA_EDGES contains the list of statements that
	    feed from statements that produce an SSA_PROP_INTERESTING
	    result.

   5- Simulation terminates when all three work lists are drained.

   Before calling ssa_propagate, it is important to clear
   prop_simulate_again_p for all the statements in the program that
   should be simulated.  This initialization allows an implementation
   to specify which statements should never be simulated.

   It is also important to compute def-use information before calling
   ssa_propagate.

   References:

     [1] Constant propagation with conditional branches,
         Wegman and Zadeck, ACM TOPLAS 13(2):181-210.

     [2] Building an Optimizing Compiler,
	 Robert Morgan, Butterworth-Heinemann, 1998, Section 8.9.

     [3] Advanced Compiler Design and Implementation,
	 Steven Muchnick, Morgan Kaufmann, 1997, Section 12.6  */

/* Function pointers used to parameterize the propagation engine.  */
static ssa_prop_visit_stmt_fn ssa_prop_visit_stmt;
static ssa_prop_visit_phi_fn ssa_prop_visit_phi;

/* Keep track of statements that have been added to one of the SSA
   edges worklists.  This flag is used to avoid visiting statements
   unnecessarily when draining an SSA edge worklist.  If while
   simulating a basic block, we find a statement with
   STMT_IN_SSA_EDGE_WORKLIST set, we clear it to prevent SSA edge
   processing from visiting it again.

   NOTE: users of the propagation engine are not allowed to use
   the GF_PLF_1 flag.  */
#define STMT_IN_SSA_EDGE_WORKLIST	GF_PLF_1

/* A bitmap to keep track of executable blocks in the CFG.  */
static sbitmap executable_blocks;

/* Array of control flow edges on the worklist.  */
static vec<basic_block> cfg_blocks;

static unsigned int cfg_blocks_num = 0;
static int cfg_blocks_tail;
static int cfg_blocks_head;

static sbitmap bb_in_list;

/* Worklist of SSA edges which will need reexamination as their
   definition has changed.  SSA edges are def-use edges in the SSA
   web.  For each D-U edge, we store the target statement or PHI node
   U.  */
static vec<gimple> interesting_ssa_edges;

/* Identical to INTERESTING_SSA_EDGES.  For performance reasons, the
   list of SSA edges is split into two.  One contains all SSA edges
   who need to be reexamined because their lattice value changed to
   varying (this worklist), and the other contains all other SSA edges
   to be reexamined (INTERESTING_SSA_EDGES).

   Since most values in the program are VARYING, the ideal situation
   is to move them to that lattice value as quickly as possible.
   Thus, it doesn't make sense to process any other type of lattice
   value until all VARYING values are propagated fully, which is one
   thing using the VARYING worklist achieves.  In addition, if we
   don't use a separate worklist for VARYING edges, we end up with
   situations where lattice values move from
   UNDEFINED->INTERESTING->VARYING instead of UNDEFINED->VARYING.  */
static vec<gimple> varying_ssa_edges;


/* Return true if the block worklist empty.  */

static inline bool
cfg_blocks_empty_p (void)
{
  return (cfg_blocks_num == 0);
}


/* Add a basic block to the worklist.  The block must not be already
   in the worklist, and it must not be the ENTRY or EXIT block.  */

static void
cfg_blocks_add (basic_block bb)
{
  bool head = false;

  gcc_assert (bb != ENTRY_BLOCK_PTR_FOR_FN (cfun)
	      && bb != EXIT_BLOCK_PTR_FOR_FN (cfun));
  gcc_assert (!bitmap_bit_p (bb_in_list, bb->index));

  if (cfg_blocks_empty_p ())
    {
      cfg_blocks_tail = cfg_blocks_head = 0;
      cfg_blocks_num = 1;
    }
  else
    {
      cfg_blocks_num++;
      if (cfg_blocks_num > cfg_blocks.length ())
	{
	  /* We have to grow the array now.  Adjust to queue to occupy
	     the full space of the original array.  We do not need to
	     initialize the newly allocated portion of the array
	     because we keep track of CFG_BLOCKS_HEAD and
	     CFG_BLOCKS_HEAD.  */
	  cfg_blocks_tail = cfg_blocks.length ();
	  cfg_blocks_head = 0;
	  cfg_blocks.safe_grow (2 * cfg_blocks_tail);
	}
      /* Minor optimization: we prefer to see blocks with more
	 predecessors later, because there is more of a chance that
	 the incoming edges will be executable.  */
      else if (EDGE_COUNT (bb->preds)
	       >= EDGE_COUNT (cfg_blocks[cfg_blocks_head]->preds))
	cfg_blocks_tail = ((cfg_blocks_tail + 1) % cfg_blocks.length ());
      else
	{
	  if (cfg_blocks_head == 0)
	    cfg_blocks_head = cfg_blocks.length ();
	  --cfg_blocks_head;
	  head = true;
	}
    }

  cfg_blocks[head ? cfg_blocks_head : cfg_blocks_tail] = bb;
  bitmap_set_bit (bb_in_list, bb->index);
}


/* Remove a block from the worklist.  */

static basic_block
cfg_blocks_get (void)
{
  basic_block bb;

  bb = cfg_blocks[cfg_blocks_head];

  gcc_assert (!cfg_blocks_empty_p ());
  gcc_assert (bb);

  cfg_blocks_head = ((cfg_blocks_head + 1) % cfg_blocks.length ());
  --cfg_blocks_num;
  bitmap_clear_bit (bb_in_list, bb->index);

  return bb;
}


/* We have just defined a new value for VAR.  If IS_VARYING is true,
   add all immediate uses of VAR to VARYING_SSA_EDGES, otherwise add
   them to INTERESTING_SSA_EDGES.  */

static void
add_ssa_edge (tree var, bool is_varying)
{
  imm_use_iterator iter;
  use_operand_p use_p;

  FOR_EACH_IMM_USE_FAST (use_p, iter, var)
    {
      gimple use_stmt = USE_STMT (use_p);

      if (prop_simulate_again_p (use_stmt)
	  && !gimple_plf (use_stmt, STMT_IN_SSA_EDGE_WORKLIST))
	{
	  gimple_set_plf (use_stmt, STMT_IN_SSA_EDGE_WORKLIST, true);
	  if (is_varying)
	    varying_ssa_edges.safe_push (use_stmt);
	  else
	    interesting_ssa_edges.safe_push (use_stmt);
	}
    }
}


/* Add edge E to the control flow worklist.  */

static void
add_control_edge (edge e)
{
  basic_block bb = e->dest;
  if (bb == EXIT_BLOCK_PTR_FOR_FN (cfun))
    return;

  /* If the edge had already been executed, skip it.  */
  if (e->flags & EDGE_EXECUTABLE)
    return;

  e->flags |= EDGE_EXECUTABLE;

  /* If the block is already in the list, we're done.  */
  if (bitmap_bit_p (bb_in_list, bb->index))
    return;

  cfg_blocks_add (bb);

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "\nAdding Destination of edge (%d -> %d) to worklist\n",
	e->src->index, e->dest->index);
}


/* Simulate the execution of STMT and update the work lists accordingly.  */

static void
simulate_stmt (gimple stmt)
{
  enum ssa_prop_result val = SSA_PROP_NOT_INTERESTING;
  edge taken_edge = NULL;
  tree output_name = NULL_TREE;

  /* Don't bother visiting statements that are already
     considered varying by the propagator.  */
  if (!prop_simulate_again_p (stmt))
    return;

  if (gimple_code (stmt) == GIMPLE_PHI)
    {
      val = ssa_prop_visit_phi (as_a <gphi *> (stmt));
      output_name = gimple_phi_result (stmt);
    }
  else
    val = ssa_prop_visit_stmt (stmt, &taken_edge, &output_name);

  if (val == SSA_PROP_VARYING)
    {
      prop_set_simulate_again (stmt, false);

      /* If the statement produced a new varying value, add the SSA
	 edges coming out of OUTPUT_NAME.  */
      if (output_name)
	add_ssa_edge (output_name, true);

      /* If STMT transfers control out of its basic block, add
	 all outgoing edges to the work list.  */
      if (stmt_ends_bb_p (stmt))
	{
	  edge e;
	  edge_iterator ei;
	  basic_block bb = gimple_bb (stmt);
	  FOR_EACH_EDGE (e, ei, bb->succs)
	    add_control_edge (e);
	}
    }
  else if (val == SSA_PROP_INTERESTING)
    {
      /* If the statement produced new value, add the SSA edges coming
	 out of OUTPUT_NAME.  */
      if (output_name)
	add_ssa_edge (output_name, false);

      /* If we know which edge is going to be taken out of this block,
	 add it to the CFG work list.  */
      if (taken_edge)
	add_control_edge (taken_edge);
    }
}

/* Process an SSA edge worklist.  WORKLIST is the SSA edge worklist to
   drain.  This pops statements off the given WORKLIST and processes
   them until there are no more statements on WORKLIST.
   We take a pointer to WORKLIST because it may be reallocated when an
   SSA edge is added to it in simulate_stmt.  */

static void
process_ssa_edge_worklist (vec<gimple> *worklist)
{
  /* Drain the entire worklist.  */
  while (worklist->length () > 0)
    {
      basic_block bb;

      /* Pull the statement to simulate off the worklist.  */
      gimple stmt = worklist->pop ();

      /* If this statement was already visited by simulate_block, then
	 we don't need to visit it again here.  */
      if (!gimple_plf (stmt, STMT_IN_SSA_EDGE_WORKLIST))
	continue;

      /* STMT is no longer in a worklist.  */
      gimple_set_plf (stmt, STMT_IN_SSA_EDGE_WORKLIST, false);

      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "\nSimulating statement (from ssa_edges): ");
	  print_gimple_stmt (dump_file, stmt, 0, dump_flags);
	}

      bb = gimple_bb (stmt);

      /* PHI nodes are always visited, regardless of whether or not
	 the destination block is executable.  Otherwise, visit the
	 statement only if its block is marked executable.  */
      if (gimple_code (stmt) == GIMPLE_PHI
	  || bitmap_bit_p (executable_blocks, bb->index))
	simulate_stmt (stmt);
    }
}


/* Simulate the execution of BLOCK.  Evaluate the statement associated
   with each variable reference inside the block.  */

static void
simulate_block (basic_block block)
{
  gimple_stmt_iterator gsi;

  /* There is nothing to do for the exit block.  */
  if (block == EXIT_BLOCK_PTR_FOR_FN (cfun))
    return;

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "\nSimulating block %d\n", block->index);

  /* Always simulate PHI nodes, even if we have simulated this block
     before.  */
  for (gsi = gsi_start_phis (block); !gsi_end_p (gsi); gsi_next (&gsi))
    simulate_stmt (gsi_stmt (gsi));

  /* If this is the first time we've simulated this block, then we
     must simulate each of its statements.  */
  if (!bitmap_bit_p (executable_blocks, block->index))
    {
      gimple_stmt_iterator j;
      unsigned int normal_edge_count;
      edge e, normal_edge;
      edge_iterator ei;

      /* Note that we have simulated this block.  */
      bitmap_set_bit (executable_blocks, block->index);

      for (j = gsi_start_bb (block); !gsi_end_p (j); gsi_next (&j))
	{
	  gimple stmt = gsi_stmt (j);

	  /* If this statement is already in the worklist then
	     "cancel" it.  The reevaluation implied by the worklist
	     entry will produce the same value we generate here and
	     thus reevaluating it again from the worklist is
	     pointless.  */
	  if (gimple_plf (stmt, STMT_IN_SSA_EDGE_WORKLIST))
	    gimple_set_plf (stmt, STMT_IN_SSA_EDGE_WORKLIST, false);

	  simulate_stmt (stmt);
	}

      /* We can not predict when abnormal and EH edges will be executed, so
	 once a block is considered executable, we consider any
	 outgoing abnormal edges as executable.

	 TODO: This is not exactly true.  Simplifying statement might
	 prove it non-throwing and also computed goto can be handled
	 when destination is known.

	 At the same time, if this block has only one successor that is
	 reached by non-abnormal edges, then add that successor to the
	 worklist.  */
      normal_edge_count = 0;
      normal_edge = NULL;
      FOR_EACH_EDGE (e, ei, block->succs)
	{
	  if (e->flags & (EDGE_ABNORMAL | EDGE_EH))
	    add_control_edge (e);
	  else
	    {
	      normal_edge_count++;
	      normal_edge = e;
	    }
	}

      if (normal_edge_count == 1)
	add_control_edge (normal_edge);
    }
}


/* Initialize local data structures and work lists.  */

static void
ssa_prop_init (void)
{
  edge e;
  edge_iterator ei;
  basic_block bb;

  /* Worklists of SSA edges.  */
  interesting_ssa_edges.create (20);
  varying_ssa_edges.create (20);

  executable_blocks = sbitmap_alloc (last_basic_block_for_fn (cfun));
  bitmap_clear (executable_blocks);

  bb_in_list = sbitmap_alloc (last_basic_block_for_fn (cfun));
  bitmap_clear (bb_in_list);

  if (dump_file && (dump_flags & TDF_DETAILS))
    dump_immediate_uses (dump_file);

  cfg_blocks.create (20);
  cfg_blocks.safe_grow_cleared (20);

  /* Initially assume that every edge in the CFG is not executable.
     (including the edges coming out of the entry block).  */
  FOR_ALL_BB_FN (bb, cfun)
    {
      gimple_stmt_iterator si;

      for (si = gsi_start_bb (bb); !gsi_end_p (si); gsi_next (&si))
	gimple_set_plf (gsi_stmt (si), STMT_IN_SSA_EDGE_WORKLIST, false);

      for (si = gsi_start_phis (bb); !gsi_end_p (si); gsi_next (&si))
	gimple_set_plf (gsi_stmt (si), STMT_IN_SSA_EDGE_WORKLIST, false);

      FOR_EACH_EDGE (e, ei, bb->succs)
	e->flags &= ~EDGE_EXECUTABLE;
    }

  /* Seed the algorithm by adding the successors of the entry block to the
     edge worklist.  */
  FOR_EACH_EDGE (e, ei, ENTRY_BLOCK_PTR_FOR_FN (cfun)->succs)
    add_control_edge (e);
}


/* Free allocated storage.  */

static void
ssa_prop_fini (void)
{
  interesting_ssa_edges.release ();
  varying_ssa_edges.release ();
  cfg_blocks.release ();
  sbitmap_free (bb_in_list);
  sbitmap_free (executable_blocks);
}


/* Return true if EXPR is an acceptable right-hand-side for a
   GIMPLE assignment.  We validate the entire tree, not just
   the root node, thus catching expressions that embed complex
   operands that are not permitted in GIMPLE.  This function
   is needed because the folding routines in fold-const.c
   may return such expressions in some cases, e.g., an array
   access with an embedded index addition.  It may make more
   sense to have folding routines that are sensitive to the
   constraints on GIMPLE operands, rather than abandoning any
   any attempt to fold if the usual folding turns out to be too
   aggressive.  */

bool
valid_gimple_rhs_p (tree expr)
{
  enum tree_code code = TREE_CODE (expr);

  switch (TREE_CODE_CLASS (code))
    {
    case tcc_declaration:
      if (!is_gimple_variable (expr))
	return false;
      break;

    case tcc_constant:
      /* All constants are ok.  */
      break;

    case tcc_comparison:
      /* GENERIC allows comparisons with non-boolean types, reject
         those for GIMPLE.  Let vector-typed comparisons pass - rules
	 for GENERIC and GIMPLE are the same here.  */
      if (!(INTEGRAL_TYPE_P (TREE_TYPE (expr))
	    && (TREE_CODE (TREE_TYPE (expr)) == BOOLEAN_TYPE
		|| TYPE_PRECISION (TREE_TYPE (expr)) == 1))
	  && ! VECTOR_TYPE_P (TREE_TYPE (expr)))
	return false;

      /* Fallthru.  */
    case tcc_binary:
      if (!is_gimple_val (TREE_OPERAND (expr, 0))
	  || !is_gimple_val (TREE_OPERAND (expr, 1)))
	return false;
      break;

    case tcc_unary:
      if (!is_gimple_val (TREE_OPERAND (expr, 0)))
	return false;
      break;

    case tcc_expression:
      switch (code)
        {
        case ADDR_EXPR:
          {
	    tree t;
	    if (is_gimple_min_invariant (expr))
	      return true;
            t = TREE_OPERAND (expr, 0);
            while (handled_component_p (t))
              {
                /* ??? More checks needed, see the GIMPLE verifier.  */
                if ((TREE_CODE (t) == ARRAY_REF
                     || TREE_CODE (t) == ARRAY_RANGE_REF)
                    && !is_gimple_val (TREE_OPERAND (t, 1)))
                  return false;
                t = TREE_OPERAND (t, 0);
              }
            if (!is_gimple_id (t))
              return false;
          }
          break;

	default:
	  if (get_gimple_rhs_class (code) == GIMPLE_TERNARY_RHS)
	    {
	      if (((code == VEC_COND_EXPR || code == COND_EXPR)
		   ? !is_gimple_condexpr (TREE_OPERAND (expr, 0))
		   : !is_gimple_val (TREE_OPERAND (expr, 0)))
		  || !is_gimple_val (TREE_OPERAND (expr, 1))
		  || !is_gimple_val (TREE_OPERAND (expr, 2)))
		return false;
	      break;
	    }
	  return false;
	}
      break;

    case tcc_vl_exp:
      return false;

    case tcc_exceptional:
      if (code == CONSTRUCTOR)
	{
	  unsigned i;
	  tree elt;
	  FOR_EACH_CONSTRUCTOR_VALUE (CONSTRUCTOR_ELTS (expr), i, elt)
	    if (!is_gimple_val (elt))
	      return false;
	  return true;
	}
      if (code != SSA_NAME)
        return false;
      break;

    case tcc_reference:
      if (code == BIT_FIELD_REF)
	return is_gimple_val (TREE_OPERAND (expr, 0));
      return false;

    default:
      return false;
    }

  return true;
}


/* Return true if EXPR is a CALL_EXPR suitable for representation
   as a single GIMPLE_CALL statement.  If the arguments require
   further gimplification, return false.  */

static bool
valid_gimple_call_p (tree expr)
{
  unsigned i, nargs;

  if (TREE_CODE (expr) != CALL_EXPR)
    return false;

  nargs = call_expr_nargs (expr);
  for (i = 0; i < nargs; i++)
    {
      tree arg = CALL_EXPR_ARG (expr, i);
      if (is_gimple_reg_type (TREE_TYPE (arg)))
	{
	  if (!is_gimple_val (arg))
	    return false;
	}
      else
	if (!is_gimple_lvalue (arg))
	  return false;
    }

  return true;
}


/* Make SSA names defined by OLD_STMT point to NEW_STMT
   as their defining statement.  */

void
move_ssa_defining_stmt_for_defs (gimple new_stmt, gimple old_stmt)
{
  tree var;
  ssa_op_iter iter;

  if (gimple_in_ssa_p (cfun))
    {
      /* Make defined SSA_NAMEs point to the new
         statement as their definition.  */
      FOR_EACH_SSA_TREE_OPERAND (var, old_stmt, iter, SSA_OP_ALL_DEFS)
        {
          if (TREE_CODE (var) == SSA_NAME)
            SSA_NAME_DEF_STMT (var) = new_stmt;
        }
    }
}

/* Helper function for update_gimple_call and update_call_from_tree.
   A GIMPLE_CALL STMT is being replaced with GIMPLE_CALL NEW_STMT.  */

static void
finish_update_gimple_call (gimple_stmt_iterator *si_p, gimple new_stmt,
			   gimple stmt)
{
  gimple_call_set_lhs (new_stmt, gimple_call_lhs (stmt));
  move_ssa_defining_stmt_for_defs (new_stmt, stmt);
  gimple_set_vuse (new_stmt, gimple_vuse (stmt));
  gimple_set_vdef (new_stmt, gimple_vdef (stmt));
  gimple_set_location (new_stmt, gimple_location (stmt));
  if (gimple_block (new_stmt) == NULL_TREE)
    gimple_set_block (new_stmt, gimple_block (stmt));
  gsi_replace (si_p, new_stmt, false);
}

/* Update a GIMPLE_CALL statement at iterator *SI_P to call to FN
   with number of arguments NARGS, where the arguments in GIMPLE form
   follow NARGS argument.  */

bool
update_gimple_call (gimple_stmt_iterator *si_p, tree fn, int nargs, ...)
{
  va_list ap;
  gcall *new_stmt, *stmt = as_a <gcall *> (gsi_stmt (*si_p));

  gcc_assert (is_gimple_call (stmt));
  va_start (ap, nargs);
  new_stmt = gimple_build_call_valist (fn, nargs, ap);
  finish_update_gimple_call (si_p, new_stmt, stmt);
  va_end (ap);
  return true;
}

/* Update a GIMPLE_CALL statement at iterator *SI_P to reflect the
   value of EXPR, which is expected to be the result of folding the
   call.  This can only be done if EXPR is a CALL_EXPR with valid
   GIMPLE operands as arguments, or if it is a suitable RHS expression
   for a GIMPLE_ASSIGN.  More complex expressions will require
   gimplification, which will introduce additional statements.  In this
   event, no update is performed, and the function returns false.
   Note that we cannot mutate a GIMPLE_CALL in-place, so we always
   replace the statement at *SI_P with an entirely new statement.
   The new statement need not be a call, e.g., if the original call
   folded to a constant.  */

bool
update_call_from_tree (gimple_stmt_iterator *si_p, tree expr)
{
  gimple stmt = gsi_stmt (*si_p);

  if (valid_gimple_call_p (expr))
    {
      /* The call has simplified to another call.  */
      tree fn = CALL_EXPR_FN (expr);
      unsigned i;
      unsigned nargs = call_expr_nargs (expr);
      vec<tree> args = vNULL;
      gcall *new_stmt;

      if (nargs > 0)
        {
          args.create (nargs);
          args.safe_grow_cleared (nargs);

          for (i = 0; i < nargs; i++)
            args[i] = CALL_EXPR_ARG (expr, i);
        }

      new_stmt = gimple_build_call_vec (fn, args);
      finish_update_gimple_call (si_p, new_stmt, stmt);
      args.release ();

      return true;
    }
  else if (valid_gimple_rhs_p (expr))
    {
      tree lhs = gimple_call_lhs (stmt);
      gimple new_stmt;

      /* The call has simplified to an expression
         that cannot be represented as a GIMPLE_CALL. */
      if (lhs)
        {
          /* A value is expected.
             Introduce a new GIMPLE_ASSIGN statement.  */
          STRIP_USELESS_TYPE_CONVERSION (expr);
          new_stmt = gimple_build_assign (lhs, expr);
          move_ssa_defining_stmt_for_defs (new_stmt, stmt);
	  gimple_set_vuse (new_stmt, gimple_vuse (stmt));
	  gimple_set_vdef (new_stmt, gimple_vdef (stmt));
        }
      else if (!TREE_SIDE_EFFECTS (expr))
        {
          /* No value is expected, and EXPR has no effect.
             Replace it with an empty statement.  */
          new_stmt = gimple_build_nop ();
	  if (gimple_in_ssa_p (cfun))
	    {
	      unlink_stmt_vdef (stmt);
	      release_defs (stmt);
	    }
        }
      else
        {
          /* No value is expected, but EXPR has an effect,
             e.g., it could be a reference to a volatile
             variable.  Create an assignment statement
             with a dummy (unused) lhs variable.  */
          STRIP_USELESS_TYPE_CONVERSION (expr);
	  if (gimple_in_ssa_p (cfun))
	    lhs = make_ssa_name (TREE_TYPE (expr));
	  else
	    lhs = create_tmp_var (TREE_TYPE (expr));
          new_stmt = gimple_build_assign (lhs, expr);
	  gimple_set_vuse (new_stmt, gimple_vuse (stmt));
	  gimple_set_vdef (new_stmt, gimple_vdef (stmt));
          move_ssa_defining_stmt_for_defs (new_stmt, stmt);
        }
      gimple_set_location (new_stmt, gimple_location (stmt));
      gsi_replace (si_p, new_stmt, false);
      return true;
    }
  else
    /* The call simplified to an expression that is
       not a valid GIMPLE RHS.  */
    return false;
}


/* Entry point to the propagation engine.

   VISIT_STMT is called for every statement visited.
   VISIT_PHI is called for every PHI node visited.  */

void
ssa_propagate (ssa_prop_visit_stmt_fn visit_stmt,
	       ssa_prop_visit_phi_fn visit_phi)
{
  ssa_prop_visit_stmt = visit_stmt;
  ssa_prop_visit_phi = visit_phi;

  ssa_prop_init ();

  /* Iterate until the worklists are empty.  */
  while (!cfg_blocks_empty_p ()
	 || interesting_ssa_edges.length () > 0
	 || varying_ssa_edges.length () > 0)
    {
      if (!cfg_blocks_empty_p ())
	{
	  /* Pull the next block to simulate off the worklist.  */
	  basic_block dest_block = cfg_blocks_get ();
	  simulate_block (dest_block);
	}

      /* In order to move things to varying as quickly as
	 possible,process the VARYING_SSA_EDGES worklist first.  */
      process_ssa_edge_worklist (&varying_ssa_edges);

      /* Now process the INTERESTING_SSA_EDGES worklist.  */
      process_ssa_edge_worklist (&interesting_ssa_edges);
    }

  ssa_prop_fini ();
}


/* Return true if STMT is of the form 'mem_ref = RHS', where 'mem_ref'
   is a non-volatile pointer dereference, a structure reference or a
   reference to a single _DECL.  Ignore volatile memory references
   because they are not interesting for the optimizers.  */

bool
stmt_makes_single_store (gimple stmt)
{
  tree lhs;

  if (gimple_code (stmt) != GIMPLE_ASSIGN
      && gimple_code (stmt) != GIMPLE_CALL)
    return false;

  if (!gimple_vdef (stmt))
    return false;

  lhs = gimple_get_lhs (stmt);

  /* A call statement may have a null LHS.  */
  if (!lhs)
    return false;

  return (!TREE_THIS_VOLATILE (lhs)
          && (DECL_P (lhs)
	      || REFERENCE_CLASS_P (lhs)));
}


/* Propagation statistics.  */
struct prop_stats_d
{
  long num_const_prop;
  long num_copy_prop;
  long num_stmts_folded;
  long num_dce;
};

static struct prop_stats_d prop_stats;

/* Replace USE references in statement STMT with the values stored in
   PROP_VALUE. Return true if at least one reference was replaced.  */

static bool
replace_uses_in (gimple stmt, ssa_prop_get_value_fn get_value)
{
  bool replaced = false;
  use_operand_p use;
  ssa_op_iter iter;

  FOR_EACH_SSA_USE_OPERAND (use, stmt, iter, SSA_OP_USE)
    {
      tree tuse = USE_FROM_PTR (use);
      tree val = (*get_value) (tuse);

      if (val == tuse || val == NULL_TREE)
	continue;

      if (gimple_code (stmt) == GIMPLE_ASM
	  && !may_propagate_copy_into_asm (tuse))
	continue;

      if (!may_propagate_copy (tuse, val))
	continue;

      if (TREE_CODE (val) != SSA_NAME)
	prop_stats.num_const_prop++;
      else
	prop_stats.num_copy_prop++;

      propagate_value (use, val);

      replaced = true;
    }

  return replaced;
}


/* Replace propagated values into all the arguments for PHI using the
   values from PROP_VALUE.  */

static bool
replace_phi_args_in (gphi *phi, ssa_prop_get_value_fn get_value)
{
  size_t i;
  bool replaced = false;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Folding PHI node: ");
      print_gimple_stmt (dump_file, phi, 0, TDF_SLIM);
    }

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    {
      tree arg = gimple_phi_arg_def (phi, i);

      if (TREE_CODE (arg) == SSA_NAME)
	{
	  tree val = (*get_value) (arg);

	  if (val && val != arg && may_propagate_copy (arg, val))
	    {
	      if (TREE_CODE (val) != SSA_NAME)
		prop_stats.num_const_prop++;
	      else
		prop_stats.num_copy_prop++;

	      propagate_value (PHI_ARG_DEF_PTR (phi, i), val);
	      replaced = true;

	      /* If we propagated a copy and this argument flows
		 through an abnormal edge, update the replacement
		 accordingly.  */
	      if (TREE_CODE (val) == SSA_NAME
		  && gimple_phi_arg_edge (phi, i)->flags & EDGE_ABNORMAL)
		SSA_NAME_OCCURS_IN_ABNORMAL_PHI (val) = 1;
	    }
	}
    }

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      if (!replaced)
	fprintf (dump_file, "No folding possible\n");
      else
	{
	  fprintf (dump_file, "Folded into: ");
	  print_gimple_stmt (dump_file, phi, 0, TDF_SLIM);
	  fprintf (dump_file, "\n");
	}
    }

  return replaced;
}


class substitute_and_fold_dom_walker : public dom_walker
{
public:
    substitute_and_fold_dom_walker (cdi_direction direction,
				    ssa_prop_get_value_fn get_value_fn_,
				    ssa_prop_fold_stmt_fn fold_fn_,
				    bool do_dce_)
	: dom_walker (direction), get_value_fn (get_value_fn_),
      fold_fn (fold_fn_), do_dce (do_dce_), something_changed (false)
    {
      stmts_to_remove.create (0);
      need_eh_cleanup = BITMAP_ALLOC (NULL);
    }
    ~substitute_and_fold_dom_walker ()
    {
      stmts_to_remove.release ();
      BITMAP_FREE (need_eh_cleanup);
    }

    virtual void before_dom_children (basic_block);
    virtual void after_dom_children (basic_block) {}

    ssa_prop_get_value_fn get_value_fn;
    ssa_prop_fold_stmt_fn fold_fn;
    bool do_dce;
    bool something_changed;
    vec<gimple> stmts_to_remove;
    bitmap need_eh_cleanup;
};

void
substitute_and_fold_dom_walker::before_dom_children (basic_block bb)
{
  /* Propagate known values into PHI nodes.  */
  for (gphi_iterator i = gsi_start_phis (bb);
       !gsi_end_p (i);
       gsi_next (&i))
    {
      gphi *phi = i.phi ();
      tree res = gimple_phi_result (phi);
      if (virtual_operand_p (res))
	continue;
      if (do_dce
	  && res && TREE_CODE (res) == SSA_NAME)
	{
	  tree sprime = get_value_fn (res);
	  if (sprime
	      && sprime != res
	      && may_propagate_copy (res, sprime))
	    {
	      stmts_to_remove.safe_push (phi);
	      continue;
	    }
	}
      something_changed |= replace_phi_args_in (phi, get_value_fn);
    }

  /* Propagate known values into stmts.  In some case it exposes
     more trivially deletable stmts to walk backward.  */
  for (gimple_stmt_iterator i = gsi_start_bb (bb);
       !gsi_end_p (i);
       gsi_next (&i))
    {
      bool did_replace;
      gimple stmt = gsi_stmt (i);
      gimple old_stmt;
      enum gimple_code code = gimple_code (stmt);

      /* Ignore ASSERT_EXPRs.  They are used by VRP to generate
	 range information for names and they are discarded
	 afterwards.  */

      if (code == GIMPLE_ASSIGN
	  && TREE_CODE (gimple_assign_rhs1 (stmt)) == ASSERT_EXPR)
	continue;

      /* No point propagating into a stmt we have a value for we
         can propagate into all uses.  Mark it for removal instead.  */
      tree lhs = gimple_get_lhs (stmt);
      if (do_dce
	  && lhs && TREE_CODE (lhs) == SSA_NAME)
	{
	  tree sprime = get_value_fn (lhs);
	  if (sprime
	      && sprime != lhs
	      && may_propagate_copy (lhs, sprime)
	      && !stmt_could_throw_p (stmt)
	      && !gimple_has_side_effects (stmt))
	    {
	      stmts_to_remove.safe_push (stmt);
	      continue;
	    }
	}

      /* Replace the statement with its folded version and mark it
	 folded.  */
      did_replace = false;
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "Folding statement: ");
	  print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
	}

      old_stmt = stmt;

      /* Some statements may be simplified using propagator
	 specific information.  Do this before propagating
	 into the stmt to not disturb pass specific information.  */
      if (fold_fn
	  && (*fold_fn)(&i))
	{
	  did_replace = true;
	  prop_stats.num_stmts_folded++;
	  stmt = gsi_stmt (i);
	  update_stmt (stmt);
	}

      /* Replace real uses in the statement.  */
      did_replace |= replace_uses_in (stmt, get_value_fn);

      /* If we made a replacement, fold the statement.  */
      if (did_replace)
	fold_stmt (&i, follow_single_use_edges);

      /* Now cleanup.  */
      if (did_replace)
	{
	  stmt = gsi_stmt (i);

	  /* If we cleaned up EH information from the statement,
	     remove EH edges.  */
	  if (maybe_clean_or_replace_eh_stmt (old_stmt, stmt))
	    bitmap_set_bit (need_eh_cleanup, bb->index);

	  if (is_gimple_assign (stmt)
	      && (get_gimple_rhs_class (gimple_assign_rhs_code (stmt))
		  == GIMPLE_SINGLE_RHS))
	    {
	      tree rhs = gimple_assign_rhs1 (stmt);

	      if (TREE_CODE (rhs) == ADDR_EXPR)
		recompute_tree_invariant_for_addr_expr (rhs);
	    }

	  /* Determine what needs to be done to update the SSA form.  */
	  update_stmt (stmt);
	  if (!is_gimple_debug (stmt))
	    something_changed = true;
	}

      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  if (did_replace)
	    {
	      fprintf (dump_file, "Folded into: ");
	      print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
	      fprintf (dump_file, "\n");
	    }
	  else
	    fprintf (dump_file, "Not folded\n");
	}
    }
}



/* Perform final substitution and folding of propagated values.

   PROP_VALUE[I] contains the single value that should be substituted
   at every use of SSA name N_I.  If PROP_VALUE is NULL, no values are
   substituted.

   If FOLD_FN is non-NULL the function will be invoked on all statements
   before propagating values for pass specific simplification.

   DO_DCE is true if trivially dead stmts can be removed.

   If DO_DCE is true, the statements within a BB are walked from
   last to first element.  Otherwise we scan from first to last element.

   Return TRUE when something changed.  */

bool
substitute_and_fold (ssa_prop_get_value_fn get_value_fn,
		     ssa_prop_fold_stmt_fn fold_fn,
		     bool do_dce)
{
  gcc_assert (get_value_fn);

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "\nSubstituting values and folding statements\n\n");

  memset (&prop_stats, 0, sizeof (prop_stats));

  calculate_dominance_info (CDI_DOMINATORS);
  substitute_and_fold_dom_walker walker(CDI_DOMINATORS,
					get_value_fn, fold_fn, do_dce);
  walker.walk (ENTRY_BLOCK_PTR_FOR_FN (cfun));

  /* We cannot remove stmts during the BB walk, especially not release
     SSA names there as that destroys the lattice of our callers.
     Remove stmts in reverse order to make debug stmt creation possible.  */
  while (!walker.stmts_to_remove.is_empty ())
    {
      gimple stmt = walker.stmts_to_remove.pop ();
      if (dump_file && dump_flags & TDF_DETAILS)
	{
	  fprintf (dump_file, "Removing dead stmt ");
	  print_gimple_stmt (dump_file, stmt, 0, 0);
	  fprintf (dump_file, "\n");
	}
      prop_stats.num_dce++;
      gimple_stmt_iterator gsi = gsi_for_stmt (stmt);
      if (gimple_code (stmt) == GIMPLE_PHI)
	remove_phi_node (&gsi, true);
      else
	{
	  unlink_stmt_vdef (stmt);
	  gsi_remove (&gsi, true);
	  release_defs (stmt);
	}
    }

  if (!bitmap_empty_p (walker.need_eh_cleanup))
    gimple_purge_all_dead_eh_edges (walker.need_eh_cleanup);

  statistics_counter_event (cfun, "Constants propagated",
			    prop_stats.num_const_prop);
  statistics_counter_event (cfun, "Copies propagated",
			    prop_stats.num_copy_prop);
  statistics_counter_event (cfun, "Statements folded",
			    prop_stats.num_stmts_folded);
  statistics_counter_event (cfun, "Statements deleted",
			    prop_stats.num_dce);

  return walker.something_changed;
}


/* Return true if we may propagate ORIG into DEST, false otherwise.  */

bool
may_propagate_copy (tree dest, tree orig)
{
  tree type_d = TREE_TYPE (dest);
  tree type_o = TREE_TYPE (orig);

  /* If ORIG is a default definition which flows in from an abnormal edge
     then the copy can be propagated.  It is important that we do so to avoid
     uninitialized copies.  */
  if (TREE_CODE (orig) == SSA_NAME
      && SSA_NAME_OCCURS_IN_ABNORMAL_PHI (orig)
      && SSA_NAME_IS_DEFAULT_DEF (orig)
      && (SSA_NAME_VAR (orig) == NULL_TREE
	  || TREE_CODE (SSA_NAME_VAR (orig)) == VAR_DECL))
    ;
  /* Otherwise if ORIG just flows in from an abnormal edge then the copy cannot
     be propagated.  */
  else if (TREE_CODE (orig) == SSA_NAME
	   && SSA_NAME_OCCURS_IN_ABNORMAL_PHI (orig))
    return false;
  /* Similarly if DEST flows in from an abnormal edge then the copy cannot be
     propagated.  */
  else if (TREE_CODE (dest) == SSA_NAME
	   && SSA_NAME_OCCURS_IN_ABNORMAL_PHI (dest))
    return false;

  /* Do not copy between types for which we *do* need a conversion.  */
  if (!useless_type_conversion_p (type_d, type_o))
    return false;

  /* Generally propagating virtual operands is not ok as that may
     create overlapping life-ranges.  */
  if (TREE_CODE (dest) == SSA_NAME && virtual_operand_p (dest))
    return false;

  /* Anything else is OK.  */
  return true;
}

/* Like may_propagate_copy, but use as the destination expression
   the principal expression (typically, the RHS) contained in
   statement DEST.  This is more efficient when working with the
   gimple tuples representation.  */

bool
may_propagate_copy_into_stmt (gimple dest, tree orig)
{
  tree type_d;
  tree type_o;

  /* If the statement is a switch or a single-rhs assignment,
     then the expression to be replaced by the propagation may
     be an SSA_NAME.  Fortunately, there is an explicit tree
     for the expression, so we delegate to may_propagate_copy.  */

  if (gimple_assign_single_p (dest))
    return may_propagate_copy (gimple_assign_rhs1 (dest), orig);
  else if (gswitch *dest_swtch = dyn_cast <gswitch *> (dest))
    return may_propagate_copy (gimple_switch_index (dest_swtch), orig);

  /* In other cases, the expression is not materialized, so there
     is no destination to pass to may_propagate_copy.  On the other
     hand, the expression cannot be an SSA_NAME, so the analysis
     is much simpler.  */

  if (TREE_CODE (orig) == SSA_NAME
      && SSA_NAME_OCCURS_IN_ABNORMAL_PHI (orig))
    return false;

  if (is_gimple_assign (dest))
    type_d = TREE_TYPE (gimple_assign_lhs (dest));
  else if (gimple_code (dest) == GIMPLE_COND)
    type_d = boolean_type_node;
  else if (is_gimple_call (dest)
           && gimple_call_lhs (dest) != NULL_TREE)
    type_d = TREE_TYPE (gimple_call_lhs (dest));
  else
    gcc_unreachable ();

  type_o = TREE_TYPE (orig);

  if (!useless_type_conversion_p (type_d, type_o))
    return false;

  return true;
}

/* Similarly, but we know that we're propagating into an ASM_EXPR.  */

bool
may_propagate_copy_into_asm (tree dest ATTRIBUTE_UNUSED)
{
  return true;
}


/* Common code for propagate_value and replace_exp.

   Replace use operand OP_P with VAL.  FOR_PROPAGATION indicates if the
   replacement is done to propagate a value or not.  */

static void
replace_exp_1 (use_operand_p op_p, tree val,
    	       bool for_propagation ATTRIBUTE_UNUSED)
{
#if defined ENABLE_CHECKING
  tree op = USE_FROM_PTR (op_p);

  gcc_assert (!(for_propagation
		&& TREE_CODE (op) == SSA_NAME
		&& TREE_CODE (val) == SSA_NAME
		&& !may_propagate_copy (op, val)));
#endif

  if (TREE_CODE (val) == SSA_NAME)
    SET_USE (op_p, val);
  else
    SET_USE (op_p, unshare_expr (val));
}


/* Propagate the value VAL (assumed to be a constant or another SSA_NAME)
   into the operand pointed to by OP_P.

   Use this version for const/copy propagation as it will perform additional
   checks to ensure validity of the const/copy propagation.  */

void
propagate_value (use_operand_p op_p, tree val)
{
  replace_exp_1 (op_p, val, true);
}

/* Replace *OP_P with value VAL (assumed to be a constant or another SSA_NAME).

   Use this version when not const/copy propagating values.  For example,
   PRE uses this version when building expressions as they would appear
   in specific blocks taking into account actions of PHI nodes.

   The statement in which an expression has been replaced should be
   folded using fold_stmt_inplace.  */

void
replace_exp (use_operand_p op_p, tree val)
{
  replace_exp_1 (op_p, val, false);
}


/* Propagate the value VAL (assumed to be a constant or another SSA_NAME)
   into the tree pointed to by OP_P.

   Use this version for const/copy propagation when SSA operands are not
   available.  It will perform the additional checks to ensure validity of
   the const/copy propagation, but will not update any operand information.
   Be sure to mark the stmt as modified.  */

void
propagate_tree_value (tree *op_p, tree val)
{
  if (TREE_CODE (val) == SSA_NAME)
    *op_p = val;
  else
    *op_p = unshare_expr (val);
}


/* Like propagate_tree_value, but use as the operand to replace
   the principal expression (typically, the RHS) contained in the
   statement referenced by iterator GSI.  Note that it is not
   always possible to update the statement in-place, so a new
   statement may be created to replace the original.  */

void
propagate_tree_value_into_stmt (gimple_stmt_iterator *gsi, tree val)
{
  gimple stmt = gsi_stmt (*gsi);

  if (is_gimple_assign (stmt))
    {
      tree expr = NULL_TREE;
      if (gimple_assign_single_p (stmt))
        expr = gimple_assign_rhs1 (stmt);
      propagate_tree_value (&expr, val);
      gimple_assign_set_rhs_from_tree (gsi, expr);
    }
  else if (gcond *cond_stmt = dyn_cast <gcond *> (stmt))
    {
      tree lhs = NULL_TREE;
      tree rhs = build_zero_cst (TREE_TYPE (val));
      propagate_tree_value (&lhs, val);
      gimple_cond_set_code (cond_stmt, NE_EXPR);
      gimple_cond_set_lhs (cond_stmt, lhs);
      gimple_cond_set_rhs (cond_stmt, rhs);
    }
  else if (is_gimple_call (stmt)
           && gimple_call_lhs (stmt) != NULL_TREE)
    {
      tree expr = NULL_TREE;
      bool res;
      propagate_tree_value (&expr, val);
      res = update_call_from_tree (gsi, expr);
      gcc_assert (res);
    }
  else if (gswitch *swtch_stmt = dyn_cast <gswitch *> (stmt))
    propagate_tree_value (gimple_switch_index_ptr (swtch_stmt), val);
  else
    gcc_unreachable ();
}
