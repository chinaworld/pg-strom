/*
 * datastore.c
 *
 * Routines to manage data store; row-store, column-store, toast-buffer,
 * and param-buffer.
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
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
#include "pg_strom.h"
#include "cuda_numeric.h"
#include "nvme_strom.h"

/*
 * estimate_num_chunks
 *
 * it estimates number of chunks to be fetched from the supplied Path
 */
cl_uint
estimate_num_chunks(Path *pathnode)
{
	RelOptInfo *rel = pathnode->parent;
	int			ncols = list_length(rel->reltarget->exprs);
    Size        htup_size;
	cl_uint		num_chunks;

	htup_size = MAXALIGN(offsetof(HeapTupleHeaderData,
								  t_bits[BITMAPLEN(ncols)]));
	if (rel->reloptkind != RELOPT_BASEREL)
		htup_size += MAXALIGN(rel->reltarget->width);
	else
	{
		double      heap_size = (double)
			(BLCKSZ - SizeOfPageHeaderData) * rel->pages;

		htup_size += MAXALIGN(heap_size / Max(rel->tuples, 1.0) -
							  sizeof(ItemIdData) - SizeofHeapTupleHeader);
	}
	num_chunks = (cl_uint)
		((double)(htup_size + sizeof(cl_int)) * pathnode->rows /
		 (double)(pgstrom_chunk_size() -
				  STROMALIGN(offsetof(kern_data_store, colmeta[ncols]))));
	num_chunks = Max(num_chunks, 1);

	return num_chunks;
}

/*
 * PDS_fetch_tuple - fetch a tuple from the PDS
 */
static inline bool
KDS_fetch_tuple_row(TupleTableSlot *slot,
					kern_data_store *kds,
					GpuTaskState *gts)
{
	if (gts->curr_index < kds->nitems)
	{
		size_t			row_index = gts->curr_index++;
		Relation		rel = gts->css.ss.ss_currentRelation;
		kern_tupitem   *tup_item;
		HeapTuple		tuple = &gts->curr_tuple;

		tup_item = KERN_DATA_STORE_TUPITEM(kds, row_index);
		ExecClearTuple(slot);
		tuple->t_len  = tup_item->t_len;
		tuple->t_self = tup_item->t_self;
		tuple->t_tableOid = (rel ? RelationGetRelid(rel) : InvalidOid);
		tuple->t_data = &tup_item->htup;

		ExecStoreTuple(tuple, slot, InvalidBuffer, false);

		return true;
	}
	return false;
}

static inline bool
KDS_fetch_tuple_slot(TupleTableSlot *slot,
					 kern_data_store *kds,
					 GpuTaskState *gts)
{
	if (gts->curr_index < kds->nitems)
	{
		size_t	row_index = gts->curr_index++;
		Datum  *tts_values = KERN_DATA_STORE_VALUES(kds, row_index);
		bool   *tts_isnull = KERN_DATA_STORE_ISNULL(kds, row_index);
		int		natts = slot->tts_tupleDescriptor->natts;

		memcpy(slot->tts_values, tts_values, sizeof(Datum) * natts);
		memcpy(slot->tts_isnull, tts_isnull, sizeof(bool) * natts);
#ifdef NOT_USED
		/*
		 * XXX - pointer reference is better than memcpy from performance
		 * perspectives, however, we need to ensure tts_values/tts_isnull
		 * shall be restored when pgstrom-data-store is released.
		 * It will be cause of complicated / invisible bugs.
		 */
		slot->tts_values = tts_values;
		slot->tts_isnull = tts_isnull;
#endif
		ExecStoreVirtualTuple(slot);
		return true;
	}
	return false;
}

static inline bool
KDS_fetch_tuple_block(TupleTableSlot *slot,
					  kern_data_store *kds,
					  GpuTaskState *gts)
{
	Relation	rel = gts->css.ss.ss_currentRelation;
	HeapTuple	tuple = &gts->curr_tuple;
	BlockNumber	block_nr;
	PageHeader	hpage;
	cl_uint		max_lp_index;
	ItemId		lpp;

	while (gts->curr_index < kds->nitems)
	{
		block_nr = KERN_DATA_STORE_BLOCK_BLCKNR(kds, gts->curr_index);
		hpage = KERN_DATA_STORE_BLOCK_PGPAGE(kds, gts->curr_index);
		Assert(PageIsAllVisible(hpage));
		max_lp_index = PageGetMaxOffsetNumber(hpage);
		while (gts->curr_lp_index < max_lp_index)
		{
			cl_uint		lp_index = gts->curr_lp_index++;

			lpp = &hpage->pd_linp[lp_index];
			if (!ItemIdIsNormal(lpp))
				continue;

			tuple->t_len = ItemIdGetLength(lpp);
			BlockIdSet(&tuple->t_self.ip_blkid, block_nr);
			tuple->t_self.ip_posid = lp_index;
			tuple->t_tableOid = (rel ? RelationGetRelid(rel) : InvalidOid);
			tuple->t_data = (HeapTupleHeader)((char *)hpage +
											  ItemIdGetOffset(lpp));
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);
			return true;
		}
		/* move to the next block */
		gts->curr_index++;
		gts->curr_lp_index = 0;
	}
	return false;	/* end of the PDS */
}

