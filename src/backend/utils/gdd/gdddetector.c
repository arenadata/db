/*-------------------------------------------------------------------------
 *
 * gdddetector.c
 *	  Global DeadLock Detector - Detector Algorithm
 *
 *
 * Copyright (c) 2018-Present Pivotal Software, Inc.
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/procarray.h"

#include "gdddetector.h"

/***************************************************************************/

/*
 * Safe list foreach which supports in-place deletion.
 */
#define gdd_list_foreach_safe(iter, _list) \
	for ((iter).list = (_list), \
		 (iter).prev = NULL, \
		 (iter).cell = list_head((iter).list); \
		 (iter).cell != NULL; \
		 (iter).prev = (iter).cell, \
		 (iter).cell = (iter).cell ? lnext((iter).cell) : list_head((iter).list))

/*
 * Helper functions to get the information stored in iter.
 */
#define gdd_list_iter_get_list(iter) ((iter).list)
#define gdd_list_iter_get_cell(iter) ((iter).cell)
#define gdd_list_iter_get_prev(iter) ((iter).prev)

/*
 * Helper function to get the current pointer at iter.
 */
#define gdd_list_iter_get_ptr(iter) (lfirst((iter).cell))

/*
 * In-place delete the list cell at iter.
 * By using this you must re-get the list via gdd_list_iter_get_list().
 */
#define gdd_list_iter_delete(iter) do \
{ \
	(iter).list = list_delete_cell((iter).list, (iter).cell, (iter).prev); \
	(iter).prev = (iter).list != NIL ? (iter).prev : NULL; \
	(iter).cell = (iter).prev; \
} while (0)


/*
 * Safe map foreach which supports in-place deletion.
 */
#define gdd_map_foreach_safe(iter, _map) \
	for ((iter).map = (_map), (iter).pos = 0; \
		 (iter).pos < (iter).map->length; \
		 (iter).pos++)

/*
 * Get current pointer at iter.
 */
#define gdd_map_iter_get_ptr(iter) \
	((iter).map->pairs[(iter).pos].ptr)

/*
 * In-place delete the <k,v> pair at iter.
 */
#define gdd_map_iter_delete(iter) \
	(iter).map->pairs[(iter).pos--] = (iter).map->pairs[--((iter).map->length)]

/*
 * Return true if iter is not the last item.
 */
#define gdd_map_iter_has_next(iter) \
	((iter).pos + 1 < (iter).map->length)


/*
 * Foreach helpers to walk through kinds of object.
 *
 * With these helpers we can flatten some nested walk through logic,
 * but be careful on some limitations:
 *
 * - Some of these helpers are implemented with nested for loops,
 *   `break` might not work as expected, do not use it;
 * - These helpers are all safe, but only the innermost iter can be in-place
 *   deleted, do not delete iters at other levels;
 */

#define gdd_vert_foreach_in_edge(edgeiter, vert) \
	gdd_list_foreach_safe((edgeiter), (vert)->edgesIn)

#define gdd_vert_foreach_out_edge(edgeiter, vert) \
	gdd_list_foreach_safe((edgeiter), (vert)->edgesOut)


#define gdd_graph_foreach_vert(vertiter, graph) \
	gdd_map_foreach_safe((vertiter), &((graph)->verts))

#define gdd_graph_foreach_in_edge(vertiter, edgeiter, graph) \
	gdd_graph_foreach_vert((vertiter), (graph)) \
	gdd_vert_foreach_in_edge((edgeiter), \
							 (GddVert *) gdd_map_iter_get_ptr(vertiter))

#define gdd_graph_foreach_out_edge(vertiter, edgeiter, graph) \
	gdd_graph_foreach_vert((vertiter), (graph)) \
	gdd_vert_foreach_out_edge((edgeiter), \
							  (GddVert *) gdd_map_iter_get_ptr(vertiter))


#define gdd_ctx_foreach_global(globaliter, ctx) \
	gdd_map_foreach_safe((globaliter), &((ctx)->globals))

