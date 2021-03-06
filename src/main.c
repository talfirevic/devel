/*
 * main.c
 *
 * Entrypoint of PG-Strom extension, and misc uncategolized functions.
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/hash.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/ruleutils.h"
#include <float.h>
#include <limits.h>
#include "pg_strom.h"

PG_MODULE_MAGIC;

/*
 * Misc static variables
 */
static planner_hook_type	planner_hook_next;
static HTAB				   *pgstrom_path_htab = NULL;

/*
 * miscellaneous GUC parameters
 */
bool		pgstrom_enabled;
bool		pgstrom_perfmon_enabled;
static bool	pgstrom_debug_kernel_source;
bool		pgstrom_bulkload_enabled;
double		pgstrom_bulkload_density;
int			pgstrom_max_async_tasks;

/* cost factors */
double		pgstrom_gpu_setup_cost;
double		pgstrom_gpu_operator_cost;
double		pgstrom_gpu_tuple_cost;

/* buffer usage */
double		pgstrom_nrows_growth_ratio_limit;
double		pgstrom_nrows_growth_margin;

static void
pgstrom_init_misc_guc(void)
{
	/* turn on/off PG-Strom feature */
	DefineCustomBoolVariable("pg_strom.enabled",
							 "Enables the planner's use of PG-Strom",
							 NULL,
							 &pgstrom_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off performance monitor on EXPLAIN ANALYZE */
	DefineCustomBoolVariable("pg_strom.perfmon",
							 "Enables the performance monitor of PG-Strom",
							 NULL,
							 &pgstrom_perfmon_enabled,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off bulkload feature to exchange PG-Strom nodes */
	DefineCustomBoolVariable("pg_strom.bulkload_enabled",
							 "Enables the bulk-loading mode of PG-Strom",
							 NULL,
							 &pgstrom_bulkload_enabled,
							 true,
							 PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/* threshold of bulkload density */
	DefineCustomRealVariable("pg_strom.bulkload_density",
							 "Threshold to use bulkload for data exchange",
							 NULL,
							 &pgstrom_bulkload_density,
							 0.50,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off cuda kernel source saving */
	DefineCustomBoolVariable("pg_strom.debug_kernel_source",
							 "Turn on/off to display the kernel source path",
							 NULL,
							 &pgstrom_debug_kernel_source,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* maximum number of GpuTask can concurrently executed */
	DefineCustomIntVariable("pg_strom.max_async_tasks",
							"max number of GPU tasks to be run asynchronously",
							NULL,
							&pgstrom_max_async_tasks,
							32,
							4,
							INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	/* cost factor for Gpu setup */
	DefineCustomRealVariable("pg_strom.gpu_setup_cost",
							 "Cost to setup GPU device to run",
							 NULL,
							 &pgstrom_gpu_setup_cost,
							 1000 * DEFAULT_SEQ_PAGE_COST,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* cost factor for Gpu operator */
	DefineCustomRealVariable("pg_strom.gpu_operator_cost",
							 "Cost of processing each operators by GPU",
							 NULL,
							 &pgstrom_gpu_operator_cost,
							 DEFAULT_CPU_OPERATOR_COST / 100.0,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* cost factor to process tuples in Gpu */
	DefineCustomRealVariable("pg_strom.gpu_tuple_cost",
							 "Cost of processing each tuple for GPU",
							 NULL,
							 &pgstrom_gpu_tuple_cost,
							 DEFAULT_CPU_TUPLE_COST / 32,
							 0,
							 DBL_MAX,
                             PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/* max ratio (hard limit) for row population */
	DefineCustomRealVariable("pg_strom.nrows_growth_ratio_limit",
							 "Hard limit of rows growth ratio",
							 NULL,
							 &pgstrom_nrows_growth_ratio_limit,
							 12.0,
							 0,
							 DBL_MAX,
                             PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/* safety margin on row population */
	DefineCustomRealVariable("pg_strom.nrows_growth_margin",
							 "Safety margin if logic may grows # of rows",
							 NULL,
							 &pgstrom_nrows_growth_margin,
							 0.25,
							 0.0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
}

/*
 *
 *
 */
typedef struct
{
	PlannerInfo	   *root;		/* hash key 1 -- must be first */
	Relids			relids;		/* hash key 2 -- must be first */
	CustomPath	   *cpath;
} pgstrom_path_tracker;

static uint32
path_tracker_hash(const void *key, Size keysize)
{
	pgstrom_path_tracker   *hentry = (pgstrom_path_tracker *) key;
	uint32		hash;

	Assert(keysize == offsetof(pgstrom_path_tracker, cpath));
	hash = hash_any((unsigned char *)&hentry->root, sizeof(PlannerInfo *));
	hash ^= bms_hash_value(hentry->relids);

	return hash;
}

static int
path_tracker_match(const void *key1, const void *key2, Size keysize)
{
	pgstrom_path_tracker   *hentry1 = (pgstrom_path_tracker *) key1;
	pgstrom_path_tracker   *hentry2 = (pgstrom_path_tracker *) key2;

	Assert(keysize == offsetof(pgstrom_path_tracker, cpath));
	if (hentry1->root == hentry2->root &&
		bms_equal(hentry1->relids, hentry2->relids))
	{
		return 0;	/* matched */
	}
	return 1;		/* not matched */
}

/*
 * pgstrom_track_path
 *
 * It tracks self managed CustomPath node, for optimal construction.
 */
void
pgstrom_add_path(PlannerInfo *root, RelOptInfo *rel,
				 CustomPath *cpath, Size cpath_length)
{
	pgstrom_path_tracker   *hentry;
	pgstrom_path_tracker	hkey;
	bool					found;

	/*
	 * Create own Path hash-table, if first time
	 */
	if (!pgstrom_path_htab)
	{
		HASHCTL		hash_ctl;

		/* Create the hash table */
		MemSet(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = offsetof(pgstrom_path_tracker, cpath);
		hash_ctl.entrysize = sizeof(pgstrom_path_tracker);
		hash_ctl.hash = path_tracker_hash;
		hash_ctl.match = path_tracker_match;
		hash_ctl.hcxt = CurrentMemoryContext;
		pgstrom_path_htab = hash_create("PgStromPathHashTable",
										256L,
										&hash_ctl,
										HASH_ELEM | HASH_FUNCTION |
										HASH_COMPARE | HASH_CONTEXT);
	}

	/*
	 * Find a hash entry, and tracks this CustomPath node regardless
	 * of the cost (because mergeable GpuJoin may offer cheaper total
	 * cost later)
	 */
	memset(&hkey, 0, sizeof(pgstrom_path_tracker));
	hkey.root = root;
	hkey.relids = rel->relids;
	hentry = (pgstrom_path_tracker *)
		hash_search(pgstrom_path_htab, &hkey, HASH_ENTER, &found);
	if (!found)
	{
		Assert(hentry->root == root &&
			   bms_equal(hentry->relids, rel->relids));
		hentry->cpath = palloc(cpath_length);
		memcpy(hentry->cpath, cpath, cpath_length);
	}
	else
	{
		/* check total_cost of Path, then cheaper one will servive */
		if (hentry->cpath->path.total_cost > cpath->path.total_cost)
		{
			hentry->cpath = palloc(cpath_length);
			memcpy(hentry->cpath, cpath, cpath_length);
		}
	}

	/*
	 * Finally, add this CustomPath node to the target relation.
	 *
	 * NOTE: If other path dominates this CustomPath node, add_path
	 * may release the supplied CustomPath immediately. So, we have
	 * to track CustomPath node prior to add_path().
	 */
	add_path(rel, &cpath->path);
}

/*
 * pgstrom_find_path
 *
 * It tries to find tracked CustomPath node.
 */
CustomPath *
pgstrom_find_path(PlannerInfo *root, RelOptInfo *rel)
{
	/*
	 * Find the required hash entry, if any
	 */
	if (pgstrom_path_htab)
	{
		pgstrom_path_tracker   *hentry;
		pgstrom_path_tracker	hkey;

		memset(&hkey, 0, sizeof(pgstrom_path_tracker));
		hkey.root = root;
		hkey.relids = rel->relids;
		hentry = (pgstrom_path_tracker *)
			hash_search(pgstrom_path_htab, &hkey, HASH_FIND, NULL);
		if (hentry)
			return hentry->cpath;
	}
	return NULL;
}

/*
 * pgstrom_recursive_grafter
 *
 * It tries to inject GpuPreAgg and GpuSort (these are not "officially"
 * supported by planner) on the pre-built plan tree.
 */
static void
pgstrom_recursive_grafter(PlannedStmt *pstmt, Plan **p_curr_plan)
{
	Plan	   *plan = *p_curr_plan;
	ListCell   *lc;

	Assert(plan != NULL);

	switch (nodeTag(plan))
	{
		case T_Agg:
			/*
			 * Try to inject GpuPreAgg plan if cost of the aggregate plan
			 * is enough expensive to justify preprocess by GPU.
			 */
			pgstrom_try_insert_gpupreagg(pstmt, (Agg *) plan);
			break;

		case T_SubqueryScan:
			{
				SubqueryScan   *subquery = (SubqueryScan *) plan;
				Plan		  **p_subplan = &subquery->subplan;
				pgstrom_recursive_grafter(pstmt, p_subplan);
			}
			break;
		case T_ModifyTable:
			{
				ModifyTable *mtplan = (ModifyTable *) plan;

				foreach (lc, mtplan->plans)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		case T_Append:
			{
				Append *aplan = (Append *) plan;

				foreach (lc, aplan->appendplans)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		case T_MergeAppend:
			{
				MergeAppend *maplan = (MergeAppend *) plan;

				foreach (lc, maplan->mergeplans)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		case T_BitmapAnd:
			{
				BitmapAnd  *baplan = (BitmapAnd *) plan;

				foreach (lc, baplan->bitmapplans)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		case T_BitmapOr:
			{
				BitmapOr   *boplan = (BitmapOr *) plan;

				foreach (lc, boplan->bitmapplans)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				foreach (lc, cscan->custom_children)
				{
					Plan  **p_subplan = (Plan **) &lfirst(lc);
					pgstrom_recursive_grafter(pstmt, p_subplan);
				}
			}
			break;
		default:
			/* nothing to do, keep existgin one */
			break;
	}

	/* also walk down left and right child plan sub-tree, if any */
	if (plan->lefttree)
		pgstrom_recursive_grafter(pstmt, &plan->lefttree);
	if (plan->righttree)
		pgstrom_recursive_grafter(pstmt, &plan->righttree);

	switch (nodeTag(plan))
	{
		case T_Sort:
			/* Try to replace Sort node by GpuSort node if cost of
			 * the alternative plan is enough reasonable to replace.
			 */
			pgstrom_try_insert_gpusort(pstmt, p_curr_plan);
			break;

		default:
			/* nothing to do, keep existing one */
			break;
	}
}

/*
 * pgstrom_planner_entrypoint
 *
 * It overrides the planner_hook for two purposes.
 * 1. To inject GpuPreAgg and GpuSort on the PlannedStmt once built.
 *    (Note that it is not a usual way to inject paths, so we have
 *     to be careful to inject it)
 * 2. To initialize the hash table to save GPU accelerated Paths to
 *    investigate N-way join, even if intermediation path was not
 *    the cheapest one.
 */
static PlannedStmt *
pgstrom_planner_entrypoint(Query *parse,
						   int cursorOptions,
						   ParamListInfo boundParams)
{
	PlannedStmt	*result;
	HTAB		*pgstrom_path_htab_saved = pgstrom_path_htab;

	PG_TRY();
	{
		pgstrom_path_htab = NULL;

		if (planner_hook_next)
			result = planner_hook_next(parse, cursorOptions, boundParams);
		else
			result = standard_planner(parse, cursorOptions, boundParams);

		if (pgstrom_enabled)
		{
			ListCell   *cell;

			Assert(result->planTree != NULL);
			pgstrom_recursive_grafter(result, &result->planTree);

			foreach (cell, result->subplans)
			{
				Plan  **p_subplan = (Plan **) &cell->data.ptr_value;
				pgstrom_recursive_grafter(result, p_subplan);
			}
		}
	}
	PG_CATCH();
	{
		pgstrom_path_htab = pgstrom_path_htab_saved;
		PG_RE_THROW();
	}
	PG_END_TRY();
	pgstrom_path_htab = pgstrom_path_htab_saved;

	return result;
}

/*
 * _PG_init
 *
 * Main entrypoint of PG-Strom. It shall be invoked only once when postmaster
 * process is starting up, then it calls other sub-systems to initialize for
 * each ones.
 */
void
_PG_init(void)
{
	/*
	 * PG-Strom has to be loaded using shared_preload_libraries option
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		errmsg("PG-Strom must be loaded via shared_preload_libraries")));

	/* initialization of CUDA related stuff */
	pgstrom_init_cuda_control();
	pgstrom_init_cuda_program();
	/* initialization of data store support */
	pgstrom_init_datastore();

	/* registration of custom-scan providers */
	pgstrom_init_gpuscan();
	pgstrom_init_gpujoin();
	pgstrom_init_gpupreagg();
	pgstrom_init_gpusort();

	/* miscellaneous initializations */
	pgstrom_init_misc_guc();
	pgstrom_init_codegen();

	/* overall planner hook registration */
	planner_hook_next = planner_hook;
	planner_hook = pgstrom_planner_entrypoint;
}

/* ------------------------------------------------------------
 *
 * Misc routines to support EXPLAIN command
 *
 * ------------------------------------------------------------
 */
void
show_scan_qual(List *qual, const char *qlabel,
               PlanState *planstate, List *ancestors,
               ExplainState *es)
{
	bool        useprefix;
	Node	   *node;
	List       *context;
	char       *exprstr;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* Set up deparsing context */
	context = set_deparse_context_planstate(es->deparse_cxt,
											(Node *) planstate,
											ancestors);
	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * "which" identifies which instrumentation counter to print
 */
void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* In text mode, suppress zero counts; they're not interesting enough */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, 0.0, 0, es);
	}
}

void
pgstrom_accum_perfmon(pgstrom_perfmon *accum, const pgstrom_perfmon *pfm)
{
	if (!accum->enabled)
		return;

	accum->num_samples++;
	accum->time_inner_load		+= pfm->time_inner_load;
	accum->time_outer_load		+= pfm->time_outer_load;
	accum->time_materialize		+= pfm->time_materialize;
	accum->time_launch_cuda		+= pfm->time_launch_cuda;
	accum->time_sync_tasks		+= pfm->time_sync_tasks;
	accum->num_dma_send			+= pfm->num_dma_send;
	accum->num_dma_recv			+= pfm->num_dma_recv;
	accum->bytes_dma_send		+= pfm->bytes_dma_send;
	accum->bytes_dma_recv		+= pfm->bytes_dma_recv;
	accum->time_dma_send		+= pfm->time_dma_send;
	accum->time_dma_recv		+= pfm->time_dma_recv;
	/* in case of gpuscan */
	accum->num_kern_qual		+= pfm->num_kern_qual;
	accum->time_kern_qual		+= pfm->time_kern_qual;
	/* in case of gpuhashjoin */
	accum->num_kern_join		+= pfm->num_kern_join;
	accum->num_kern_proj		+= pfm->num_kern_proj;
	accum->time_kern_join		+= pfm->time_kern_join;
	accum->time_kern_proj		+= pfm->time_kern_proj;
	/* in case of gpupreagg */
	accum->num_kern_prep		+= pfm->num_kern_prep;
	accum->num_kern_lagg		+= pfm->num_kern_lagg;
	accum->num_kern_gagg		+= pfm->num_kern_gagg;
	accum->num_kern_nogrp		+= pfm->num_kern_nogrp;
	accum->time_kern_prep		+= pfm->time_kern_prep;
	accum->time_kern_lagg		+= pfm->time_kern_lagg;
	accum->time_kern_gagg		+= pfm->time_kern_gagg;
	accum->time_kern_nogrp		+= pfm->time_kern_nogrp;
	/* in case of gpusort */
	accum->num_prep_sort		+= pfm->num_prep_sort;
	accum->num_gpu_sort			+= pfm->num_gpu_sort;
	accum->num_cpu_sort			+= pfm->num_cpu_sort;
	accum->time_prep_sort		+= pfm->time_prep_sort;
	accum->time_gpu_sort		+= pfm->time_gpu_sort;
	accum->time_cpu_sort		+= pfm->time_cpu_sort;
	accum->time_cpu_sort_real	+= pfm->time_cpu_sort_real;
	accum->time_cpu_sort_min	= Min(accum->time_cpu_sort_min,
									  pfm->time_cpu_sort);
	accum->time_cpu_sort_max	= Max(accum->time_cpu_sort_min,
									  pfm->time_cpu_sort);
	accum->time_bgw_sync		+= pfm->time_bgw_sync;
	/* for debug usage */
	accum->time_debug1			+= pfm->time_debug1;
	accum->time_debug2			+= pfm->time_debug2;
	accum->time_debug3			+= pfm->time_debug3;
	accum->time_debug4			+= pfm->time_debug4;
}

static char *
bytesz_unitary_format(double nbytes)
{
	if (nbytes > (double)(1UL << 43))
		return psprintf("%.2fTB", nbytes / (double)(1UL << 40));
	else if (nbytes > (double)(1UL << 33))
		return psprintf("%.2fGB", nbytes / (double)(1UL << 30));
	else if (nbytes > (double)(1UL << 23))
		return psprintf("%.2fMB", nbytes / (double)(1UL << 20));
	else if (nbytes > (double)(1UL << 13))
		return psprintf("%.2fKB", nbytes / (double)(1UL << 10));
	return psprintf("%uB", (unsigned int)nbytes);
}

static char *
milliseconds_unitary_format(double milliseconds)
{
	if (milliseconds > 300000.0)	/* more then 5min */
		return psprintf("%.2fmin", milliseconds / 60000.0);
	else if (milliseconds > 8000.0)	/* more than 8sec */
		return psprintf("%.2fsec", milliseconds / 1000.0);
	return psprintf("%.2fms", milliseconds);
}

static void
pgstrom_explain_perfmon(pgstrom_perfmon *pfm, ExplainState *es)
{
	char		buf[256];

	if (!pfm->enabled || pfm->num_samples == 0)
		return;

	/* common performance statistics */
	ExplainPropertyInteger("number of tasks", pfm->num_samples, es);

	if (pfm->time_inner_load > 0.0)
	{
		snprintf(buf, sizeof(buf), "%s",
				 milliseconds_unitary_format(pfm->time_inner_load));
		ExplainPropertyText("total time for inner load", buf, es);

		snprintf(buf, sizeof(buf), "%s",
				 milliseconds_unitary_format(pfm->time_outer_load));
		ExplainPropertyText("total time for outer load", buf, es);
	}
	else
	{
		snprintf(buf, sizeof(buf), "%s",
				 milliseconds_unitary_format(pfm->time_outer_load));
		ExplainPropertyText("total time to load", buf, es);
	}

	if (pfm->time_materialize > 0.0)
	{
		snprintf(buf, sizeof(buf), "%s",
                 milliseconds_unitary_format(pfm->time_materialize));
		ExplainPropertyText("total time to materialize", buf, es);
	}

	if (pfm->time_launch_cuda > 0.0)
	{
		snprintf(buf, sizeof(buf), "%s",
				 milliseconds_unitary_format(pfm->time_launch_cuda));
		ExplainPropertyText("total time to CUDA commands", buf, es);
	}

	if (pfm->time_sync_tasks > 0.0)
	{
		snprintf(buf, sizeof(buf), "%s",
				 milliseconds_unitary_format(pfm->time_sync_tasks));
		ExplainPropertyText("total time to synchronize", buf, es);
	}

	if (pfm->num_dma_send > 0)
	{
		double	band =
			((double)pfm->bytes_dma_send * 1000.0 / pfm->time_dma_send);
		snprintf(buf, sizeof(buf),
				 "%s/sec, len: %s, time: %s, count: %u",
				 bytesz_unitary_format(band),
				 bytesz_unitary_format((double)pfm->bytes_dma_send),
				 milliseconds_unitary_format(pfm->time_dma_send),
				 pfm->num_dma_send);
		ExplainPropertyText("DMA send", buf, es);
	}

	if (pfm->num_dma_recv > 0)
	{
		double	band =
			((double)pfm->bytes_dma_recv * 1000.0 / pfm->time_dma_recv);
		snprintf(buf, sizeof(buf),
				 "%s/sec, len: %s, time: %s, count: %u",
				 bytesz_unitary_format(band),
				 bytesz_unitary_format((double)pfm->bytes_dma_recv),
				 milliseconds_unitary_format(pfm->time_dma_recv),
				 pfm->num_dma_recv);
		ExplainPropertyText("DMA recv", buf, es);
	}

	/* in case of gpuscan */
	if (pfm->num_kern_qual > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_kern_qual),
                 milliseconds_unitary_format(pfm->time_kern_qual /
											 (double)pfm->num_kern_qual),
                 pfm->num_kern_qual);
		ExplainPropertyText("Qual kernel exec", buf, es);
	}
	/* in case of gpuhashjoin */
	if (pfm->num_kern_join > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_kern_join),
                 milliseconds_unitary_format(pfm->time_kern_join /
											 (double)pfm->num_kern_join),
                 pfm->num_kern_join);
		ExplainPropertyText("GpuJoin main kernel", buf, es);
	}

	if (pfm->num_kern_proj > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_kern_proj),
                 milliseconds_unitary_format(pfm->time_kern_proj /
											 (double)pfm->num_kern_proj),
                 pfm->num_kern_proj);
		ExplainPropertyText("GpuJoin projection", buf, es);
	}
	/* in case of gpupreagg */
	if (pfm->num_kern_prep > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_kern_prep),
				 milliseconds_unitary_format(pfm->time_kern_prep /
											 (double)pfm->num_kern_prep),
				 pfm->num_kern_prep);
        ExplainPropertyText("Aggregate preparation", buf, es);
	}

	if (pfm->num_kern_lagg > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format((double)pfm->time_kern_lagg),
				 milliseconds_unitary_format((double)pfm->time_kern_lagg /
											 (double)pfm->num_kern_lagg),
				 pfm->num_kern_lagg);
        ExplainPropertyText("Local reduction kernel", buf, es);
	}

	if (pfm->num_kern_gagg > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format((double)pfm->time_kern_gagg),
				 milliseconds_unitary_format((double)pfm->time_kern_gagg /
											 (double)pfm->num_kern_gagg),
				 pfm->num_kern_gagg);
        ExplainPropertyText("Global reduction kernel", buf, es);
	}

	if (pfm->num_kern_nogrp > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format((double)pfm->time_kern_nogrp),
				 milliseconds_unitary_format((double)pfm->time_kern_nogrp /
											 (double)pfm->num_kern_nogrp),
				 pfm->num_kern_nogrp);
        ExplainPropertyText("NoGroup reduction kernel", buf, es);
	}

	/* in case of gpusort */
	if (pfm->num_prep_sort > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_prep_sort),
				 milliseconds_unitary_format(pfm->time_prep_sort /
											 (double)pfm->num_gpu_sort),
				 pfm->num_gpu_sort);
		ExplainPropertyText("GPU sort prep", buf, es);
	}

	if (pfm->num_gpu_sort > 0)
	{
		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_gpu_sort),
				 milliseconds_unitary_format(pfm->time_gpu_sort /
											 (double)pfm->num_gpu_sort),
				 pfm->num_gpu_sort);
		ExplainPropertyText("GPU sort exec", buf, es);
	}

	if (pfm->num_cpu_sort > 0)
	{
		cl_double	overhead;

		snprintf(buf, sizeof(buf), "total: %s, avg: %s, count: %u",
				 milliseconds_unitary_format(pfm->time_cpu_sort),
				 milliseconds_unitary_format(pfm->time_cpu_sort /
											 (double)pfm->num_cpu_sort),
				 pfm->num_cpu_sort);
		ExplainPropertyText("CPU sort exec", buf, es);

		overhead = pfm->time_cpu_sort_real - pfm->time_cpu_sort;
		snprintf(buf, sizeof(buf), "overhead: %s, sync: %s",
				 milliseconds_unitary_format(overhead),
				 milliseconds_unitary_format(pfm->time_bgw_sync));
		ExplainPropertyText("BGWorker", buf, es);
	}

	/* for debugging if any */
	if (pfm->time_debug1 > 0.0)
	{
		snprintf(buf, sizeof(buf), "debug1: %s",
				 milliseconds_unitary_format(pfm->time_debug1));
		ExplainPropertyText("debug-1", buf, es);
	}
	if (pfm->time_debug2 > 0.0)
	{
		snprintf(buf, sizeof(buf), "debug2: %s",
				 milliseconds_unitary_format(pfm->time_debug2));
		ExplainPropertyText("debug-2", buf, es);
	}
	if (pfm->time_debug3 > 0.0)
	{
		snprintf(buf, sizeof(buf), "debug3: %s",
				 milliseconds_unitary_format(pfm->time_debug3));
		ExplainPropertyText("debug-3", buf, es);
	}
	if (pfm->time_debug4 > 0.0)
	{
		snprintf(buf, sizeof(buf), "debug4: %s",
				 milliseconds_unitary_format(pfm->time_debug4));
		ExplainPropertyText("debug-4", buf, es);
	}
}