bool
KDS_fetch_tuple_column(TupleTableSlot *slot,
					   kern_data_store *kds,
					   size_t row_index)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	int			j;

	/*
	 * XXX - Is a mode to fetch system columns (if any) valuable?
	 * Right now, KDS_fetch_tuple_column() is only used by gstore_fdw.c
	 * to fetch rows from KDS(column), however, its transaction control
	 * properties are separately saved, thus, nobody tries to pick up
	 * system columns via this API.
	 */
	Assert(kds->format == KDS_FORMAT_COLUMN);
	Assert(kds->ncols == tupdesc->natts + NumOfSystemAttrs);
	if (row_index >= kds->nitems)
	{
		ExecClearTuple(slot);
		return false;
	}

	for (j=0; j < tupdesc->natts; j++)
	{
		void   *addr = kern_get_datum_column(kds, j, row_index);
		int		attlen = kds->colmeta[j].attlen;

		if (!addr)
			slot->tts_isnull[j] = true;
		else
		{
			slot->tts_isnull[j] = false;
			if (!kds->colmeta[j].attbyval)
				slot->tts_values[j] = PointerGetDatum(addr);
			else if (attlen == sizeof(cl_char))
				slot->tts_values[j] = CharGetDatum(*((cl_char *)addr));
			else if (attlen == sizeof(cl_short))
				slot->tts_values[j] = Int16GetDatum(*((cl_short *)addr));
			else if (attlen == sizeof(cl_int))
				slot->tts_values[j] = Int32GetDatum(*((cl_int *)addr));
			else if (attlen == sizeof(cl_long))
				slot->tts_values[j] = Int64GetDatum(*((cl_long *)addr));
			else
				elog(ERROR, "unexpected attlen: %d", attlen);
		}
	}
	ExecStoreVirtualTuple(slot);

	return true;
}

bool
PDS_fetch_tuple(TupleTableSlot *slot,
				pgstrom_data_store *pds,
				GpuTaskState *gts)
{
	switch (pds->kds.format)
	{
		case KDS_FORMAT_ROW:
		case KDS_FORMAT_HASH:
			return KDS_fetch_tuple_row(slot, &pds->kds, gts);
		case KDS_FORMAT_SLOT:
			return KDS_fetch_tuple_slot(slot, &pds->kds, gts);
		case KDS_FORMAT_BLOCK:
			return KDS_fetch_tuple_block(slot, &pds->kds, gts);
		case KDS_FORMAT_COLUMN:
			return KDS_fetch_tuple_column(slot, &pds->kds, gts->curr_index++);
		default:
			elog(ERROR, "Bug? unsupported data store format: %d",
				pds->kds.format);
	}
}

/*
 * PDS_clone - makes an empty data store with same definition
 */
pgstrom_data_store *
__PDS_clone(pgstrom_data_store *pds_old,
			const char *filename, int lineno)
{
	pgstrom_data_store *pds_new;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;

	rc = __gpuMemAllocManaged(pds_old->gcontext,
							  &m_deviceptr,
							  offsetof(pgstrom_data_store,
									   kds) + pds_old->kds.length,
							  CU_MEM_ATTACH_GLOBAL,
							  filename, lineno);
	if (rc != CUDA_SUCCESS)
		werror("out of managed memory");
	pds_new = (pgstrom_data_store *) m_deviceptr;

	/* setup */
	memset(&pds_new->chain, 0, sizeof(dlist_node));
	pds_new->gcontext = pds_old->gcontext;
	pg_atomic_init_u32(&pds_new->refcnt, 1);
	pds_new->nblocks_uncached = 0;
	pds_new->filedesc = -1;
	memcpy(&pds_new->kds,
		   &pds_old->kds,
		   KERN_DATA_STORE_HEAD_LENGTH(&pds_old->kds));
	/* make the data store empty */
	pds_new->kds.usage = 0;
	pds_new->kds.nitems = 0;

	return pds_new;
}

/*
 * PDS_retain
 */
pgstrom_data_store *
PDS_retain(pgstrom_data_store *pds)
{
	int32		refcnt_old	__attribute__((unused));

	refcnt_old = (int32)pg_atomic_fetch_add_u32(&pds->refcnt, 1);

	Assert(refcnt_old > 0);

	return pds;
}

/*
 * PDS_release
 */
void
PDS_release(pgstrom_data_store *pds)
{
	GpuContext *gcontext = pds->gcontext;
	CUresult	rc;
	int32		refcnt;

	refcnt = (int32)pg_atomic_sub_fetch_u32(&pds->refcnt, 1);
	Assert(refcnt >= 0);
	if (refcnt == 0)
	{
		if (pds->kds.format != KDS_FORMAT_BLOCK)
		{
			rc = gpuMemFree(gcontext, (CUdeviceptr) pds);
			if (rc != CUDA_SUCCESS)
				werror("failed on gpuMemFree: %s", errorText(rc));
		}
		else
		{
			rc = gpuMemFreeHost(gcontext, pds);
			if (rc != CUDA_SUCCESS)
				werror("failed on gpuMemFreeHost: %s", errorText(rc));
		}
	}
}

