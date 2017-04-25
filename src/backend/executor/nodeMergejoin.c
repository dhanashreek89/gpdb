/*
 * GPDB_83_MERGE_FIXME: I left out gpmon counter stuff like this:
 *
 * Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_MERGEJOIN_OUTERTUPLE);
 * Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_QEXEC_M_ROWSIN);
 *
 * Do we really need that stuff? There are counters in the inner and outer nodes themselves.
 */


/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.c
 *	  routines supporting merge joins
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeMergejoin.c,v 1.92 2008/08/14 18:47:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMergeJoin			mergejoin outer and inner relations.
 *		ExecInitMergeJoin		creates and initializes run time states
 *		ExecEndMergeJoin		cleans up the node.
 *
 * NOTES
 *
 *		Merge-join is done by joining the inner and outer tuples satisfying
 *		join clauses of the form ((= outerKey innerKey) ...).
 *		The join clause list is provided by the query planner and may contain
 *		more than one (= outerKey innerKey) clause (for composite sort key).
 *
 *		However, the query executor needs to know whether an outer
 *		tuple is "greater/smaller" than an inner tuple so that it can
 *		"synchronize" the two relations. For example, consider the following
 *		relations:
 *
 *				outer: (0 ^1 1 2 5 5 5 6 6 7)	current tuple: 1
 *				inner: (1 ^3 5 5 5 5 6)			current tuple: 3
 *
 *		To continue the merge-join, the executor needs to scan both inner
 *		and outer relations till the matching tuples 5. It needs to know
 *		that currently inner tuple 3 is "greater" than outer tuple 1 and
 *		therefore it should scan the outer relation first to find a
 *		matching tuple and so on.
 *
 *		Therefore, rather than directly executing the merge join clauses,
 *		we evaluate the left and right key expressions separately and then
 *		compare the columns one at a time (see MJCompare).	The planner
 *		passes us enough information about the sort ordering of the inputs
 *		to allow us to determine how to make the comparison.  We may use the
 *		appropriate btree comparison function, since Postgres' only notion
 *		of ordering is specified by btree opfamilies.
 *
 *
 *		Consider the above relations and suppose that the executor has
 *		just joined the first outer "5" with the last inner "5". The
 *		next step is of course to join the second outer "5" with all
 *		the inner "5's". This requires repositioning the inner "cursor"
 *		to point at the first inner "5". This is done by "marking" the
 *		first inner 5 so we can restore the "cursor" to it before joining
 *		with the second outer 5. The access method interface provides
 *		routines to mark and restore to a tuple.
 *
 *
 *		Essential operation of the merge join algorithm is as follows:
 *
 *		Join {
 *			get initial outer and inner tuples				INITIALIZE
 *			do forever {
 *				while (outer != inner) {					SKIP_TEST
 *					if (outer < inner)
 *						advance outer						SKIPOUTER_ADVANCE
 *					else
 *						advance inner						SKIPINNER_ADVANCE
 *				}
 *				mark inner position							SKIP_TEST
 *				do forever {
 *					while (outer == inner) {
 *						join tuples							JOINTUPLES
 *						advance inner position				NEXTINNER
 *					}
 *					advance outer position					NEXTOUTER
 *					if (outer == mark)						TESTOUTER
 *						restore inner position to mark		TESTOUTER
 *					else
 *						break	// return to top of outer loop
 *				}
 *			}
 *		}
 *
 *		The merge join operation is coded in the fashion
 *		of a state machine.  At each state, we do something and then
 *		proceed to another state.  This state is stored in the node's
 *		execution state information and is preserved across calls to
 *		ExecMergeJoin. -cim 10/31/89
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "cdb/cdbvars.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/nodeMergejoin.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * Runtime data for each mergejoin clause
 */
typedef struct MergeJoinClauseData
{
	/* Executable expression trees */
	ExprState  *lexpr;			/* left-hand (outer) input expression */
	ExprState  *rexpr;			/* right-hand (inner) input expression */

	/*
	 * If we have a current left or right input tuple, the values of the
	 * expressions are loaded into these fields:
	 */
	Datum		ldatum;			/* current left-hand value */
	Datum		rdatum;			/* current right-hand value */
	bool		lisnull;		/* and their isnull flags */
	bool		risnull;

	/*
	 * CDB: Remember whether the mergejoin operation was actually an "is
	 *      not distinct from" predicate.
	 */
	bool		notdistinct;

	/*
	 * The comparison strategy in use, and the lookup info to let us call the
	 * btree comparison support function.
	 */
	bool		reverse;		/* if true, negate the cmpfn's output */
	bool		nulls_first;	/* if true, nulls sort low */
	FmgrInfo	cmpfinfo;
} MergeJoinClauseData;

/* Result type for MJEvalOuterValues and MJEvalInnerValues */
typedef enum
{
	MJEVAL_MATCHABLE,			/* normal, potentially matchable tuple */
	MJEVAL_NONMATCHABLE,		/* tuple cannot join because it has a null */
	MJEVAL_ENDOFJOIN			/* end of input (physical or effective) */
} MJEvalResult;


#define MarkInnerTuple(innerTupleSlot, mergestate) \
	ExecCopySlot((mergestate)->mj_MarkedTupleSlot, (innerTupleSlot))