/*
 * pgstrom_explain_gputaskstate
 *
 * common additional explain output for all the GpuTaskState nodes
 */
void
pgstrom_explain_gputaskstate(GpuTaskState *gts, ExplainState *es)
{
	/*
	 * Explain custom-flags
	 */
	if (es->verbose)
	{
		StringInfoData	buf;
		uint32			cflags = gts->css.flags;

		initStringInfo(&buf);

		if ((cflags & CUSTOMPATH_PREFERE_ROW_FORMAT) != 0)
			appendStringInfo(&buf, "format: heap-tuple");
		else
			appendStringInfo(&buf, "format: tuple-slot");

		if ((cflags & CUSTOMPATH_SUPPORT_BULKLOAD) != 0)
			appendStringInfo(&buf, ", bulkload: supported");
		else
			appendStringInfo(&buf, ", bulkload: unsupported");

		ExplainPropertyText("Features", buf.data, es);

		pfree(buf.data);
	}

	/*
	 * Show source path of the GPU kernel
	 */
	if (es->verbose &&
		gts->kern_source != NULL &&
		pgstrom_debug_kernel_source)
	{
		const char *cuda_source = pgstrom_cuda_source_file(gts);

		ExplainPropertyText("Kernel Source", cuda_source, es);
	}

	/*
	 * Show performance information
	 */
	if (es->analyze && gts->pfm_accum.enabled)
		pgstrom_explain_perfmon(&gts->pfm_accum, es);
}