void
init_kernel_data_store(kern_data_store *kds,
					   TupleDesc tupdesc,
					   Size length,
					   int format,
					   uint nrooms)
{
	int		i, j, attcacheoff;

	memset(kds, 0, offsetof(kern_data_store, colmeta));
	kds->length = length;
	kds->nitems = 0;
	kds->usage = 0;
	kds->nrooms = nrooms;
	kds->ncols = tupdesc->natts;
	kds->format = format;
	kds->tdhasoid = tupdesc->tdhasoid;
	kds->tdtypeid = tupdesc->tdtypeid;
	kds->tdtypmod = tupdesc->tdtypmod;
	kds->table_oid = InvalidOid;	/* caller shall set */
	kds->nslots = 0;				/* caller shall set, if any */
	kds->hash_min = 0;
	kds->hash_max = UINT_MAX;
	kds->nrows_per_block = 0;

	if (format != KDS_FORMAT_COLUMN)
	{
		attcacheoff = offsetof(HeapTupleHeaderData, t_bits);
		if (tupdesc->tdhasoid)
			attcacheoff += sizeof(Oid);
		attcacheoff = MAXALIGN(attcacheoff);
	}
	else
	{
		/* attcacheoff does not make sense for columnar format */
		attcacheoff = -1;
	}

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		int		attalign = typealign_get_width(attr->attalign);
		bool	attbyval = attr->attbyval;
		int		attlen = attr->attlen;

		if (!attr->attbyval)
			kds->has_notbyval = true;
		if (attcacheoff > 0)
		{
			if (attlen > 0)
				attcacheoff = TYPEALIGN(attalign, attcacheoff);
			else
				attcacheoff = -1;	/* no more shortcut any more */
		}
		kds->colmeta[i].attbyval = attbyval;
		kds->colmeta[i].attalign = attalign;
		kds->colmeta[i].attlen = attlen;
		kds->colmeta[i].attnum = attr->attnum;
		kds->colmeta[i].attcacheoff = attcacheoff;
		kds->colmeta[i].atttypid = (cl_uint)attr->atttypid;
		kds->colmeta[i].atttypmod = (cl_int)attr->atttypmod;
		kds->colmeta[i].va_offset = 0;
		kds->colmeta[i].extra_sz = 0;
		if (attcacheoff >= 0)
			attcacheoff += attr->attlen;
		/*
		 * !!don't forget to update pl_cuda.c if kern_colmeta layout would
		 * be updated !!
		 */
	}

	/*
	 * columnar format has system attribute definition next to the regular
	 * attributes. If no data array, keep va_offset/extra_sz zero.
	 */
	if (format == KDS_FORMAT_COLUMN)
	{
		kds->ncols += NumOfSystemAttrs;
		for (j=FirstLowInvalidHeapAttributeNumber+1; j < 0; j++)
		{
			Form_pg_attribute attr = SystemAttributeDefinition(j, true);

			i = kds->ncols + j;
			kds->colmeta[i].attbyval = attr->attbyval;
			kds->colmeta[i].attalign = typealign_get_width(attr->attalign);
			kds->colmeta[i].attlen = attr->attlen;
			kds->colmeta[i].attnum = j;
			kds->colmeta[i].attcacheoff = -1;
			kds->colmeta[i].atttypid = (cl_uint)attr->atttypid;
			kds->colmeta[i].atttypmod = (cl_int)attr->atttypmod;
			kds->colmeta[i].va_offset = 0;
			kds->colmeta[i].extra_sz = 0;
		}
	}
}

pgstrom_data_store *
__PDS_create_row(GpuContext *gcontext,
				 TupleDesc tupdesc,
				 size_t bytesize,
				 const char *filename, int lineno)
{
	pgstrom_data_store *pds;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;

	bytesize = STROMALIGN_DOWN(bytesize);
	rc = __gpuMemAllocManaged(gcontext,
							  &m_deviceptr,
							  offsetof(pgstrom_data_store,
									   kds) + bytesize,
							  CU_MEM_ATTACH_GLOBAL,
							  filename, lineno);
	if (rc != CUDA_SUCCESS)
		werror("out of managed memory");
	pds = (pgstrom_data_store *) m_deviceptr;

	/* setup */
	memset(&pds->chain, 0, sizeof(dlist_node));
	pds->gcontext = gcontext;
	pg_atomic_init_u32(&pds->refcnt, 1);
	init_kernel_data_store(&pds->kds, tupdesc, bytesize,
						   KDS_FORMAT_ROW, INT_MAX);
	pds->nblocks_uncached = 0;
	pds->filedesc = -1;

	return pds;
}

pgstrom_data_store *
__PDS_create_hash(GpuContext *gcontext,
				  TupleDesc tupdesc,
				  size_t bytesize,
				  const char *filename, int lineno)
{
	pgstrom_data_store *pds;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;

	bytesize = STROMALIGN_DOWN(bytesize);
	if (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) > bytesize)
		elog(ERROR, "Required length for KDS-Hash is too short");

	rc = __gpuMemAllocManaged(gcontext,
							  &m_deviceptr,
							  offsetof(pgstrom_data_store,
									   kds) + bytesize,
							  CU_MEM_ATTACH_GLOBAL,
							  filename, lineno);
	if (rc != CUDA_SUCCESS)
		werror("out of managed memory");
	pds = (pgstrom_data_store *) m_deviceptr;

	/* setup */
	memset(&pds->chain, 0, sizeof(dlist_node));
	pds->gcontext = gcontext;
	pg_atomic_init_u32(&pds->refcnt, 1);
	init_kernel_data_store(&pds->kds, tupdesc, bytesize,
						   KDS_FORMAT_HASH, INT_MAX);
	pds->nblocks_uncached = 0;
	pds->filedesc = -1;

	return pds;
}