/*
 * MJExamineQuals
 *
 * This deconstructs the list of mergejoinable expressions, which is given
 * to us by the planner in the form of a list of "leftexpr = rightexpr"
 * expression trees in the order matching the sort columns of the inputs.
 * We build an array of MergeJoinClause structs containing the information
 * we will need at runtime.  Each struct essentially tells us how to compare
 * the two expressions from the original clause.
 *
 * In addition to the expressions themselves, the planner passes the btree
 * opfamily OID, btree strategy number (BTLessStrategyNumber or
 * BTGreaterStrategyNumber), and nulls-first flag that identify the intended
 * sort ordering for each merge key.  The mergejoinable operator is an
 * equality operator in this opfamily, and the two inputs are guaranteed to be
 * ordered in either increasing or decreasing (respectively) order according
 * to this opfamily, with nulls at the indicated end of the range.	This
 * allows us to obtain the needed comparison function from the opfamily.
 *
 * CDB: We also recognize the "is not distinct from" predicate which is
 *      interesting for sequential window plans.  The pseudo-Lisp for this
 *      predicate is (BoolExpr_NOT (DistinctExpr_= leftexpr rightexpr)).
 */
static MergeJoinClause
MJExamineQuals(List *mergeclauses,
			   Oid *mergefamilies,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   PlanState *parent)
{
	MergeJoinClause clauses;
	int			nClauses = list_length(mergeclauses);
	int			iClause;
	ListCell   *cl;

	clauses = (MergeJoinClause) palloc0(nClauses * sizeof(MergeJoinClauseData));

	iClause = 0;
	foreach(cl, mergeclauses)
	{
		OpExpr	   *qual = (OpExpr *) lfirst(cl);
		MergeJoinClause clause = &clauses[iClause];
		Oid			opfamily = mergefamilies[iClause];
		StrategyNumber opstrategy = mergestrategies[iClause];
		bool		nulls_first = mergenullsfirst[iClause];
		int			op_strategy;
		Oid			op_lefttype;
		Oid			op_righttype;
		bool		op_recheck;
		RegProcedure cmpproc;
		AclResult	aclresult;

		if (!IsA(qual, OpExpr))
		{
			BoolExpr *bx = (BoolExpr*)qual;
			bool ok = false;
			
			if ( IsA(bx, BoolExpr) && bx->boolop == NOT_EXPR && list_length(bx->args) == 1 )
			{
				DistinctExpr *dx = (DistinctExpr*)linitial(bx->args);
				
				if ( IsA(dx, DistinctExpr) )
				{
					clause->notdistinct = true;
					qual = (OpExpr *)dx;
					ok = true;
				}
			}
			if (!ok)
				elog(ERROR, "mergejoin clause is not an OpExpr");
		}

		/*
		 * Prepare the input expressions for execution.
		 */
		clause->lexpr = ExecInitExpr((Expr *) linitial(qual->args), parent);
		clause->rexpr = ExecInitExpr((Expr *) lsecond(qual->args), parent);

		/* Extract the operator's declared left/right datatypes */
		get_op_opfamily_properties(qual->opno, opfamily,
								   &op_strategy,
								   &op_lefttype,
								   &op_righttype,
								   &op_recheck);
		if (op_strategy != BTEqualStrategyNumber)		/* should not happen */
			elog(ERROR, "cannot merge using non-equality operator %u",
				 qual->opno);
		Assert(!op_recheck);	/* never true for btree */

		/* And get the matching support procedure (comparison function) */
		cmpproc = get_opfamily_proc(opfamily,
									op_lefttype,
									op_righttype,
									BTORDER_PROC);
		if (!RegProcedureIsValid(cmpproc))		/* should not happen */
			elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
				 BTORDER_PROC, op_lefttype, op_righttype, opfamily);

		/* Check permission to call cmp function */
		aclresult = pg_proc_aclcheck(cmpproc, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(cmpproc));

		/* Set up the fmgr lookup information */
		fmgr_info(cmpproc, &(clause->cmpfinfo));

		/* Fill the additional comparison-strategy flags */
		if (opstrategy == BTLessStrategyNumber)
			clause->reverse = false;
		else if (opstrategy == BTGreaterStrategyNumber)
			clause->reverse = true;
		else	/* planner screwed up */
			elog(ERROR, "unsupported mergejoin strategy %d", opstrategy);

		clause->nulls_first = nulls_first;

		iClause++;
	}

	return clauses;
}

/*
 * MJEvalOuterValues
 *
 * Compute the values of the mergejoined expressions for the current
 * outer tuple.  We also detect whether it's impossible for the current
 * outer tuple to match anything --- this is true if it yields a NULL
 * input, since we assume mergejoin operators are strict.  If the NULL
 * is in the first join column, and that column sorts nulls last, then
 * we can further conclude that no following tuple can match anything
 * either, since they must all have nulls in the first column.  However,
 * that case is only interesting if we're not in FillOuter mode, else
 * we have to visit all the tuples anyway.
 *
 * For the convenience of callers, we also make this routine responsible
 * for testing for end-of-input (null outer tuple), and returning
 * MJEVAL_ENDOFJOIN when that's seen.  This allows the same code to be used
 * for both real end-of-input and the effective end-of-input represented by
 * a first-column NULL.
 *
 * We evaluate the values in OuterEContext, which can be reset each
 * time we move to a new tuple.
 */