#define gdd_ctx_foreach_graph(graphiter, ctx) \
	gdd_map_foreach_safe((graphiter), &((ctx)->graphs))

#define gdd_ctx_foreach_vert(graphiter, vertiter, ctx) \
	gdd_ctx_foreach_graph((graphiter), (ctx)) \
	gdd_graph_foreach_vert((vertiter), \
						   (GddGraph *) gdd_map_iter_get_ptr(graphiter))

#define gdd_ctx_foreach_in_edge(graphiter, vertiter, edgeiter, ctx) \
	gdd_ctx_foreach_graph((graphiter), (ctx)) \
	gdd_graph_foreach_in_edge((vertiter), (edgeiter), \
							  (GddGraph *) gdd_map_iter_get_ptr(graphiter))

#define gdd_ctx_foreach_out_edge(graphiter, vertiter, edgeiter, ctx) \
	gdd_ctx_foreach_graph((graphiter), (ctx)) \
	gdd_graph_foreach_out_edge((vertiter), (edgeiter), \
							   (GddGraph *) gdd_map_iter_get_ptr(graphiter))

/***************************************************************************/

typedef struct GddPair		GddPair;
typedef struct GddMap		GddMap;
typedef struct GddMapIter	GddMapIter;
typedef struct GddListIter	GddListIter;
typedef struct GddEdge		GddEdge;
typedef struct GddVert		GddVert;
typedef struct GddGraph		GddGraph;
typedef struct GddStat		GddStat;

/***************************************************************************/

/*
 * A <int, pointer> pair.
 */
struct GddPair
{
	int			key;
	void		*ptr;
};

/*
 * A simple int->ptr map, implemented with array.
 */
struct GddMap
{
	int			length;			/* length of the <k,v> array */
	int			capacity;		/* capacity of the <k,v> array */

	GddPair		*pairs;			/* array of <k,v> pairs */
};

/*
 * Map iterator which supports safe map foreach iteration.
 */
struct GddMapIter
{
	GddMap		*map;
	int			pos;
};

/*
 * List iterator which supports safe list foreach iteration.
 */
struct GddListIter
{
	List		*list;
	ListCell	*cell;
	ListCell	*prev;
};

/*
 * A directed edge.
 */
struct GddEdge
{
	bool		solid;			/* a solid edge? */

	GddVert		*from;			/* the from vert */
	GddVert		*to;			/* the to vert */
};

/*
 * A vertex (vert) on a segment.
 */
struct GddVert
{
	int			id;				/* vert id */

	/*
	 * Pointer to ctx->topstat, all the verts on all the globals maintain
	 * the overall in/out degrees count together.
	 */
	GddStat		*topstat;

	/*
	 * Pointer to the `global` struct with the same vert id, the global
	 * in/out degrees of vert.
	 */
	GddStat		*global;

	List		*edgesIn;		/* List<Edge>, directed edges to vert */
	List		*edgesOut;		/* List<Edge>, directed edges from vert */
};

/*
 * Directed graph on a segment.
 */
struct GddGraph
{
	int			id;				/* segment id */

	GddMap		verts;			/* Map<vid, Vert>, the verts */
};

/*
 * Accounting information of a vert.
 */
struct GddStat
{
	int			id;				/* vert id */

	int			indeg;			/* in degree */
	int			outdeg;			/* out degree */
};

/*
 * GDD context, the toplevel struct which contains all the information.
 */
struct GddCtx
{
	GddStat		topstat;		/* overall in/out degrees of all verts on all graphs */
	GddMap		globals;		/* Map<vid, Stat>, global in/out degrees */
	GddMap		graphs;			/* Map<segid, Graph>, the local graphs */
};

/***************************************************************************/

/***************************************************************************/

static GddGraph *gddCtxGetGraph(GddCtx *ctx, int segid);
static GddStat *gddCtxGetGlobalStat(GddCtx *ctx, int vid);
static void gddCtxRemoveVid(GddCtx *ctx, int vid);
static int gddCtxGetMaxVid(GddCtx *ctx);