pgstrom_data_store *
__PDS_create_slot(GpuContext *gcontext,
				  TupleDesc tupdesc,
				  size_t bytesize,
				  const char *filename, int lineno)
{
	pgstrom_data_store *pds;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;
	size_t		nrooms;

	bytesize = STROMALIGN_DOWN(bytesize);
	if (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) > bytesize)
		elog(ERROR, "Required length for KDS-Slot is too short");

	nrooms = (bytesize - KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts))
		/ LONGALIGN((sizeof(Datum) + sizeof(char)) * tupdesc->natts);

	rc = __gpuMemAllocManaged(gcontext,
							  &m_deviceptr,
							  offsetof(pgstrom_data_store,
									   kds) + bytesize,
							  CU_MEM_ATTACH_GLOBAL,
							  filename, lineno);
	if (rc != CUDA_SUCCESS)
		werror("out of managed memory");
	pds = (pgstrom_data_store *) m_deviceptr;

	/* setup */
	memset(&pds->chain, 0, sizeof(dlist_node));
	pds->gcontext = gcontext;
	pg_atomic_init_u32(&pds->refcnt, 1);
	init_kernel_data_store(&pds->kds, tupdesc,
						   bytesize - offsetof(pgstrom_data_store, kds),
						   KDS_FORMAT_SLOT, nrooms);
	pds->nblocks_uncached = 0;
	pds->filedesc = -1;

	return pds;
}

pgstrom_data_store *
__PDS_create_block(GpuContext *gcontext,
				   TupleDesc tupdesc,
				   NVMEScanState *nvme_sstate,
				   const char *filename, int lineno)
{
	pgstrom_data_store *pds = NULL;
	cl_uint		nrooms = nvme_sstate->nblocks_per_chunk;
	size_t		bytesize;
	CUresult	rc;

	bytesize = KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts)
		+ STROMALIGN(sizeof(BlockNumber) * nrooms)
		+ BLCKSZ * nrooms;
	if (offsetof(pgstrom_data_store, kds) + bytesize > pgstrom_chunk_size())
		elog(ERROR,
			 "Bug? PDS length (%zu) is larger than pg_strom.chunk_size(%zu)",
			 offsetof(pgstrom_data_store, kds) + bytesize,
			 pgstrom_chunk_size());

	rc = __gpuMemAllocHost(gcontext,
						   (void **)&pds,
						   pgstrom_chunk_size(),
						   filename, lineno);
	if (rc != CUDA_SUCCESS)
		werror("failed on gpuMemAllocHost: %s", errorText(rc));
	/* setup */
	memset(&pds->chain, 0, sizeof(dlist_node));
	pds->gcontext = gcontext;
	pg_atomic_init_u32(&pds->refcnt, 1);
	init_kernel_data_store(&pds->kds, tupdesc, bytesize,
						   KDS_FORMAT_BLOCK, nrooms);
    pds->kds.nrows_per_block = nvme_sstate->nrows_per_block;
    pds->nblocks_uncached = 0;
	pds->filedesc = -1;

	return pds;
}

/*
 * support for bulkload onto ROW/BLOCK format
 */

/*
 * nvme_sstate_open_smgr - fetch File descriptor of relation
 *
 * see storage/smgr/md.c
 */
typedef struct _MdfdVec
{
	File			mdfd_vfd;		/* fd number in fd.c's pool */
	BlockNumber		mdfd_segno;		/* segment number, from 0 */
#if PG_VERSION_NUM < 100000
	struct _MdfdVec *mdfd_chain;	/* next segment, or NULL */
#endif
} MdfdVec;

static int
nvme_sstate_open_segment(SMgrRelation rd_smgr, int seg_nr)
{
	/* see _mdfd_openseg() and _mdfd_segpath() */
	char	   *temp;
	char	   *path;
	int			fdesc;

	temp = relpath(rd_smgr->smgr_rnode, MAIN_FORKNUM);
	if (seg_nr > 0)
	{
		path = psprintf("%s.%u", temp, seg_nr);
		pfree(temp);
	}
	else
		path = temp;

	fdesc = open(path, O_RDWR | PG_BINARY, 0600);
	if (fdesc < 0)
		elog(ERROR, "failed on open('%s'): %m", path);
	pfree(path);

	return fdesc;
}

