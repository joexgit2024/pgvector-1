#include "postgres.h"

#include "access/relscan.h"
#include "hnsw.h"
#include "pgstat.h"
#include "storage/bufmgr.h"

/*
 * Algorithm 5 from paper
 */
static void
GetScanItems(IndexScanDesc scan, Datum q)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	FmgrInfo   *procinfo = so->procinfo;
	Oid			collation = so->collation;
	List	   *ep = NIL;
	List	   *w;
	HnswElement entryPoint = GetEntryPoint(index);

	if (entryPoint == NULL)
		return;

	/* TODO Use memory context */
	ep = lappend(ep, EntryCandidate(entryPoint, q, index, procinfo, collation, false));

	for (int lc = entryPoint->level; lc >= 1; lc--)
	{
		w = SearchLayer(q, ep, 1, lc, index, procinfo, collation, false, NULL, NULL);
		ep = w;
	}

	/* TODO Return all visited elements at level 0, not just ef search */
	so->w = SearchLayer(q, ep, hnsw_ef_search, 0, index, procinfo, collation, false, NULL, NULL);
}

/*
 * Prepare for an index scan
 */
IndexScanDesc
hnswbeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	HnswScanOpaque so;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	so = (HnswScanOpaque) palloc(sizeof(HnswScanOpaqueData));
	so->buf = InvalidBuffer;
	so->first = true;
	so->w = NIL;

	/* Set support functions */
	so->procinfo = index_getprocinfo(index, 1, HNSW_DISTANCE_PROC);
	so->normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
	so->collation = index->rd_indcollation[0];

	scan->opaque = so;

	return scan;
}

/*
 * Start or restart an index scan
 */
void
hnswrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;

	so->first = true;
	list_free(so->w);
	so->w = NIL;

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));

	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/*
 * Fetch the next tuple in the given scan
 */
bool
hnswgettuple(IndexScanDesc scan, ScanDirection dir)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;

	/*
	 * Index can be used to scan backward, but Postgres doesn't support
	 * backward scan on operators
	 */
	Assert(ScanDirectionIsForward(dir));

	if (so->first)
	{
		Datum		value;

		/* Count index scan for stats */
		pgstat_count_index_scan(scan->indexRelation);

		/* Safety check */
		if (scan->orderByData == NULL)
			elog(ERROR, "cannot scan hnsw index without order");

		/* No items will match if null */
		if (scan->orderByData->sk_flags & SK_ISNULL)
			return false;

		value = scan->orderByData->sk_argument;

		/* Value should not be compressed or toasted */
		Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
		Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));

		if (so->normprocinfo != NULL)
		{
			/* No items will match if normalization fails */
			if (!HnswNormValue(so->normprocinfo, so->collation, &value, NULL))
				return false;
		}

		GetScanItems(scan, value);
		so->first = false;

		/* Clean up if we allocated a new value */
		if (value != scan->orderByData->sk_argument)
			pfree(DatumGetPointer(value));
	}

	while (list_length(so->w) > 0)
	{
		HnswCandidate *hc = llast(so->w);
		ItemPointer tid;
		BlockNumber indexblkno;

		/* Move to next element if no valid heap tids */
		if (list_length(hc->element->heaptids) == 0)
		{
			so->w = list_delete_last(so->w);
			continue;
		}

		tid = llast(hc->element->heaptids);
		indexblkno = hc->element->blkno;

		hc->element->heaptids = list_delete_last(hc->element->heaptids);

#if PG_VERSION_NUM >= 120000
		scan->xs_heaptid = *tid;
#else
		scan->xs_ctup.t_self = *tid;
#endif

		if (BufferIsValid(so->buf))
			ReleaseBuffer(so->buf);

		/*
		 * An index scan must maintain a pin on the index page holding the
		 * item last returned by amgettuple
		 *
		 * https://www.postgresql.org/docs/current/index-locking.html
		 */
		so->buf = ReadBuffer(scan->indexRelation, indexblkno);

		scan->xs_recheckorderby = false;
		return true;
	}

	return false;
}

/*
 * End a scan and release resources
 */
void
hnswendscan(IndexScanDesc scan)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;

	/* Release pin */
	if (BufferIsValid(so->buf))
		ReleaseBuffer(so->buf);

	list_free(so->w);

	pfree(so);
	scan->opaque = NULL;
}