static GddStat *gddStatNew(int vid);
static void gddStatInit(GddStat *stat, int vid);

static GddGraph *gddGraphNew(int id);
static GddVert *gddGraphGetVert(GddGraph *graph, int vid);
static GddEdge *gddGraphMakeEdge(GddGraph *graph, int from, int to, bool solid);
static void gddGraphRemoveVid(GddGraph *graph, int vid);
static void gddGraphDump(GddGraph *graph, StringInfo str);

static GddVert *gddVertNew(int id);
static void gddVertBindStats(GddVert *vert, GddStat *global, GddStat *topstat);
static bool gddVertUnlinkAll(GddVert *vert);
static bool gddVertReduce(GddVert *vert);
static int gddVertGetInDegree(GddVert *vert);
static int gddVertGetOutDegree(GddVert *vert);

static GddEdge *gddEdgeNew(GddVert *from, GddVert *to, bool solid);
static void gddEdgeLink(GddEdge *edge);
static void gddEdgeUnlink(GddEdge *edge, GddListIter *fromiter, GddListIter *toiter);
static void gddEdgeUnlinkFrom(GddEdge *edge, GddListIter *fromiter);
static void gddEdgeUnlinkTo(GddEdge *edge, GddListIter *toiter);
static void gddEdgeDump(GddEdge *edge, StringInfo str);

static void gddMapInit(GddMap *map);
static void gddMapEnsureCapacity(GddMap *map, int capacity);
static GddPair *gddMapGetPair(GddMap *map, int key);
static void *gddMapGet(GddMap *map, int key);
static void gddMapSetUnsafe(GddMap *map, int key, void *ptr);

/***************************************************************************/

/*
 * Create a new Global DeadLock Detector context.
 */
GddCtx *
GddCtxNew(void)
{
	GddCtx		*ctx = palloc(sizeof(*ctx));

	gddStatInit(&ctx->topstat, 0);
	gddMapInit(&ctx->globals);
	gddMapInit(&ctx->graphs);

	return ctx;
}

/*
 * Add an edge to one local graph.
 */
void
GddCtxAddEdge(GddCtx *ctx, int segid, int from, int to, bool solid)
{
	GddEdge		*edge;
	GddGraph	*graph;
	GddStat		*global;

	Assert(ctx != NULL);

	graph = gddCtxGetGraph(ctx, segid);
	edge = gddGraphMakeEdge(graph, from, to, solid);

	global = gddCtxGetGlobalStat(ctx, from);
	gddVertBindStats(edge->from, global, &ctx->topstat);

	global = gddCtxGetGlobalStat(ctx, to);
	gddVertBindStats(edge->to, global, &ctx->topstat);

	/*
	 * Actually link the edge to its from/to verts,
	 * and update global in/out degrees.
	 */
	gddEdgeLink(edge);
}

/*
 * Reduce verts and edges looply until nothing can be deleted.
 */
void
GddCtxReduce(GddCtx *ctx)
{
	GddMapIter	graphiter;
	GddMapIter	vertiter;
	bool		dirty = true;

	Assert(ctx != NULL);

	while (dirty)
	{
		dirty = false;

		gdd_ctx_foreach_vert(graphiter, vertiter, ctx)
		{
			GddVert		*vert = gdd_map_iter_get_ptr(vertiter);

			if (gddVertReduce(vert))
				dirty = true;

			/*
			 * Remove vert from the graph if no edges left.
			 *
			 * We want to do this even if gddVertReduce() returns false
			 * as vert1's last edge might be removed in vert2's reduce call.
			 * However depending on the order of vert1 and vert2, we can not
			 * promise all the empty verts are removed, so do not make the
			 * assumption on this.  See gddCtxGetMaxVid() for example.
			 */
			if (!gddVertGetInDegree(vert) && !gddVertGetOutDegree(vert))
				gdd_map_iter_delete(vertiter);
		}
	}
}