static void
nvme_sstate_open_files(GpuContext *gcontext,
					   NVMEScanState *nvme_sstate,
					   Relation relation)
{
	SMgrRelation rd_smgr = relation->rd_smgr;
	MdfdVec	   *vec;
	int			i, nr_segs;
	int			fdesc;

#if PG_VERSION_NUM < 100000
	/* PG9.6 */
	nr_segs = nvme_sstate->nr_segs;
	memset(nvme_sstate->fdesc, -1, sizeof(int) * nr_segs);
	for (vec = rd_smgr->md_fd[MAIN_FORKNUM];
		 vec != NULL;
		 vec = vec->mdfd_chain)
	{
		if (vec->mdfd_vfd < 0)
			elog(ERROR, "Bug? seg=%u of relation %s is not opened",
				 vec->mdfd_segno, RelationGetRelationName(relation));
		if (vec->mdfd_segno >= nr_segs)
			continue;	/* skip, out of the range */

		fdesc = FileGetRawDesc(vec->mdfd_vfd);
		if (fdesc < 0)
			fdesc = nvme_sstate_open_segment(rd_smgr, vec->mdfd_segno);
		else
		{
			fdesc = dup(fdesc);
			if (fdesc < 0)
				elog(ERROR, "failed on dup(2): %m");
		}

		if (!trackRawFileDesc(gcontext, fdesc, __FILE__, __LINE__))
		{
			close(fdesc);
			elog(ERROR, "out of memory");
		}
		nvme_sstate->fdesc[vec->mdfd_segno] = fdesc;
	}

	for (i=0; i < nr_segs; i++)
	{
		if (nvme_sstate->fdesc[i] >= 0)
			continue;
		fdesc = nvme_sstate_open_segment(rd_smgr, i);
		if (!trackRawFileDesc(gcontext, fdesc, __FILE__, __LINE__))
		{
			close(fdesc);
			elog(ERROR, "out of memory");
		}
		nvme_sstate->fdesc[i] = fdesc;
	}
#else
	/* PG10 or later */
	nr_segs = Min(rd_smgr->md_num_open_segs[MAIN_FORKNUM],
				  nvme_sstate->nr_segs);
	for (i=0; i < nr_segs; i++)
	{
		vec = &rd_smgr->md_seg_fds[MAIN_FORKNUM][i];
		if (vec->mdfd_segno != i)
			elog(ERROR, "Bug? mdfd_segno is not consistent");
		if (vec->mdfd_vfd < 0)
			elog(ERROR, "Bug? seg=%d of relation %s is not opened",
				 i, RelationGetRelationName(relation));
		fdesc = FileGetRawDesc(vec->mdfd_vfd);
		if (fdesc < 0)
			fdesc = nvme_sstate_open_segment(rd_smgr, i);
		else
		{
			fdesc = dup(fdesc);
			if (fdesc < 0)
				elog(ERROR, "failed on dup(2): %m");
		}

		if (!trackRawFileDesc(gcontext, fdesc, __FILE__, __LINE__))
		{
			close(fdesc);
			elog(ERROR, "out of memory");
		}
		nvme_sstate->fdesc[i] = fdesc;
	}

	while (i < nvme_sstate->nr_segs)
	{
		fdesc = nvme_sstate_open_segment(rd_smgr, i);
		if (!trackRawFileDesc(gcontext, fdesc, __FILE__, __LINE__))
		{
			close(fdesc);
			elog(ERROR, "out of memory");
		}
		nvme_sstate->fdesc[i] = fdesc;
	}
#endif
}

/*
 * PDS_init_heapscan_state - construct a per-query state for heap-scan
 * with KDS_FORMAT_BLOCK / NVMe-Strom.
 */
void
PDS_init_heapscan_state(GpuTaskState *gts,
						cl_uint nrows_per_block)
{
	GpuContext	   *gcontext = gts->gcontext;
	Relation		relation = gts->css.ss.ss_currentRelation;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	EState		   *estate = gts->css.ss.ps.state;
	BlockNumber		nr_blocks;
	BlockNumber		nr_segs;
	NVMEScanState  *nvme_sstate;
	cl_uint			nrooms_max;
	cl_uint			nchunks;
	cl_uint			nblocks_per_chunk;

	/* check storage capability and relation's size */
	if (!RelationWillUseNvmeStrom(relation, &nr_blocks))
		return;

	/*
	 * Calculation of an optimal number of data-blocks for each PDS.
	 *
	 * We intend to load maximum available blocks onto the PDS which has
	 * configured chunk size, however, it will lead unbalanced smaller
	 * chunk around the bound of storage file segment.
	 * (Typically, 32 of 4091blocks + 1 of 160 blocks)
	 * So, we will adjust @nblocks_per_chunk to balance chunk size all
	 * around the relation scan.
	 */
	nrooms_max = (pgstrom_chunk_size() -
				  KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts))
		/ (sizeof(BlockNumber) + BLCKSZ);
	while (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) +
		   STROMALIGN(sizeof(BlockNumber) * nrooms_max) +
		   BLCKSZ * nrooms_max > pgstrom_chunk_size())
		nrooms_max--;
	if (nrooms_max < 1)
		return;

	nchunks = (RELSEG_SIZE + nrooms_max - 1) / nrooms_max;
	nblocks_per_chunk = (RELSEG_SIZE + nchunks - 1) / nchunks;

	/* allocation of NVMEScanState structure */
	nr_segs = (nr_blocks + (BlockNumber) RELSEG_SIZE - 1) / RELSEG_SIZE;
	nvme_sstate = MemoryContextAllocZero(estate->es_query_cxt,
										 offsetof(NVMEScanState,
												  fdesc[nr_segs]));
	nvme_sstate->nrows_per_block = nrows_per_block;
	nvme_sstate->nblocks_per_chunk = nblocks_per_chunk;
	nvme_sstate->curr_segno = InvalidBlockNumber;
	nvme_sstate->curr_vmbuffer = InvalidBuffer;
	nvme_sstate->nr_segs = nr_segs;
	nvme_sstate_open_files(gcontext, nvme_sstate, relation);

	gts->nvme_sstate = nvme_sstate;
}

/*
 * PDS_end_heapscan_state
 */