static MJEvalResult
MJEvalOuterValues(MergeJoinState *mergestate)
{
	ExprContext *econtext = mergestate->mj_OuterEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* Check for end of outer subplan */
	if (TupIsNull(mergestate->mj_OuterTupleSlot))
		return MJEVAL_ENDOFJOIN;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_outertuple = mergestate->mj_OuterTupleSlot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->ldatum = ExecEvalExpr(clause->lexpr, econtext,
									  &clause->lisnull, NULL);
		if (clause->lisnull && !clause->notdistinct)
		{
			/* match is impossible; can we end the join early? */
			if (i == 0 && !clause->nulls_first && !mergestate->mj_FillOuter)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJEvalInnerValues
 *
 * Same as above, but for the inner tuple.	Here, we have to be prepared
 * to load data from either the true current inner, or the marked inner,
 * so caller must tell us which slot to load from.
 */
static MJEvalResult
MJEvalInnerValues(MergeJoinState *mergestate, TupleTableSlot *innerslot)
{
	ExprContext *econtext = mergestate->mj_InnerEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* Check for end of inner subplan */
	if (TupIsNull(innerslot))
		return MJEVAL_ENDOFJOIN;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_innertuple = innerslot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->rdatum = ExecEvalExpr(clause->rexpr, econtext,
									  &clause->risnull, NULL);
		if (clause->risnull && !clause->notdistinct)
		{
			/* match is impossible; can we end the join early? */
			if (i == 0 && !clause->nulls_first && !mergestate->mj_FillInner)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJCompare
 *
 * Compare the mergejoinable values of the current two input tuples
 * and return 0 if they are equal (ie, the mergejoin equalities all
 * succeed), +1 if outer > inner, -1 if outer < inner.
 *
 * MJEvalOuterValues and MJEvalInnerValues must already have been called
 * for the current outer and inner tuples, respectively.
 */
static int32
MJCompare(MergeJoinState *mergestate)
{
	int32		result = 0;
	bool		nulleqnull = false;
	ExprContext *econtext = mergestate->js.ps.ps_ExprContext;
	int			i;
	MemoryContext oldContext;
	FunctionCallInfoData fcinfo;

	/*
	 * Call the comparison functions in short-lived context, in case they leak
	 * memory.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];
		Datum		fresult;

		/*
		 * Deal with null inputs.
		 */
		if (clause->lisnull)
		{
			if (clause->risnull)
			{
				nulleqnull = !clause->notdistinct;	/* NULL "=" NULL */
				continue;
			}
			if (clause->nulls_first)
				result = -1;	/* NULL "<" NOT_NULL */
			else
				result = 1;		/* NULL ">" NOT_NULL */
			break;
		}
		if (clause->risnull)
		{
			if (clause->nulls_first)
				result = 1;		/* NOT_NULL ">" NULL */
			else
				result = -1;	/* NOT_NULL "<" NULL */
			break;
		}

		/*
		 * OK to call the comparison function.
		 */
		InitFunctionCallInfoData(fcinfo, &(clause->cmpfinfo), 2,
								 NULL, NULL);
		fcinfo.arg[0] = clause->ldatum;
		fcinfo.arg[1] = clause->rdatum;
		fcinfo.argnull[0] = false;
		fcinfo.argnull[1] = false;
		fresult = FunctionCallInvoke(&fcinfo);
		if (fcinfo.isnull)
		{
			nulleqnull = true;	/* treat like NULL = NULL */
			continue;
		}
		result = DatumGetInt32(fresult);

		if (clause->reverse)
			result = -result;

		if (result != 0)
			break;
	}

	/*
	 * If we had any null comparison results or NULL-vs-NULL inputs, we do not
	 * want to report that the tuples are equal.  Instead, if result is still
	 * 0, change it to +1.	This will result in advancing the inner side of
	 * the join.
	 */
	if (nulleqnull && result == 0)
		result = 1;

	MemoryContextSwitchTo(oldContext);

	return result;
}


/*
 * Generate a fake join tuple with nulls for the inner tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillOuter(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	List	   *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_OuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_NullInnerTupleSlot;

	if (TupIsNull(node->mj_OuterTupleSlot))
		return NULL;

	if (ExecQual(otherqual, econtext, false))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		MJ_printf("ExecMergeJoin: returning outer fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo, NULL);
	}

	return NULL;
}

/*
 * Generate a fake join tuple with nulls for the outer tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillInner(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	List	   *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	/* If we don't have an inner, return NULL */
	if(TupIsNull(node->mj_InnerTupleSlot))
		return NULL;

	econtext->ecxt_outertuple = node->mj_NullOuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_InnerTupleSlot;

	if (ExecQual(otherqual, econtext, false))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		MJ_printf("ExecMergeJoin: returning inner fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo, NULL);
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *		ExecMergeTupleDump
 *
 *		This function is called through the MJ_dump() macro
 *		when EXEC_MERGEJOINDEBUG is defined
 * ----------------------------------------------------------------
 */
#ifdef EXEC_MERGEJOINDEBUG

static void
ExecMergeTupleDumpOuter(MergeJoinState *mergestate)
{
	TupleTableSlot *outerSlot = mergestate->mj_OuterTupleSlot;

	printf("==== outer tuple ====\n");
	if (TupIsNull(outerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(outerSlot);
}

static void
ExecMergeTupleDumpInner(MergeJoinState *mergestate)
{
	TupleTableSlot *innerSlot = mergestate->mj_InnerTupleSlot;

	printf("==== inner tuple ====\n");
	if (TupIsNull(innerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(innerSlot);
}

static void
ExecMergeTupleDumpMarked(MergeJoinState *mergestate)
{
	TupleTableSlot *markedSlot = mergestate->mj_MarkedTupleSlot;

	printf("==== marked tuple ====\n");
	if (TupIsNull(markedSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(markedSlot);
}

static void
ExecMergeTupleDump(MergeJoinState *mergestate)
{
	printf("******** ExecMergeTupleDump ********\n");

	ExecMergeTupleDumpOuter(mergestate);
	ExecMergeTupleDumpInner(mergestate);
	ExecMergeTupleDumpMarked(mergestate);

	printf("******** \n");
}
#endif

/* ----------------------------------------------------------------
 *		ExecMergeJoin
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMergeJoin(MergeJoinState *node)
{
	EState	   *estate;
	List	   *joinqual;
	List	   *otherqual;
	bool		qualResult;
	int32		compareResult;
	PlanState  *innerPlan;
	TupleTableSlot *innerTupleSlot;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	ExprContext *econtext;
	bool		doFillOuter;
	bool		doFillInner;

	/*
	 * get information from node
	 */
	estate = node->js.ps.state;
	innerPlan = innerPlanState(node);
	outerPlan = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	doFillOuter = node->mj_FillOuter;
	doFillInner = node->mj_FillInner;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't happen
	 * until we're done projecting out tuples from a join tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * MPP-4165: My fix for MPP-3300 was correct in that we avoided
	 * the *deadlock* but had very unexpected (and painful)
	 * performance characteristics: we basically de-pipeline and
	 * de-parallelize execution of any query which has motion below
	 * us.
	 *
	 * So now prefetch_inner is set (see createplan.c) if we have *any* motion
	 * below us. If we don't have any motion, it doesn't matter.
	 *
	 * See motion_sanity_walker() for details on how a deadlock may occur.
	 */
	if (node->prefetch_inner)
	{
		innerTupleSlot = ExecProcNode(innerPlan);
          	if (!TupIsNull(innerTupleSlot))
          	{
          		Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_MERGEJOIN_INNERTUPLE);
          		Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_QEXEC_M_ROWSIN); 
          	}
		node->mj_InnerTupleSlot = innerTupleSlot;

		ExecReScan(innerPlan, econtext);
		ResetExprContext(econtext);

		node->mj_squelchInner = false; /* we will never need to Squelch the inner, we've fetched it all */
		node->prefetch_inner = false;
	}

	/*
	 * ok, everything is setup.. let's go to work
	 */
	for (;;)
	{
		MJ_dump(node);
		CheckSendPlanStateGpmonPkt(&node->js.ps);

		/*
		 * get the current state of the join and do things accordingly.
		 */
		switch (node->mj_JoinState)
		{
				/*
				 * EXEC_MJ_INITIALIZE_OUTER means that this is the first time
				 * ExecMergeJoin() has been called and so we have to fetch the
				 * first matchable tuple for both outer and inner subplans. We
				 * do the outer side in INITIALIZE_OUTER state, then advance
				 * to INITIALIZE_INNER state for the inner subplan.
				 */
			case EXEC_MJ_INITIALIZE_OUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_OUTER\n");

				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* OK to go get the first inner tuple */
						node->mj_JoinState = EXEC_MJ_INITIALIZE_INNER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Stay in same state to fetch next outer tuple */
						if (doFillOuter)
						{
							/*
							 * Generate a fake join tuple with nulls for the
							 * inner tuple, and return it if it passes the
							 * non-join quals.
							 */
							TupleTableSlot *result;

							result = MJFillOuter(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: nothing in outer subplan\n");
						if (doFillInner)
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples. We set MatchedInner = true to
							 * force the ENDOUTER state to advance inner.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							node->mj_MatchedInner = true;
							break;
						}

						/*
						 * CDB: We'll read no more from inner subtree. To keep our
						 * sibling QEs from being starved, tell source QEs not to
						 * clog up the pipeline with our never-to-be-consumed
						 * data.
						 */
						if (node->mj_squelchInner)
							ExecSquelchNode(innerPlan);

						/*
						 * The memory used by child nodes might not be freed because
						 * they are not eager free safe. However, when the merge join
						 * is done, we can free the memory used by the child nodes.
						 */
						ExecEagerFreeMergeJoin(node);

						/* Otherwise we're done. */
						return NULL;
				}
				break;

			case EXEC_MJ_INITIALIZE_INNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_INNER\n");

				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:
						/*
						 * OK, we have the initial tuples.	Begin by skipping
						 * non-matching tuples.
						 */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Mark before advancing, if wanted */
						if (node->mj_ExtraMarks)
							ExecMarkPos(innerPlan);
						/* Stay in same state to fetch next inner tuple */
						if (doFillInner)
						{
							/*
							 * Generate a fake join tuple with nulls for the
							 * outer tuple, and return it if it passes the
							 * non-join quals.
							 */
							TupleTableSlot *result;

							result = MJFillInner(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more inner tuples */
						MJ_printf("ExecMergeJoin: nothing in inner subplan\n");
						if (doFillOuter)
						{
							/*
							 * Need to emit left-join tuples for all outer
							 * tuples, including the one we just fetched.  We
							 * set MatchedOuter = false to force the ENDINNER
							 * state to emit first tuple before advancing
							 * outer.
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							node->mj_MatchedOuter = false;
							break;
						}

						/*
						 * CDB: We'll read no more from outer subtree. To keep our
						 * sibling QEs from being starved, tell source QEs not to
						 * clog up the pipeline with our never-to-be-consumed
						 * data.
						 */
						ExecSquelchNode(outerPlan);

						ExecEagerFreeMergeJoin(node);

						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_JOINTUPLES means we have two tuples which satisfied
				 * the merge clause so we join them and then proceed to get
				 * the next inner tuple (EXEC_MJ_NEXTINNER).
				 */
			case EXEC_MJ_JOINTUPLES:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTUPLES\n");

				/*
				 * Set the next state machine state.  The right things will
				 * happen whether we return this join tuple or just fall
				 * through to continue the state machine execution.
				 */
				node->mj_JoinState = EXEC_MJ_NEXTINNER;

				/*
				 * Check the extra qual conditions to see if we actually want
				 * to return this join tuple.  If not, can proceed with merge.
				 * We must distinguish the additional joinquals (which must
				 * pass to consider the tuples "matched" for outer-join logic)
				 * from the otherquals (which must pass before we actually
				 * return the tuple).
				 *
				 * We don't bother with a ResetExprContext here, on the
				 * assumption that we just did one while checking the merge
				 * qual.  One per tuple should be sufficient.  We do have to
				 * set up the econtext links to the tuples for ExecQual to
				 * use.
				 */
				outerTupleSlot = node->mj_OuterTupleSlot;
				econtext->ecxt_outertuple = outerTupleSlot;
				innerTupleSlot = node->mj_InnerTupleSlot;
				econtext->ecxt_innertuple = innerTupleSlot;

				if (node->js.jointype == JOIN_SEMI &&
					node->mj_MatchedOuter)
					qualResult = false;
				else
				{
					qualResult = (joinqual == NIL ||
								  ExecQual(joinqual, econtext, false));
					MJ_DEBUG_QUAL(joinqual, qualResult);
				}

				if (qualResult)
				{
					node->mj_MatchedOuter = true;
					node->mj_MatchedInner = true;

					/* In an antijoin, we never return a matched tuple */
					if (node->js.jointype == JOIN_LASJ || node->js.jointype == JOIN_ANTI)
					{
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					}
					qualResult = (otherqual == NIL ||
								  ExecQual(otherqual, econtext, false));
					MJ_DEBUG_QUAL(otherqual, qualResult);

					if (qualResult)
					{
						/*
						 * qualification succeeded.  now form the desired
						 * projection tuple and return the slot containing it.
						 */
						MJ_printf("ExecMergeJoin: returning tuple\n");

						return ExecProject(node->js.ps.ps_ProjInfo, NULL);
					}
				}
				break;

				/*
				 * EXEC_MJ_NEXTINNER means advance the inner scan to the next
				 * tuple. If the tuple is not nil, we then proceed to test it
				 * against the join qualification.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_NEXTINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTINNER\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next inner tuple, if any.  If there's none,
				 * advance to next outer tuple (which may be able to join to
				 * previously marked tuples).
				 *
				 * NB: must NOT do "extraMarks" here, since we may need to
				 * return to previously marked tuples.
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:
						/*
						 * Test the new inner tuple to see if it matches
						 * outer.
						 *
						 * If they do match, then we join them and move on to
						 * the next inner tuple (EXEC_MJ_JOINTUPLES).
						 *
						 * If they do not match then advance to next outer
						 * tuple.
						 */
						compareResult = MJCompare(node);
						MJ_DEBUG_COMPARE(compareResult);

						if (compareResult == 0)
							node->mj_JoinState = EXEC_MJ_JOINTUPLES;
						else
						{
							Assert(compareResult < 0);
							node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						}
						break;
					case MJEVAL_NONMATCHABLE:
						/*
						 * It contains a NULL and hence can't match any outer
						 * tuple, so we can skip the comparison and assume the
						 * new tuple is greater than current outer.
						 */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:
						/*
						 * No more inner tuples.  However, this might be
						 * only effective and not physical end of inner plan,
						 * so force mj_InnerTupleSlot to null to make sure we
						 * don't fetch more inner tuples.  (We need this hack
						 * because we are not transiting to a state where the
						 * inner plan is assumed to be exhausted.)
						 */
						node->mj_InnerTupleSlot = NULL;
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;

						if (((MergeJoin*)node->js.ps.plan)->unique_outer)
						{
							ExecEagerFreeMergeJoin(node);

							/* we are done */
							return NULL;
						}
						break;
				}
				break;

				/*-------------------------------------------
				 * EXEC_MJ_NEXTOUTER means
				 *
				 *				outer inner
				 * outer tuple -  5		5  - marked tuple
				 *				  5		5
				 *				  6		6  - inner tuple
				 *				  7		7
				 *
				 * we know we just bumped into the
				 * first inner tuple > current outer tuple (or possibly
				 * the end of the inner stream)
				 * so get a new outer tuple and then
				 * proceed to test it against the marked tuple
				 * (EXEC_MJ_TESTOUTER)
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 *------------------------------------------------
				 */
			case EXEC_MJ_NEXTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTOUTER\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						if (((MergeJoin*)node->js.ps.plan)->unique_outer)
							/* The current innerTuple will match with this outerTuple.*/
							node->mj_JoinState = EXEC_MJ_JOINTUPLES;
						else
							/* Go test the new tuple against the marked tuple */
							node->mj_JoinState = EXEC_MJ_TESTOUTER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Can't match, so fetch next outer tuple */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*--------------------------------------------------------
				 * EXEC_MJ_TESTOUTER If the new outer tuple and the marked
				 * tuple satisfy the merge clause then we know we have
				 * duplicates in the outer scan so we have to restore the
				 * inner scan to the marked tuple and proceed to join the
				 * new outer tuple with the inner tuples.
				 *
				 * This is the case when
				 *						  outer inner
				 *							4	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	5	  5
				 *							6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple == marked tuple
				 *
				 * If the outer tuple fails the test, then we are done
				 * with the marked tuples, and we have to look for a
				 * match to the current inner tuple.  So we will
				 * proceed to skip outer tuples until outer >= inner
				 * (EXEC_MJ_SKIP_TEST).
				 *
				 *		This is the case when
				 *
				 *						  outer inner
				 *							5	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple > marked tuple
				 *
				 *---------------------------------------------------------
				 */
			case EXEC_MJ_TESTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_TESTOUTER\n");

				/*
				 * Here we must compare the outer tuple with the marked inner
				 * tuple.  (We can ignore the result of MJEvalInnerValues,
				 * since the marked inner tuple is certainly matchable.)
				 */
				innerTupleSlot = node->mj_MarkedTupleSlot;
				(void) MJEvalInnerValues(node, innerTupleSlot);

				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					/*
					 * the merge clause matched so now we restore the inner
					 * scan position to the first mark, and go join that tuple
					 * (and any following ones) to the new outer.
					 *
					 * NOTE: we do not need to worry about the MatchedInner
					 * state for the rescanned inner tuples.  We know all of
					 * them will match this new outer tuple and therefore
					 * won't be emitted as fill tuples.  This works *only*
					 * because we require the extra joinquals to be nil when
					 * doing a right or full join --- otherwise some of the
					 * rescanned tuples might fail the extra joinquals.
					 */
					ExecRestrPos(innerPlan);

					/*
					 * ExecRestrPos probably should give us back a new Slot,
					 * but since it doesn't, use the marked slot.  (The
					 * previously returned mj_InnerTupleSlot cannot be assumed
					 * to hold the required tuple.)
					 */
					node->mj_InnerTupleSlot = innerTupleSlot;
					/* we need not do MJEvalInnerValues again */

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else
				{
					/* ----------------
					 *	if the new outer tuple didn't match the marked inner
					 *	tuple then we have a case like:
					 *
					 *			 outer inner
					 *			   4	 4	- marked tuple
					 * new outer - 5	 4
					 *			   6	 5	- inner tuple
					 *			   7
					 *
					 *	which means that all subsequent outer tuples will be
					 *	larger than our marked inner tuples.  So we need not
					 *	revisit any of the marked tuples but can proceed to
					 *	look for a match to the current inner.	If there's
					 *	no more inners, no more matches are possible.
					 * ----------------
					 */
					if (compareResult <= 0 && !((MergeJoin*)node->js.ps.plan)->unique_outer)
						elog(ERROR, "Mergejoin: compareResult > 0, bad plan ?");
					innerTupleSlot = node->mj_InnerTupleSlot;

					/* reload comparison data for current inner */
					switch (MJEvalInnerValues(node, innerTupleSlot))
					{
						case MJEVAL_MATCHABLE:
							/* proceed to compare it to the current outer */
							node->mj_JoinState = EXEC_MJ_SKIP_TEST;
							break;
						case MJEVAL_NONMATCHABLE:
							/*
							 * current inner can't possibly match any outer;
							 * better to advance the inner scan than the outer.
							 */
							node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
							break;
						case MJEVAL_ENDOFJOIN:
							/* No more inner tuples */
							if (doFillOuter)
							{
								/*
								 * Need to emit left-join tuples for remaining
								 * outer tuples.
								 */
								node->mj_JoinState = EXEC_MJ_ENDINNER;
								break;
							}

							/*
							 * CDB: We'll read no more from outer subtree. To keep
							 * our sibling QEs from being starved, tell source QEs
							 * not to clog up the pipeline with our
							 * never-to-be-consumed data.
							 */
							ExecSquelchNode(outerPlan);

							ExecEagerFreeMergeJoin(node);

							/* Otherwise we're done. */
							return NULL;
					}
				}
				break;

				/*----------------------------------------------------------
				 * EXEC_MJ_SKIP means compare tuples and if they do not
				 * match, skip whichever is lesser.
				 *
				 * For example:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple -  6		8  - inner tuple
				 *				  7    12
				 *				  8    14
				 *
				 * we have to advance the outer scan
				 * until we find the outer 8.
				 *
				 * On the other hand:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple - 12		8  - inner tuple
				 *				 14    10
				 *				 17    12
				 *
				 * we have to advance the inner scan
				 * until we find the inner 12.
				 *----------------------------------------------------------
				 */
			case EXEC_MJ_SKIP_TEST:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIP_TEST\n");

				/*
				 * before we advance, make sure the current tuples do not
				 * satisfy the mergeclauses.  If they do, then we update the
				 * marked tuple position and go join them.
				 */
				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					if (!((MergeJoin*)node->js.ps.plan)->unique_outer)
					{
						ExecMarkPos(innerPlan);

						MarkInnerTuple(node->mj_InnerTupleSlot, node);
					}

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else if (compareResult < 0)
					node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
				else
					/* compareResult > 0 */
					node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
				break;

				/*
				 * SKIPOUTER_ADVANCE: advance over an outer tuple that is
				 * known not to join to any inner tuple.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 */
			case EXEC_MJ_SKIPOUTER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPOUTER_ADVANCE\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* Go test the new tuple against the current inner */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Can't match, so fetch next outer tuple */
						node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/*
						 * CDB: We'll read no more from inner subtree. To keep our
						 * sibling QEs from being starved, tell source QEs not to
						 * clog up the pipeline with our never-to-be-consumed
						 * data.
						 */
						if (!TupIsNull(innerTupleSlot) && node->mj_squelchInner)
							ExecSquelchNode(innerPlan);

						ExecEagerFreeMergeJoin(node);

						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * SKIPINNER_ADVANCE: advance over an inner tuple that is
				 * known not to join to any outer tuple.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_SKIPINNER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPINNER_ADVANCE\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* Mark before advancing, if wanted */
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:
						/* proceed to compare it to the current outer */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/*
						 * current inner can't possibly match any outer;
						 * better to advance the inner scan than the outer.
						 */
						node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more inner tuples */
						MJ_printf("ExecMergeJoin: end of inner subplan\n");
						outerTupleSlot = node->mj_OuterTupleSlot;
						if (doFillOuter && !TupIsNull(outerTupleSlot))
						{
							/*
							 * Need to emit left-join tuples for remaining
							 * outer tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_ENDOUTER means we have run out of outer tuples, but
				 * are doing a right/full join and therefore must null-fill
				 * any remaing unmatched inner tuples.
				 */
			case EXEC_MJ_ENDOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDOUTER\n");

				Assert(doFillInner);

				if (!node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;		/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* Mark before advancing, if wanted */
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of inner subplan\n");
					ExecEagerFreeMergeJoin(node);

					return NULL;
				}
				else
				{
					Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_MERGEJOIN_INNERTUPLE);
					Gpmon_M_Incr(GpmonPktFromMergeJoinState(node), GPMON_QEXEC_M_ROWSIN); 
				}

				/* Else remain in ENDOUTER state and process next tuple. */
				break;

				/*
				 * EXEC_MJ_ENDINNER means we have run out of inner tuples, but
				 * are doing a left/full join and therefore must null- fill
				 * any remaing unmatched outer tuples.
				 */
			case EXEC_MJ_ENDINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDINNER\n");

				Assert(doFillOuter);

				if (!node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;		/* do it only once */

					result = MJFillOuter(node);
					if (result)
					{
						Gpmon_M_Incr_Rows_Out(GpmonPktFromMergeJoinState(node));
                               	CheckSendPlanStateGpmonPkt(&node->js.ps);
						return result;
					}
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of outer subplan\n");

					ExecEagerFreeMergeJoin(node);

					return NULL;
				}

				/* Else remain in ENDINNER state and process next tuple. */
				break;

				/*
				 * broken state value?
				 */
			default:
				elog(ERROR, "unrecognized mergejoin state: %d",
					 (int) node->mj_JoinState);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitMergeJoin
 * ----------------------------------------------------------------
 */
MergeJoinState *
ExecInitMergeJoin(MergeJoin *node, EState *estate, int eflags)
{
	MergeJoinState *mergestate;
	int markflag;
	int rewindflag;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "initializing node");

	/*
	 * create state structure
	 */
	mergestate = makeNode(MergeJoinState);
	mergestate->js.ps.plan = (Plan *) node;
	mergestate->js.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &mergestate->js.ps);

	/*
	 * we need two additional econtexts in which we can compute the join
	 * expressions from the left and right input tuples.  The node's regular
	 * econtext won't do because it gets reset too often.
	 */
	mergestate->mj_OuterEContext = CreateExprContext(estate);
	mergestate->mj_InnerEContext = CreateExprContext(estate);

	/*
	 * initialize child expressions
	 */
	mergestate->js.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->join.plan.targetlist,
					 (PlanState *) mergestate);
	mergestate->js.ps.qual = (List *)
		ExecInitExpr((Expr *) node->join.plan.qual,
					 (PlanState *) mergestate);
	mergestate->js.jointype = node->join.jointype;
	mergestate->js.joinqual = (List *)
		ExecInitExpr((Expr *) node->join.joinqual,
					 (PlanState *) mergestate);

	mergestate->prefetch_inner = node->join.prefetch_inner;
	mergestate->mj_squelchInner = true;
	/* Prepare inner operators for rewind after the prefetch */
	rewindflag = mergestate->prefetch_inner ? EXEC_FLAG_REWIND : 0;

	/* mergeclauses are handled below */

	/*
	 * initialize child nodes
	 *
	 * inner child must support MARK/RESTORE unless the outer plan is
	 * known to have no duplicate merge keys.
	 */
	markflag = node->unique_outer ? 0 : EXEC_FLAG_MARK;
	outerPlanState(mergestate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(mergestate) = ExecInitNode(innerPlan(node), estate,
											  eflags | markflag | rewindflag);

	/*
	 * For certain types of inner child nodes, it is advantageous to issue
	 * MARK every time we advance past an inner tuple we will never return to.
	 * For other types, MARK on a tuple we cannot return to is a waste of
	 * cycles.	Detect which case applies and set mj_ExtraMarks if we want to
	 * issue "unnecessary" MARK calls.
	 *
	 * Currently, only Material wants the extra MARKs, and it will be helpful
	 * only if eflags doesn't specify REWIND.
	 */
	if (IsA(innerPlan(node), Material) &&
		(eflags & EXEC_FLAG_REWIND) == 0)
		mergestate->mj_ExtraMarks = true;
	else
		mergestate->mj_ExtraMarks = false;

#define MERGEJOIN_NSLOTS 4

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &mergestate->js.ps);

	mergestate->mj_MarkedTupleSlot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(mergestate->mj_MarkedTupleSlot,
						  ExecGetResultType(innerPlanState(mergestate)));

	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = false;
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
		case JOIN_LASJ:
		case JOIN_UNIQUE_OUTER:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = false;
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(innerPlanState(mergestate)));
			break;
		case JOIN_RIGHT:
		case JOIN_UNIQUE_INNER:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(outerPlanState(mergestate)));

			/*
			 * Can't handle right or full join with non-nil extra joinclauses.
			 * This should have been caught by planner.
			 */
			if (node->join.joinqual != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RIGHT JOIN is only supported with merge-joinable join conditions")));
			break;
		case JOIN_FULL:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(outerPlanState(mergestate)));
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
							  ExecGetResultType(innerPlanState(mergestate)));

			/*
			 * Can't handle right or full join with non-nil extra joinclauses.
			 */
			if (node->join.joinqual != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable join conditions")));
			break;
		case JOIN_LASJ_NOTIN:
			elog(ERROR, "join type not supported");
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&mergestate->js.ps);
	ExecAssignProjectionInfo(&mergestate->js.ps, NULL);

	/*
	 * preprocess the merge clauses
	 */
	mergestate->mj_NumClauses = list_length(node->mergeclauses);
	mergestate->mj_Clauses = MJExamineQuals(node->mergeclauses,
											node->mergeFamilies,
											node->mergeStrategies,
											node->mergeNullsFirst,
											(PlanState *) mergestate);

	/*
	 * initialize join state
	 */
	mergestate->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	mergestate->mj_MatchedOuter = false;
	mergestate->mj_MatchedInner = false;
	mergestate->mj_OuterTupleSlot = NULL;
	mergestate->mj_InnerTupleSlot = NULL;

	/*
	 * initialization successful
	 */
	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "node initialized");

	initGpmonPktForMergeJoin((Plan *)node, &mergestate->js.ps.gpmon_pkt, estate);
	
	return mergestate;
}

int
ExecCountSlotsMergeJoin(MergeJoin *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		MERGEJOIN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeJoin
 *
 * old comments
 *		frees storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndMergeJoin(MergeJoinState *node)
{
	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "ending node processing");

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->mj_MarkedTupleSlot);

	/*
	 * shut down the subplans
	 */
	ExecEndNode(innerPlanState(node));
	ExecEndNode(outerPlanState(node));

	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "node processing ended");

	EndPlanStateGpmonPkt(&node->js.ps);
}

void
ExecReScanMergeJoin(MergeJoinState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->mj_MarkedTupleSlot);

	node->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	node->mj_MatchedOuter = false;
	node->mj_MatchedInner = false;
	node->mj_OuterTupleSlot = NULL;
	node->mj_InnerTupleSlot = NULL;

	/*
	 * if chgParam of subnodes is not null then plans will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
	if (((PlanState *) node)->righttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->righttree, exprCtxt);

}
void
initGpmonPktForMergeJoin(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, MergeJoin));
	
	{
		PerfmonNodeType type = PMNT_Invalid;

		switch(((MergeJoin *)planNode)->join.jointype)
		{
			case JOIN_INNER:
				type = PMNT_MergeJoin;
				break;
			case JOIN_LEFT:
				type = PMNT_MergeLeftJoin;
				break;
			case JOIN_ANTI:
			case JOIN_LASJ:
				type = PMNT_MergeLeftAntiSemiJoin;
				break;
			case JOIN_FULL:
				type = PMNT_MergeFullJoin;
				break;
			case JOIN_RIGHT:
				type = PMNT_MergeRightJoin;
				break;
			case JOIN_SEMI:
				type = PMNT_MergeExistsJoin;
				break;
			case JOIN_REVERSE_IN:
				type = PMNT_MergeReverseInJoin;
				break;
			case JOIN_UNIQUE_OUTER:
				type = PMNT_MergeUniqueOuterJoin;
				break;
			case JOIN_UNIQUE_INNER:
				type = PMNT_MergeUniqueInnerJoin;
				break;
			case JOIN_LASJ_NOTIN:
				elog(ERROR, "Join type not supported");
				break;
		}

		Assert(type != PMNT_Invalid);
		Assert(GPMON_MERGEJOIN_TOTAL <= (int) GPMON_QEXEC_M_COUNT);
		InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, type, 
							 (int64)planNode->plan_rows,
							 NULL);
	}
}

void
ExecEagerFreeMergeJoin(MergeJoinState *node)
{
	/*
	 * Since MergeJoin might call Mark/restore on its child nodes, its child nodes
	 * are not eager free safe. We will free their memory here.
	 */
	ExecEagerFreeChildNodes((PlanState *)node, false);
}