/*
 * Break deadlocks and return a list of vids to cancel.
 */
List *
GddCtxBreakDeadLock(GddCtx *ctx)
{
	List		*vids = NIL;

	Assert(ctx != NULL);

	while (!GddCtxEmpty(ctx))
	{
		/*
		 * The only policy supported for now is to cancel the youngest vert,
		 * who has the max vid.
		 */
		int			maxvid = gddCtxGetMaxVid(ctx);

		vids = lappend_int(vids, maxvid);

		/*
		 * Cancel this vert and reduce again to see if more deadlocks
		 * are detected.
		 */
		gddCtxRemoveVid(ctx, maxvid);
		GddCtxReduce(ctx);
	}

	return vids;
}

/*
 * Return true if no edges left.
 */
bool
GddCtxEmpty(GddCtx *ctx)
{
	Assert(ctx->topstat.indeg == ctx->topstat.outdeg);

	return ctx->topstat.indeg == 0;
}

/*
 * Dump the graphs.
 */
void
GddCtxDump(GddCtx *ctx, StringInfo str)
{
	GddMapIter	iter;

	Assert(ctx != NULL);
	Assert(str != NULL);

	appendStringInfo(str, "{");

	gdd_ctx_foreach_graph(iter, ctx)
	{
		GddGraph	*graph = gdd_map_iter_get_ptr(iter);

		gddGraphDump(graph, str);

		if (gdd_map_iter_has_next(iter))
			appendStringInfo(str, ",");
	}

	appendStringInfo(str, "}");
}

/***************************************************************************/

/*
 * Get the graph with segment id.
 *
 * A new one is created if it's first seen.
 */
static GddGraph *
gddCtxGetGraph(GddCtx *ctx, int segid)
{
	GddGraph	*graph;

	Assert(ctx != NULL);

	graph = gddMapGet(&ctx->graphs, segid);

	if (!graph)
	{
		graph = gddGraphNew(segid);
		gddMapSetUnsafe(&ctx->graphs, segid, graph);
	}

	return graph;
}

/*
 * Get the global with vert id.
 *
 * A new one is created if it's first seen.
 */
static GddStat *
gddCtxGetGlobalStat(GddCtx *ctx, int vid)
{
	GddStat		*global;

	Assert(ctx != NULL);

	global = gddMapGet(&ctx->globals, vid);

	if (!global)
	{
		global = gddStatNew(vid);
		gddMapSetUnsafe(&ctx->globals, vid, global);
	}

	return global;
}

/*
 * Remove verts whose vert id equal to vid on all local graphs.
 */
static void
gddCtxRemoveVid(GddCtx *ctx, int vid)
{
	GddMapIter	iter;

	Assert(ctx != NULL);

	/* Do not use gdd_ctx_foreach_vert() as break is used in the inner loop */
	gdd_ctx_foreach_graph(iter, ctx)
	{
		GddGraph	*graph = gdd_map_iter_get_ptr(iter);

		gddGraphRemoveVid(graph, vid);
	}
}

/*
 * Get the max vert id, return 0 if no vert left at all.
 */
static int
gddCtxGetMaxVid(GddCtx *ctx)
{
	GddMapIter	graphiter;
	GddMapIter	vertiter;
	int			maxvid = 0;

	Assert(ctx != NULL);

	gdd_ctx_foreach_vert(graphiter, vertiter, ctx)
	{
		GddVert		*vert = gdd_map_iter_get_ptr(vertiter);

		/*
		 * It's possible for vert to have zero in/out edge,
		 * which means vert was already deleted in the
		 * algorithm conception so should be skipped.
		 */
		if (gddVertGetInDegree(vert) || gddVertGetOutDegree(vert))
			maxvid = Max(maxvid, vert->id);
	}

	return maxvid;
}

/***************************************************************************/

/*
 * Create a new global struct.
 */