void
PDS_end_heapscan_state(GpuTaskState *gts)
{
	GpuContext	   *gcontext = gts->gcontext;
	NVMEScanState  *nvme_sstate = gts->nvme_sstate;
	int		i, fdesc;

	if (nvme_sstate)
	{
		/* release visibility map, if any */
		if (nvme_sstate->curr_vmbuffer != InvalidBuffer)
		{
			ReleaseBuffer(nvme_sstate->curr_vmbuffer);
			nvme_sstate->curr_vmbuffer = InvalidBuffer;
		}
		/* close file descriptors, if any */
		for (i=0; i < nvme_sstate->nr_segs; i++)
		{
			fdesc = nvme_sstate->fdesc[i];
			untrackRawFileDesc(gcontext, fdesc);
			if (close(fdesc))
				elog(NOTICE, "failed on close(%d): %m", fdesc);
		}
		pfree(nvme_sstate);
		gts->nvme_sstate = NULL;
	}
}

/*
 * PDS_exec_heapscan_block - PDS scan for KDS_FORMAT_BLOCK format
 */
static bool
PDS_exec_heapscan_block(pgstrom_data_store *pds,
						Relation relation,
						HeapScanDesc hscan,
						NVMEScanState *nvme_sstate)
{
	BlockNumber		blknum = hscan->rs_cblock;
	BlockNumber	   *block_nums;
	Snapshot		snapshot = hscan->rs_snapshot;
	BufferAccessStrategy strategy = hscan->rs_strategy;
	SMgrRelation	smgr = relation->rd_smgr;
	Buffer			buffer;
	Page			spage;
	Page			dpage;
	cl_uint			nr_loaded;
	bool			all_visible;

	/* PDS cannot eat any blocks more, obviously */
	if (pds->kds.nitems >= pds->kds.nrooms)
		return false;

	/* array of block numbers */
	block_nums = (BlockNumber *)KERN_DATA_STORE_BODY(&pds->kds);

	/*
	 * NVMe-Strom can be applied only when filesystem supports the feature,
	 * and the current source block is all-visible.
	 * Elsewhere, we will go fallback with synchronized buffer scan.
	 */
	if (RelationCanUseNvmeStrom(relation) &&
		VM_ALL_VISIBLE(relation, blknum,
					   &nvme_sstate->curr_vmbuffer))
	{
		BufferTag	newTag;
		uint32		newHash;
		LWLock	   *newPartitionLock = NULL;
		bool		retval;
		int			buf_id;

		/* create a tag so we can lookup the buffer */
		INIT_BUFFERTAG(newTag, smgr->smgr_rnode.node, MAIN_FORKNUM, blknum);
		/* determine its hash code and partition lock ID */
		newHash = BufTableHashCode(&newTag);
		newPartitionLock = BufMappingPartitionLock(newHash);

		/* check whether the block exists on the shared buffer? */
		LWLockAcquire(newPartitionLock, LW_SHARED);
		buf_id = BufTableLookup(&newTag, newHash);
		if (buf_id < 0)
		{
			BlockNumber	segno = blknum / RELSEG_SIZE;
			int			filedesc;

			Assert(segno < nvme_sstate->nr_segs);
			/*
			 * We cannot mix up multiple source files in a single PDS chunk.
			 * If heapscan_block comes across segment boundary, rest of the
			 * blocks must be read on the next PDS chunk.
			 */
			filedesc = nvme_sstate->fdesc[segno];
			if (pds->filedesc >= 0 && pds->filedesc != filedesc)
				retval = false;
			else
			{
				if (pds->filedesc < 0)
					pds->filedesc = filedesc;
				/* add uncached block for direct load */
				pds->nblocks_uncached++;
				pds->kds.nitems++;
				block_nums[pds->kds.nrooms - pds->nblocks_uncached] = blknum;

				retval = true;
			}
			LWLockRelease(newPartitionLock);
			return retval;
		}
		LWLockRelease(newPartitionLock);
	}
	/*
	 * Load the source buffer with synchronous read
	 */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, blknum,
								RBM_NORMAL, strategy);
#if 1
	/* Just like heapgetpage(), however, jobs we focus on is OLAP
	 * workload, so it's uncertain whether we should vacuum the page
	 * here.
	 */
	heap_page_prune_opt(relation, buffer);
#endif
	/* we will check tuple's visibility under the shared lock */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	nr_loaded = pds->kds.nitems - pds->nblocks_uncached;
	spage = (Page) BufferGetPage(buffer);
	dpage = (Page) KERN_DATA_STORE_BLOCK_PGPAGE(&pds->kds, nr_loaded);
	memcpy(dpage, spage, BLCKSZ);
	block_nums[nr_loaded] = blknum;

	/*
	 * Logic is almost same as heapgetpage() doing. We have to invalidate
	 * invisible tuples prior to GPU kernel execution, if not all-visible.
	 */
	all_visible = PageIsAllVisible(dpage) && !snapshot->takenDuringRecovery;
	if (!all_visible)
	{
		int				lines = PageGetMaxOffsetNumber(dpage);
		OffsetNumber	lineoff;
		ItemId			lpp;

		for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(dpage, lineoff);
			 lineoff <= lines;
			 lineoff++, lpp++)
		{
			HeapTupleData	tup;
			bool			valid;

			if (!ItemIdIsNormal(lpp))
				continue;

			tup.t_tableOid = RelationGetRelid(relation);
			tup.t_data = (HeapTupleHeader) PageGetItem((Page) dpage, lpp);
			tup.t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&tup.t_self, blknum, lineoff);

			valid = HeapTupleSatisfiesVisibility(&tup, snapshot, buffer);
			CheckForSerializableConflictOut(valid, relation, &tup,
											buffer, snapshot);
			if (!valid)
				ItemIdSetUnused(lpp);
		}
	}
	UnlockReleaseBuffer(buffer);
	/* dpage became all-visible also */
	PageSetAllVisible(dpage);
	pds->kds.nitems++;

	return true;
}