static GddStat *
gddStatNew(int vid)
{
	GddStat		*stat = palloc(sizeof(*stat));

	gddStatInit(stat, vid);

	return stat;
}

/*
 * Initialize a global struct.
 */
static void
gddStatInit(GddStat *stat, int vid)
{
	Assert(stat != NULL);

	stat->id = vid;
	stat->indeg = 0;
	stat->outdeg = 0;
}

/***************************************************************************/

/*
 * Create a new graph struct.
 */
static GddGraph *
gddGraphNew(int id)
{
	GddGraph *graph = palloc(sizeof(*graph));

	graph->id = id;

	gddMapInit(&graph->verts);

	return graph;
}

/*
 * Get the vert with vert id.
 *
 * A new one is created if it's first seen.
 */
static GddVert *
gddGraphGetVert(GddGraph *graph, int vid)
{
	GddVert		*vert;

	Assert(graph != NULL);

	vert = gddMapGet(&graph->verts, vid);

	if (!vert)
	{
		vert = gddVertNew(vid);
		gddMapSetUnsafe(&graph->verts, vid, vert);
	}

	return vert;
}

/*
 * Make an edge in graph.
 *
 * The edge is not linked yet.
 */
static GddEdge *
gddGraphMakeEdge(GddGraph *graph, int from, int to, bool solid)
{
	GddEdge		*edge;
	GddVert		*vfrom;
	GddVert		*vto;

	Assert(graph != NULL);

	vfrom = gddGraphGetVert(graph, from);
	vto = gddGraphGetVert(graph, to);

	edge = gddEdgeNew(vfrom, vto, solid);

	return edge;
}

/*
 * Remove the vert whose vert id is vid.
 */
static void
gddGraphRemoveVid(GddGraph *graph, int vid)
{
	GddMapIter	iter;

	Assert(graph != NULL);

	gdd_graph_foreach_vert(iter, graph)
	{
		GddVert		*vert = gdd_map_iter_get_ptr(iter);

		Assert(vert != NULL);

		if (vert->id != vid)
			continue;

		/* Remove all local in/out edges */
		gddVertUnlinkAll(vert);

		/* Finally remove vert from graph */
		gdd_map_iter_delete(iter);

		/* Only one vert could have vert id equals to vid */
		break;
	}
}

/*
 * Dump the verts and edges.
 */
static void
gddGraphDump(GddGraph *graph, StringInfo str)
{
	GddMapIter	vertiter;
	GddListIter	edgeiter;
	bool		first = true;

	Assert(graph != NULL);
	Assert(str != NULL);

	appendStringInfo(str, "\"seg%d\": [", graph->id);

	gdd_graph_foreach_out_edge(vertiter, edgeiter, graph)
	{
		GddEdge		*edge = gdd_list_iter_get_ptr(edgeiter);

		if (first)
			first = false;
		else
			appendStringInfo(str, ",");

		gddEdgeDump(edge, str);
	}

	appendStringInfo(str, "]");
}

/***************************************************************************/

/*
 * Create a new vert.
 */
static GddVert *
gddVertNew(int id)
{
	GddVert		*vert = palloc(sizeof(*vert));

	vert->id = id;
	vert->edgesIn = NIL;
	vert->edgesOut = NIL;

	return vert;
}

/*
 * Bind global and vert.
 */
static void
gddVertBindStats(GddVert *vert, GddStat *global, GddStat *topstat)
{
	Assert(vert != NULL);

	vert->global = global;
	vert->topstat = topstat;
}

/*
 * Unlink vert's all edges.
 */
static bool
gddVertUnlinkAll(GddVert *vert)
{
	GddListIter	edgeiter;
	bool		dirty = false;

	Assert(vert != NULL);

	gdd_vert_foreach_in_edge(edgeiter, vert)
	{
		GddEdge		*edge = gdd_list_iter_get_ptr(edgeiter);

		gddEdgeUnlink(edge, NULL, &edgeiter);
		dirty = true;
	}

	Assert(vert->edgesIn == NIL);

	gdd_vert_foreach_out_edge(edgeiter, vert)
	{
		GddEdge		*edge = gdd_list_iter_get_ptr(edgeiter);

		gddEdgeUnlink(edge, &edgeiter, NULL);
		dirty = true;
	}

	Assert(vert->edgesOut == NIL);

	return dirty;
}