/*
 * PDS_exec_heapscan_row - PDS scan for KDS_FORMAT_ROW format
 */
static bool
PDS_exec_heapscan_row(pgstrom_data_store *pds,
					  Relation relation,
					  HeapScanDesc hscan)
{
	BlockNumber		blknum = hscan->rs_cblock;
	Snapshot		snapshot = hscan->rs_snapshot;
	BufferAccessStrategy strategy = hscan->rs_strategy;
	kern_data_store	*kds = &pds->kds;
	Buffer			buffer;
	Page			page;
	int				lines;
	int				ntup;
	OffsetNumber	lineoff;
	ItemId			lpp;
	uint		   *tup_index;
	kern_tupitem   *tup_item;
	bool			all_visible;
	Size			max_consume;

	/* Load the target buffer */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, blknum,
								RBM_NORMAL, strategy);

#if 1
	/* Just like heapgetpage(), however, jobs we focus on is OLAP
	 * workload, so it's uncertain whether we should vacuum the page
	 * here.
	 */
	heap_page_prune_opt(relation, buffer);
#endif

	/* we will check tuple's visibility under the shared lock */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(page);
	ntup = 0;

	/*
	 * Check whether we have enough rooms to store expected number of
	 * tuples on the remaining space. If it is hopeless to load all
	 * the items in a block, we inform the caller this block shall be
	 * loaded on the next data store.
	 */
	max_consume = KDS_CALCULATE_HASH_LENGTH(kds->ncols,
											kds->nitems + lines,
											offsetof(kern_tupitem,
													 htup) * lines +
											BLCKSZ + kds->usage);
	if (max_consume > kds->length)
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/*
	 * Logic is almost same as heapgetpage() doing.
	 */
	all_visible = PageIsAllVisible(page) && !snapshot->takenDuringRecovery;

	/* TODO: make SerializationNeededForRead() an external function
	 * on the core side. It kills necessity of setting up HeapTupleData
	 * when all_visible and non-serialized transaction.
	 */
	tup_index = KERN_DATA_STORE_ROWINDEX(kds) + kds->nitems;
	for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(page, lineoff);
		 lineoff <= lines;
		 lineoff++, lpp++)
	{
		HeapTupleData	tup;
		bool			valid;

		if (!ItemIdIsNormal(lpp))
			continue;

		tup.t_tableOid = RelationGetRelid(relation);
		tup.t_data = (HeapTupleHeader) PageGetItem((Page) page, lpp);
		tup.t_len = ItemIdGetLength(lpp);
		ItemPointerSet(&tup.t_self, blknum, lineoff);

		if (all_visible)
			valid = true;
		else
			valid = HeapTupleSatisfiesVisibility(&tup, snapshot, buffer);

		CheckForSerializableConflictOut(valid, relation,
										&tup, buffer, snapshot);
		if (!valid)
			continue;

		/* put tuple */
		kds->usage += LONGALIGN(offsetof(kern_tupitem, htup) + tup.t_len);
		tup_item = (kern_tupitem *)((char *)kds + kds->length - kds->usage);
		tup_index[ntup] = (uintptr_t)tup_item - (uintptr_t)kds;
		tup_item->t_len = tup.t_len;
		tup_item->t_self = tup.t_self;
		memcpy(&tup_item->htup, tup.t_data, tup.t_len);

		ntup++;
	}
	UnlockReleaseBuffer(buffer);
	Assert(ntup <= MaxHeapTuplesPerPage);
	Assert(kds->nitems + ntup <= kds->nrooms);
	kds->nitems += ntup;

	return true;
}

/*
 * PDS_exec_heapscan - PDS scan entrypoint
 */
bool
PDS_exec_heapscan(GpuTaskState *gts, pgstrom_data_store *pds)
{
	Relation		relation = gts->css.ss.ss_currentRelation;
	HeapScanDesc	hscan = gts->css.ss.ss_currentScanDesc;
	bool			retval;

	CHECK_FOR_INTERRUPTS();

	if (pds->kds.format == KDS_FORMAT_ROW)
		retval = PDS_exec_heapscan_row(pds, relation, hscan);
	else if (pds->kds.format == KDS_FORMAT_BLOCK)
	{
		Assert(gts->nvme_sstate);
		retval = PDS_exec_heapscan_block(pds, relation, hscan,
										 gts->nvme_sstate);
	}
	else
		elog(ERROR, "Bug? unexpected PDS format: %d", pds->kds.format);

	return retval;
}

/*
 * PDS_insert_tuple
 *
 * It inserts a tuple onto the data store. Unlike block read mode, we cannot
 * use this API only for row-format.
 */