/*
 * Reduce vert's edges.
 *
 * The algorithm is described at README.md.
 *
 * Return true if anything is deleted.
 */
static bool
gddVertReduce(GddVert *vert)
{
	GddStat		*global;
	bool		dirty = false;

	Assert(vert != NULL);
	Assert(vert->global != NULL);

	global = vert->global;

	/*
	 * Remove global end verts (verts with global 0 in/out degrees)
	 */
	if (global->indeg == 0 || global->outdeg == 0)
	{
		/* Remove all local in/out edges */
		if (gddVertUnlinkAll(vert))
			dirty = true;

		return dirty;
	}

	/*
	 * Remove a vert's dotted in edges if vert's local out degree is 0.
	 */
	if (gddVertGetOutDegree(vert) == 0)
	{
		GddListIter	edgeiter;

		gdd_vert_foreach_in_edge(edgeiter, vert)
		{
			GddEdge		*edge = gdd_list_iter_get_ptr(edgeiter);

			/* Do not remove solid edges */
			if (edge->solid)
				continue;

			gddEdgeUnlink(edge, NULL, &edgeiter);
			dirty = true;
		}

		AssertImply(gdd_list_iter_get_list(edgeiter) == NIL,
					vert->edgesIn == NIL);
	}

	return dirty;
}

/*
 * Get vert's local in degree.
 */
static int
gddVertGetInDegree(GddVert *vert)
{
	Assert(vert != NULL);

	return list_length(vert->edgesIn);
}

/*
 * Get vert's local in degree.
 */
static int
gddVertGetOutDegree(GddVert *vert)
{
	Assert(vert != NULL);

	return list_length(vert->edgesOut);
}

/***************************************************************************/

/*
 * Create a new edge.
 */
static GddEdge *
gddEdgeNew(GddVert *from, GddVert *to, bool solid)
{
	GddEdge		*edge = palloc(sizeof(*edge));

	edge->from = from;
	edge->to = to;
	edge->solid = solid;

	return edge;
}

/*
 * Link edge to its from/to verts and update global in/out degrees.
 */
static void
gddEdgeLink(GddEdge *edge)
{
	Assert(edge != NULL);
	Assert(edge->from != NULL);
	Assert(edge->from->global != NULL);
	Assert(edge->to != NULL);
	Assert(edge->to->global != NULL);
	Assert(edge->to->topstat != NULL);
	Assert(edge->to->topstat == edge->from->topstat);

	edge->from->topstat->outdeg++;
	edge->to->topstat->indeg++;
	Assert(edge->from->topstat->outdeg == edge->to->topstat->indeg);

	/* Update global in/out degrees */
	edge->from->global->outdeg++;
	edge->to->global->indeg++;

	edge->from->edgesOut = lappend(edge->from->edgesOut, edge);
	edge->to->edgesIn = lappend(edge->to->edgesIn, edge);
}

/*
 * Unlink edge from its from/to verts and update global in/out degrees.
 */
static void
gddEdgeUnlink(GddEdge *edge, GddListIter *fromiter, GddListIter *toiter)
{
	Assert(edge != NULL);

	gddEdgeUnlinkFrom(edge, fromiter);
	gddEdgeUnlinkTo(edge, toiter);

	Assert(edge->from->topstat->outdeg == edge->to->topstat->indeg);
}

/*
 * Unlink edge from its from vert and update global out degree.
 */