bool
KDS_insert_tuple(kern_data_store *kds, TupleTableSlot *slot)
{
	size_t				required;
	HeapTuple			tuple;
	cl_uint			   *tup_index;
	kern_tupitem	   *tup_item;

	/* No room to store a new kern_rowitem? */
	if (kds->nitems >= kds->nrooms)
		return false;
	Assert(kds->ncols == slot->tts_tupleDescriptor->natts);

	if (kds->format != KDS_FORMAT_ROW)
		elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);

	/* OK, put a record */
	tup_index = KERN_DATA_STORE_ROWINDEX(kds);

	/* reference a HeapTuple in TupleTableSlot */
	tuple = ExecFetchSlotTuple(slot);

	/* check whether we have room for this tuple */
	required = LONGALIGN(offsetof(kern_tupitem, htup) + tuple->t_len);
	if (KDS_CALCULATE_ROW_LENGTH(kds->ncols,
								 kds->nitems + 1,
								 required + kds->usage) > kds->length)
		return false;

	kds->usage += required;
	tup_item = (kern_tupitem *)((char *)kds + kds->length - kds->usage);
	tup_item->t_len = tuple->t_len;
	tup_item->t_self = tuple->t_self;
	memcpy(&tup_item->htup, tuple->t_data, tuple->t_len);
	tup_index[kds->nitems++] = (uintptr_t)tup_item - (uintptr_t)kds;

	return true;
}


/*
 * PDS_insert_hashitem
 *
 * It inserts a tuple to the data store of hash format.
 */
bool
KDS_insert_hashitem(kern_data_store *kds,
					TupleTableSlot *slot,
					cl_uint hash_value)
{
	cl_uint			   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
	Size				required;
	HeapTuple			tuple;
	kern_hashitem	   *khitem;

	/* No room to store a new kern_hashitem? */
	if (kds->nitems >= kds->nrooms)
		return false;
	Assert(kds->ncols == slot->tts_tupleDescriptor->natts);

	/* KDS has to be KDS_FORMAT_HASH */
	if (kds->format != KDS_FORMAT_HASH)
		elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);

	/* compute required length */
	tuple = ExecFetchSlotTuple(slot);
	required = MAXALIGN(offsetof(kern_hashitem, t.htup) + tuple->t_len);

	Assert(kds->usage == MAXALIGN(kds->usage));
	if (KDS_CALCULATE_HASH_LENGTH(kds->ncols,
								  kds->nitems + 1,
								  required + kds->usage) > kds->length)
		return false;	/* no more space to put */

	/* OK, put a tuple */
	Assert(kds->usage == MAXALIGN(kds->usage));
	khitem = (kern_hashitem *)((char *)kds + kds->length
							   - (kds->usage + required));
	kds->usage += required;
	khitem->hash = hash_value;
	khitem->next = 0x7f7f7f7f;	/* to be set later */
	khitem->rowid = kds->nitems++;
	khitem->t.t_len = tuple->t_len;
	khitem->t.t_self = tuple->t_self;
	memcpy(&khitem->t.htup, tuple->t_data, tuple->t_len);

	row_index[khitem->rowid] = (cl_uint)((uintptr_t)&khitem->t.t_len -
										 (uintptr_t)kds);
	return true;
}

/*
 * PDS_fillup_blocks
 *
 * It fills up uncached blocks using synchronous read APIs.
 */
void
PDS_fillup_blocks(pgstrom_data_store *pds)
{
	cl_int			filedesc = pds->filedesc;
	cl_int			i, nr_loaded;
	ssize_t			nbytes;
	char		   *dest_addr;
	loff_t			curr_fpos;
	size_t			curr_size;
	BlockNumber	   *block_nums;

	if (pds->kds.format != KDS_FORMAT_BLOCK)
		elog(ERROR, "Bug? only KDS_FORMAT_BLOCK can be filled up");

	if (pds->nblocks_uncached == 0)
		return;		/* already filled up */

	Assert(filedesc >= 0);
	Assert(pds->nblocks_uncached <= pds->kds.nitems);
	nr_loaded = pds->kds.nitems - pds->nblocks_uncached;
	block_nums = (BlockNumber *)KERN_DATA_STORE_BODY(&pds->kds);
	dest_addr = (char *)KERN_DATA_STORE_BLOCK_PGPAGE(&pds->kds, nr_loaded);
	curr_fpos = 0;
	curr_size = 0;
	for (i=pds->nblocks_uncached-1; i >=0; i--)
	{
		loff_t	file_pos = (block_nums[i] & (RELSEG_SIZE - 1)) * BLCKSZ;

		if (curr_size > 0 &&
			curr_fpos + curr_size == file_pos)
		{
			/* merge with the pending i/o */
			curr_size += BLCKSZ;
		}
		else
		{
			while (curr_size > 0)
			{
				nbytes = pread(filedesc, dest_addr, curr_size, curr_fpos);
				Assert(nbytes <= curr_size);
				if (nbytes < 0 || (nbytes == 0 && errno != EINTR))
					elog(ERROR, "failed on pread(2): %m");
				dest_addr += nbytes;
				curr_fpos += nbytes;
				curr_size -= nbytes;
			}
			curr_fpos = file_pos;
			curr_size = BLCKSZ;
		}
	}

	while (curr_size > 0)
	{
		nbytes = pread(filedesc, dest_addr, curr_size, curr_fpos);
		Assert(nbytes <= curr_size);
		if (nbytes < 0 || (nbytes == 0 && errno != EINTR))
			elog(ERROR, "failed on pread(2): %m");
		dest_addr += nbytes;
		curr_fpos += nbytes;
		curr_size -= nbytes;
	}
	Assert(dest_addr == (char *)KERN_DATA_STORE_BLOCK_PGPAGE(&pds->kds,
															 pds->kds.nitems));
	pds->nblocks_uncached = 0;
}