static void
gddEdgeUnlinkFrom(GddEdge *edge, GddListIter *fromiter)
{
	Assert(edge != NULL);
	Assert(edge->from != NULL);
	Assert(edge->from->global != NULL);
	Assert(edge->from->topstat != NULL);

	edge->from->topstat->outdeg--;
	Assert(edge->from->topstat->outdeg >= 0);

	edge->from->global->outdeg--;
	Assert(edge->from->global->outdeg >= 0);

	/*
	 * If the from vert's iter is provided then delete it in-place,
	 * otherwise delete it with a scan.
	 */
	if (fromiter)
	{
		gdd_list_iter_delete(*fromiter);
		edge->from->edgesOut = gdd_list_iter_get_list(*fromiter);
	}
	else
		edge->from->edgesOut = list_delete_ptr(edge->from->edgesOut, edge);
}

/*
 * Unlink edge from its to vert and update global in degree.
 */
static void
gddEdgeUnlinkTo(GddEdge *edge, GddListIter *toiter)
{
	Assert(edge != NULL);
	Assert(edge->to != NULL);
	Assert(edge->to->global != NULL);
	Assert(edge->to->topstat != NULL);

	edge->to->topstat->indeg--;
	Assert(edge->to->topstat->indeg >= 0);

	edge->to->global->indeg--;
	Assert(edge->to->global->indeg >= 0);

	/*
	 * If the to vert's iter is provided then delete it in-place,
	 * otherwise delete it with a scan.
	 */
	if (toiter)
	{
		gdd_list_iter_delete(*toiter);
		edge->to->edgesIn = gdd_list_iter_get_list(*toiter);
	}
	else
		edge->to->edgesIn = list_delete_ptr(edge->to->edgesIn, edge);
}

/*
 * Dump edge.
 */
static void
gddEdgeDump(GddEdge *edge, StringInfo str)
{
	Assert(edge != NULL);
	Assert(edge->from != NULL);
	Assert(edge->to != NULL);
	Assert(str != NULL);

	appendStringInfo(str, "\"%d(Master Pid: %d)%s%d(Master Pid: %d)\"",
					 edge->from->id,
					 GetPidByGxid(edge->from->id),
					 edge->solid ? "==>" : "~~>",
					 edge->to->id,
					 GetPidByGxid(edge->to->id));
}

/***************************************************************************/

/*
 * Initialize a map.
 */
static void
gddMapInit(GddMap *map)
{
	Assert(map != NULL);

	map->length = 0;
	map->capacity = 4;
	map->pairs = palloc(map->capacity * sizeof(GddPair));
}

/*
 * Ensure map has enough capacity.
 */
static void
gddMapEnsureCapacity(GddMap *map, int capacity)
{
	Assert(map != NULL);

	if (capacity <= map->capacity)
		return;

	/* To prevent frequently resizing we prepare double capacity as required */
	map->capacity = capacity << 1;

	map->pairs = repalloc(map->pairs,
						  map->capacity * sizeof(GddPair));
}

/*
 * Lookup the pair struct by key.
 *
 * Return NULL if key is not found.
 */
static GddPair *
gddMapGetPair(GddMap *map, int key)
{
	int			i;

	Assert(map != NULL);

	for (i = 0; i < map->length; i++)
	{
		GddPair		*pair = &map->pairs[i];

		if (pair->key == key)
			return pair;
	}

	return NULL;
}

/*
 * Lookup the pointer by key.
 *
 * Return NULL if key is not found.  If NULL is valid value for the pointer
 * then use gddMapGetPair() instead of this one.
 */
static void *
gddMapGet(GddMap *map, int key)
{
	GddPair		*pair = gddMapGetPair(map, key);

	return pair ? pair->ptr : NULL;
}

/*
 * Append a new <k,v> pair without checking for existence.
 *
 * Only use this if you have already checked with the gddMapGet*() functions
 * that k does not exist.
 */
static void
gddMapSetUnsafe(GddMap *map, int key, void *ptr)
{
	GddPair		*pair;

	gddMapEnsureCapacity(map, map->length + 1);

	pair = &map->pairs[map->length++];

	pair->key = key;
	pair->ptr = ptr;
}
