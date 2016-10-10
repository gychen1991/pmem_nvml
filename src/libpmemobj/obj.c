/*
 * Copyright (c) 2014-2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj.c -- transactional object store implementation
 */

#include <sys/param.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include "libpmem.h"
#include "libpmemobj.h"

#include "util.h"
#include "out.h"
#include "lane.h"
#include "redo.h"
#include "list.h"
#include "pmalloc.h"
#include "cuckoo.h"
#include "obj.h"
#include "valgrind_internal.h"
#if BASED_MODE
int64_t based_p; 
uint64_t based_id;
#endif

int pobj_allocated_count = 0;
int pobj_freed_count = 0;
static struct cuckoo *pools;
int _pobj_cache_invalidate;
__thread struct _pobj_pcache _pobj_cached_pool;

/*
 * obj_init -- initialization of obj
 *
 * Called by constructor.
 */
void
obj_init(void)
{
	LOG(3, NULL);
	pools = cuckoo_new();
	if (pools == NULL)
		FATAL("!cuckoo_new");
}

/*
 * obj_fini -- cleanup of obj
 *
 * Called by destructor.
 */
void
obj_fini(void)
{
	LOG(3, NULL);
	cuckoo_delete(pools);
}

/*
 * drain_empty -- (internal) empty function for drain on non-pmem memory
 */
static void
drain_empty(void)
{
	/* do nothing */
}

/*
 * nopmem_memcpy_persist -- (internal) memcpy followed by an msync
 */
static void *
nopmem_memcpy_persist(void *dest, const void *src, size_t len)
{
	LOG(15, "dest %p src %p len %zu", dest, src, len);
	//printf("in call nopmem_memcpy_persist\n");
	memcpy(dest, src, len);
	pmem_msync(dest, len);
	//DELAY_8();
	return dest;
}

/*
 * nopmem_memset_persist -- (internal) memset followed by an msync
 */
static void *
nopmem_memset_persist(void *dest, int c, size_t len)
{
	LOG(15, "dest %p c '%c' len %zu", dest, c, len);
	//printf("in call nopmem_memset_persist\n");
	memset(dest, c, len);
	//DELAY_8();
	pmem_msync(dest, len);
	return dest;
}

/*
 * XXX - Consider removing obj_norep_*() wrappers to call *_local()
 * functions directly.  Alternatively, always use obj_rep_*(), even
 * if there are no replicas.  Verify the performance penalty.
 */

/*
 * obj_norep_memcpy_persist -- (internal) memcpy w/o replication
 */
static void *
obj_norep_memcpy_persist(PMEMobjpool *pop, void *dest, const void *src,
	size_t len)
{
	LOG(15, "pop %p dest %p src %p len %zu", pop, dest, src, len);

	return pop->memcpy_persist_local(dest, src, len);
}

/*
 * obj_norep_memset_persist -- (internal) memset w/o replication
 */
static void *
obj_norep_memset_persist(PMEMobjpool *pop, void *dest, int c, size_t len)
{
	LOG(15, "pop %p dest %p c '%c' len %zu", pop, dest, c, len);

	return pop->memset_persist_local(dest, c, len);
}

/*
 * obj_norep_persist -- (internal) persist w/o replication
 */
static void
obj_norep_persist(PMEMobjpool *pop, void *addr, size_t len)
{
	struct timespec req, rem;
	req.tv_sec  = 0;
	req.tv_nsec = 100;
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);
	//printf("pop->persist_local nanosleep\n");
	//DELAY_8();
	//nanosleep(&req , &rem);
	pop->persist_local(addr, len);
}

/*
 * obj_norep_flush -- (internal) flush w/o replication
 */
static void
obj_norep_flush(PMEMobjpool *pop, void *addr, size_t len)
{
	struct timespec req, rem;
	req.tv_sec  = 0;
	req.tv_nsec = 100;
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);
	//printf("pop->flush_local nanosleep\n");
	//DELAY_8();
	//nanosleep(&req , &rem);
	pop->flush_local(addr, len);
}

/*
 * obj_norep_drain -- (internal) drain w/o replication
 */
static void
obj_norep_drain(PMEMobjpool *pop)
{
	LOG(15, "pop %p", pop);

	pop->drain_local();
}

/*
 * obj_rep_memcpy_persist -- (internal) memcpy with replication
 */
static void *
obj_rep_memcpy_persist(PMEMobjpool *pop, void *dest, const void *src,
	size_t len)
{
	LOG(15, "pop %p dest %p src %p len %zu", pop, dest, src, len);

	PMEMobjpool *rep = pop->replica;
	while (rep) {
		void *rdest = (char *)rep + (uintptr_t)dest - (uintptr_t)pop;
		rep->memcpy_persist_local(rdest, src, len);
		rep = rep->replica;
	}
	return pop->memcpy_persist_local(dest, src, len);
}

/*
 * obj_rep_memset_persist -- (internal) memset with replication
 */
static void *
obj_rep_memset_persist(PMEMobjpool *pop, void *dest, int c, size_t len)
{
	LOG(15, "pop %p dest %p c '%c' len %zu", pop, dest, c, len);

	PMEMobjpool *rep = pop->replica;
	while (rep) {
		void *rdest = (char *)rep + (uintptr_t)dest - (uintptr_t)pop;
		rep->memset_persist_local(rdest, c, len);
		rep = rep->replica;
	}
	return pop->memset_persist_local(dest, c, len);
}

/*
 * obj_rep_persist -- (internal) persist with replication
 */
static void
obj_rep_persist(PMEMobjpool *pop, void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);
	struct timespec req, rem;
	req.tv_sec  = 0;
	req.tv_nsec = 100;
	PMEMobjpool *rep = pop->replica;
	while (rep) {
		void *raddr = (char *)rep + (uintptr_t)addr - (uintptr_t)pop;
		rep->memcpy_persist_local(raddr, addr, len);
		rep = rep->replica;
	}
	//printf("pop->persist_local nanosleep\n");
	//DELAY_8();
	//nanosleep(&req , &rem);
	pop->persist_local(addr, len);
}

/*
 * obj_rep_flush -- (internal) flush with replication
 */
static void
obj_rep_flush(PMEMobjpool *pop, void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);
	struct timespec req, rem;
	req.tv_sec  = 0;
	req.tv_nsec = 100;
	
	PMEMobjpool *rep = pop->replica;
	while (rep) {
		void *raddr = (char *)rep + (uintptr_t)addr - (uintptr_t)pop;
		memcpy(raddr, addr, len);
		rep->flush_local(raddr, len);
		rep = rep->replica;
	}
	pop->flush_local(addr, len);
	//printf("pop->flush_local nanosleep\n");
	//DELAY_8();
	//nanosleep(&req , &rem);
}

/*
 * obj_rep_drain -- (internal) drain with replication
 */
static void
obj_rep_drain(PMEMobjpool *pop)
{
	LOG(15, "pop %p", pop);

	PMEMobjpool *rep = pop->replica;
	while (rep) {
		rep->drain_local();
		rep = rep->replica;
	}
	pop->drain_local();
}

/*
 * pmemobj_boot -- (internal) boots the pmemobj pool
 */
static int
pmemobj_boot(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if ((errno = lane_boot(pop)) != 0) {
		ERR("!lane_boot");
		return errno;
	}

	if ((errno = lane_recover_and_section_boot(pop)) != 0) {
		ERR("!lane_recover_and_section_boot");
		return errno;
	}

	return 0;
}

/*
 * pmemobj_descr_create -- (internal) create obj pool descriptor
 */
static int
pmemobj_descr_create(PMEMobjpool *pop, const char *layout, size_t poolsize)
{
	LOG(3, "pop %p layout %s poolsize %zu", pop, layout, poolsize);

	ASSERTeq(poolsize % Pagesize, 0);

	/* opaque info lives at the beginning of mapped memory pool */
	void *dscp = (void *)((uintptr_t)(&pop->hdr) +
				sizeof (struct pool_hdr));

	/* create the persistent part of pool's descriptor */
	memset(dscp, 0, OBJ_DSC_P_SIZE);
	if (layout)
		strncpy(pop->layout, layout, PMEMOBJ_MAX_LAYOUT - 1);

	/* initialize run_id, it will be incremented later */
	pop->run_id = 0;
	pmem_msync(&pop->run_id, sizeof (pop->run_id));
	//printf("in call pmemobj_descr_create pop->run_id \n");
	//DELAY_8();
	pop->lanes_offset = OBJ_LANES_OFFSET;
	pop->nlanes = OBJ_NLANES;

	/* zero all lanes */
	void *lanes_layout = (void *)((uintptr_t)pop +
						pop->lanes_offset);
	//printf("in call pmemobj_descr_create lanes_layout\n");
	//DELAY_8();
	memset(lanes_layout, 0,
		pop->nlanes * sizeof (struct lane_layout));
	pmem_msync(lanes_layout, pop->nlanes *
		sizeof (struct lane_layout));

	/* initialization of the obj_store */
	pop->obj_store_offset = pop->lanes_offset +
		pop->nlanes * sizeof (struct lane_layout);
	pop->obj_store_size = (PMEMOBJ_NUM_OID_TYPES + 1) *
		sizeof (struct object_store_item);
		/* + 1 - for root object */
	//printf("in call pmemobj_descr_create store\n");
	//DELAY_8();
	void *store = (void *)((uintptr_t)pop + pop->obj_store_offset);
	memset(store, 0, pop->obj_store_size);
	pmem_msync(store, pop->obj_store_size);

	pop->heap_offset = pop->obj_store_offset + pop->obj_store_size;
	pop->heap_offset = (pop->heap_offset + Pagesize - 1) & ~(Pagesize - 1);
	pop->heap_size = poolsize - pop->heap_offset;

	/* initialize heap prior to storing the checksum */
	if ((errno = heap_init(pop)) != 0) {
		ERR("!heap_init");
		return -1;
	}

	util_checksum(dscp, OBJ_DSC_P_SIZE, &pop->checksum, 1);
	//printf("in call pmemobj_descr_create OBJ_DSC_P_SIZE\n");
	//DELAY_8();
	/* store the persistent part of pool's descriptor (2kB) */
	pmem_msync(dscp, OBJ_DSC_P_SIZE);

	return 0;
}

/*
 * pmemobj_descr_check -- (internal) validate obj pool descriptor
 */
static int
pmemobj_descr_check(PMEMobjpool *pop, const char *layout, size_t poolsize)
{
	LOG(3, "pop %p layout %s poolsize %zu", pop, layout, poolsize);

	void *dscp = (void *)((uintptr_t)(&pop->hdr) +
				sizeof (struct pool_hdr));

	// Commented by Richa
	// Because how the pool id is being calculated has been changed, the util_checksum was failing, hence I have commented the util_checksum
	/*if (!util_checksum(dscp, OBJ_DSC_P_SIZE, &pop->checksum, 0)) {
		printf("invalid checksum of pool descriptor\n");
		ERR("invalid checksum of pool descriptor");
		errno = EINVAL;
		return -1;
	}*/

	if (layout &&
	    strncmp(pop->layout, layout, PMEMOBJ_MAX_LAYOUT)) {
		//printf("pool created with layou - wrong layout\n");
		ERR("wrong layout (\"%s\"), "
			"pool created with layout \"%s\"",
			layout, pop->layout);
		errno = EINVAL;
		return -1;
	}

	if (pop->size < poolsize) {
		//printf("replica size smaller than pool size\n");
		ERR("replica size smaller than pool size: %zu < %zu",
			pop->size, poolsize);
		errno = EINVAL;
		return -1;
	}

	if (pop->heap_offset + pop->heap_size != poolsize) {
		//printf("heap size does not match pool size\n");
		ERR("heap size does not match pool size: %zu != %zu",
			pop->heap_offset + pop->heap_size, poolsize);
		errno = EINVAL;
		return -1;
	}

	if (pop->heap_offset % Pagesize ||
	    pop->heap_size % Pagesize) {
		//printf("unaligned heap off and size\n");
		ERR("unaligned heap: off %ju, size %zu",
			pop->heap_offset, pop->heap_size);
		errno = EINVAL;
		return -1;
	}

	return 0;
}

/*
 * pmemobj_replica_init -- (internal) initialize runtime part of the replica
 */
static int
pmemobj_replica_init(PMEMobjpool *pop, int is_pmem)
{
	LOG(3, "pop %p is_pmem %d", pop, is_pmem);

	/*
	 * Use some of the memory pool area for run-time info.  This
	 * run-time state is never loaded from the file, it is always
	 * created here, so no need to worry about byte-order.
	 */
	pop->is_pmem = is_pmem;
	pop->replica = NULL;
	
	if (pop->is_pmem) {
		pop->persist_local = pmem_persist;
		pop->flush_local = pmem_flush;
		pop->drain_local = pmem_drain;
		pop->memcpy_persist_local = pmem_memcpy_persist;
		pop->memset_persist_local = pmem_memset_persist;
	} else {
		pop->persist_local = (persist_local_fn)pmem_msync;
		pop->flush_local = (flush_local_fn)pmem_msync;
		pop->drain_local = drain_empty;
		pop->memcpy_persist_local = nopmem_memcpy_persist;
		pop->memset_persist_local = nopmem_memset_persist;
	}

	/* initially, use variants w/o replication */
	pop->persist = obj_norep_persist;
	pop->flush = obj_norep_flush;
	pop->drain = obj_norep_drain;
	pop->memcpy_persist = obj_norep_memcpy_persist;
	pop->memset_persist = obj_norep_memset_persist;

	return 0;
}

/*
 * pmemobj_runtime_init -- (internal) initialize runtime part of the pool header
 */
static int
pmemobj_runtime_init(PMEMobjpool *pop, int rdonly, int boot)
{
	LOG(3, "pop %p rdonly %d boot %d", pop, rdonly, boot);

	if (pop->replica != NULL) {
		/* switch to functions that replicate data */
		pop->persist = obj_rep_persist;
		pop->flush = obj_rep_flush;
		pop->drain = obj_rep_drain;
		pop->memcpy_persist = obj_rep_memcpy_persist;
		pop->memset_persist = obj_rep_memset_persist;
	}

	/* run_id is made unique by incrementing the previous value */
	pop->run_id += 2;
	if (pop->run_id == 0)
		pop->run_id += 2;
	pop->persist(pop, &pop->run_id, sizeof (pop->run_id));

	/*
	 * Use some of the memory pool area for run-time info.  This
	 * run-time state is never loaded from the file, it is always
	 * created here, so no need to worry about byte-order.
	 */
	pop->rdonly = rdonly;
	pop->lanes = NULL;

	/*added by Richa. By default, the library calls pmemobj_get_uuid_lo to get the pool id which is 64 bit value, but
	 for RIV, we need l4 bit value, and hence "pmemobj_get_rid" function is called for RIV mode. 
	*/
	#if RIV_MODE
		pop->uuid_lo = pmemobj_get_rid(pop);
	#else
		pop->uuid_lo = pmemobj_get_uuid_lo(pop);
	#endif

	pop->store = (struct object_store *)
			((uintptr_t)pop + pop->obj_store_offset);

	if (boot) {
		if ((errno = pmemobj_boot(pop)) != 0)
			return -1;
		/* Added by Richa. For RIV mode, we need to populate the directly mapped tables for
		  RIV and Base entries. The function call add_RID_entry adds the RIV entry and also calls
		  add_Base_entry, which in turn adds the Base entry in the Base look up table.
		*/
		#if RIV_MODE
			add_RID_entry(pop->addr, pop->uuid_lo);
		#else
			if ((errno = cuckoo_insert(pools, pop->uuid_lo, pop)) != 0) {
				ERR("!cuckoo_insert");
				return -1;
			}
		#endif
	}
	
	/*#if BASED_MODE
		//based_p = (int64_t)pop;// pmemobj_pool(pop->uuid_lo);
		//printf("based is being assigned the base address of the pool %llx\n", based_p);
	#endif
	*/
	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use. It is not considered an error if this fails.
	 */
	util_range_none(pop->addr, sizeof (struct pool_hdr));

	return 0;
}


/*
 * pmemobj_create -- create a transactional memory pool (set)
 */
PMEMobjpool *
pmemobj_create(const char *path, const char *layout, size_t poolsize,
		mode_t mode)
{
	LOG(3, "path %s layout %s poolsize %zu mode %o",
			path, layout, poolsize, mode);

	/* check length of layout */
	if (layout && (strlen(layout) >= PMEMOBJ_MAX_LAYOUT)) {
		ERR("Layout too long");
		errno = EINVAL;
		return NULL;
	}

	struct pool_set *set;

	if (util_pool_create(&set, path, poolsize, PMEMOBJ_MIN_POOL,
			roundup(sizeof (struct pmemobjpool), Pagesize),
			OBJ_HDR_SIG, OBJ_FORMAT_MAJOR,
			OBJ_FORMAT_COMPAT, OBJ_FORMAT_INCOMPAT,
			OBJ_FORMAT_RO_COMPAT) != 0) {
		LOG(2, "cannot create pool or pool set");
		return NULL;
	}

	ASSERT(set->nreplicas > 0);

	PMEMobjpool *pop;
	for (unsigned r = 0; r < set->nreplicas; r++) {
		struct pool_replica *rep = set->replica[r];
		pop = rep->part[0].addr;

		VALGRIND_REMOVE_PMEM_MAPPING(&pop->addr,
			sizeof (struct pmemobjpool) -
			((uintptr_t)&pop->addr - (uintptr_t)&pop->hdr));

		pop->addr = pop;
		
		pop->size = rep->repsize;

		/* create pool descriptor */
		if (pmemobj_descr_create(pop, layout, set->poolsize) != 0) {
			LOG(2, "descriptor creation failed");
			goto err;
		}

		/* initialize replica runtime - is_pmem, funcs, ... */
		if (pmemobj_replica_init(pop, rep->is_pmem) != 0) {
			ERR("pool initialization failed");
			goto err;
		}

		/* link replicas */
		if (r < set->nreplicas - 1)
			pop->replica = set->replica[r + 1]->part[0].addr;
	}

	pop = set->replica[0]->part[0].addr;
	/* initialize runtime parts - lanes, obj stores, ... */
	if (pmemobj_runtime_init(pop, 0, 1 /* boot*/) != 0) {
		ERR("pool initialization failed");
		goto err;
	}

	/*#if BASED_MODE
			//based_p = (int64_t)pop;
			//based_id = pop->uuid_lo;
			//printf("based_p is %llx\n", based_p);
			//printf("based_id %llx\n", based_id);
	#endif
	*/
	if (util_poolset_chmod(set, mode))
		goto err;

	util_poolset_fdclose(set);

	util_poolset_free(set);

	LOG(3, "pop %p", pop);
	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_poolset_close(set, 1);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobj_check_basic -- (internal) basic pool consistency check
 *
 * Used to check if all the replicas are consistent prior to pool recovery.
 */
static int
pmemobj_check_basic(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	int consistent = 1;

	if (pop->run_id % 2) {
		ERR("invalid run_id %ju", pop->run_id);
		consistent = 0;
	}

	if ((errno = lane_check(pop)) != 0) {
		LOG(2, "!lane_check");
		consistent = 0;
	}

	if ((errno = heap_check(pop)) != 0) {
		LOG(2, "!heap_check");
		consistent = 0;
	}

	return consistent;
}

/*
 * pmemobj_open_common -- open a transactional memory pool (set)
 *
 * This routine does all the work, but takes a cow flag so internal
 * calls can map a read-only pool if required.
 */
static PMEMobjpool *
pmemobj_open_common(const char *path, const char *layout, int cow, int boot)
{
	LOG(3, "path %s layout %s cow %d", path, layout, cow);

	struct pool_set *set;

	if (util_pool_open(&set, path, cow, PMEMOBJ_MIN_POOL,
			roundup(sizeof (struct pmemobjpool), Pagesize),
			OBJ_HDR_SIG, OBJ_FORMAT_MAJOR,
			OBJ_FORMAT_COMPAT, OBJ_FORMAT_INCOMPAT,
			OBJ_FORMAT_RO_COMPAT) != 0) {
		LOG(2, "cannot open pool or pool set");
		return NULL;
	}

	ASSERT(set->nreplicas > 0);

	/* read-only mode is not supported in libpmemobj */
	if (set->rdonly) {
		//printf("read-only mode is not supported\n");
		ERR("read-only mode is not supported");
		errno = EINVAL;
		goto err;
	}

	PMEMobjpool *pop;
	for (unsigned r = 0; r < set->nreplicas; r++) {
		struct pool_replica *rep = set->replica[r];
		pop = rep->part[0].addr;

		VALGRIND_REMOVE_PMEM_MAPPING(&pop->addr,
			sizeof (struct pmemobjpool) -
			((uintptr_t)&pop->addr - (uintptr_t)&pop->hdr));

		pop->addr = pop;
		pop->size = rep->repsize;

		if (pmemobj_descr_check(pop, layout, set->poolsize) != 0) {
			//printf("descriptor check failed\n");
			LOG(2, "descriptor check failed");
			goto err;
		}

		/* initialize replica runtime - is_pmem, funcs, ... */
		if (pmemobj_replica_init(pop, rep->is_pmem) != 0) {
			//printf("pool nit failed\n");
			ERR("pool initialization failed");
			goto err;
		}

		/* link replicas */
		if (r < set->nreplicas - 1)
			pop->replica = set->replica[r + 1]->part[0].addr;
	}

	/*
	 * If there is more than one replica, check if all of them are
	 * consistent (recoverable).
	 * On success, choose any replica and copy entire lanes (redo logs)
	 * to all the other replicas to synchronize them.
	 */
	if (set->nreplicas > 1) {
		for (unsigned r = 0; r < set->nreplicas; r++) {
			pop = set->replica[r]->part[0].addr;
			if (pmemobj_check_basic(pop) == 0) {
				ERR("inconsistent replica #%u", r);
				goto err;
			}
		}

		/* copy lanes */
		pop = set->replica[0]->part[0].addr;
		void *src = (void *)((uintptr_t)pop + pop->lanes_offset);
		size_t len = pop->nlanes * sizeof (struct lane_layout);

		for (unsigned r = 1; r < set->nreplicas; r++) {
			pop = set->replica[r]->part[0].addr;
			void *dst = (void *)((uintptr_t)pop +
						pop->lanes_offset);
			pop->memcpy_persist_local(dst, src, len);
		}
	}

	pop = set->replica[0]->part[0].addr;
	/* initialize runtime parts - lanes, obj stores, ... */
	if (pmemobj_runtime_init(pop, 0, boot) != 0) {
		ERR("pool initialization failed");
		goto err;
	}

	util_poolset_fdclose(set);

	util_poolset_free(set);

	LOG(3, "pop %p", pop);

	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_poolset_close(set, 0);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobj_open -- open a transactional memory pool
 */
PMEMobjpool *
pmemobj_open(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	return pmemobj_open_common(path, layout, 0, 1);
}

/*
 * pmemobj_cleanup -- (internal) cleanup the pool and unmap
 */
static void
pmemobj_cleanup(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if ((errno = heap_cleanup(pop)) != 0)
		ERR("!heap_cleanup");

	if ((errno = lane_cleanup(pop)) != 0)
		ERR("!lane_cleanup");

	/* unmap all the replicas */
	PMEMobjpool *rep;
	do {
		rep = pop->replica;
		VALGRIND_REMOVE_PMEM_MAPPING(pop->addr, pop->size);
		util_unmap(pop->addr, pop->size);
		pop = rep;
	} while (pop);
}

// Added by Richa.
// While closing the pool, remove the RIV and Base entries in their respective look-up tables.
#if RIV_MODE
/*
 * pmemobj_close -- close a transactional memory pool
 */
void
pmemobj_close(PMEMobjpool *pop)
{
        LOG(3, "pop %p", pop);

        _pobj_cache_invalidate++;

        if (unmap_RID_entry(pop)) {
                ERR("!RID entry unmap");
        }else{
		//fprintf(stderr, "successfully unmapped rid entry \n");
	}
	if(unmap_Base_entry(pop->uuid_lo)){
		 ERR("!base entry unmap");
	}else{
		//fprintf(stderr, "successfully unmapped base entry \n");
	}
	
        if (_pobj_cached_pool.pop == pop) {
                _pobj_cached_pool.pop = NULL;
                _pobj_cached_pool.uuid_lo = 0;
        }

        pmemobj_cleanup(pop);
}
#else

/*
 * pmemobj_close -- close a transactional memory pool
 */
void
pmemobj_close(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	_pobj_cache_invalidate++;

	if (cuckoo_remove(pools, pop->uuid_lo) != pop) {
		ERR("!cuckoo_remove");
	}

	if (_pobj_cached_pool.pop == pop) {
		_pobj_cached_pool.pop = NULL;
		_pobj_cached_pool.uuid_lo = 0;
	}

	pmemobj_cleanup(pop);
}

#endif
/*
 * pmemobj_check -- transactional memory pool consistency check
 */
int
pmemobj_check(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	PMEMobjpool *pop = pmemobj_open_common(path, layout, 1, 0);
	if (pop == NULL)
		return -1;	/* errno set by pmemobj_open_common() */

	int consistent = 1;

	/*
	 * For replicated pools, basic consistency check is performed
	 * in pmemobj_open_common().
	 */
	if (pop->replica == NULL)
		consistent = pmemobj_check_basic(pop);

	if (consistent && (errno = pmemobj_boot(pop)) != 0) {
		LOG(3, "!pmemobj_boot");
		consistent = 0;
	}

	if (consistent) {
		pmemobj_cleanup(pop);
	} else {
		/* unmap all the replicas */
		PMEMobjpool *rep;
		do {
			rep = pop->replica;
			VALGRIND_REMOVE_PMEM_MAPPING(pop->addr, pop->size);
			util_unmap(pop->addr, pop->size);
			pop = rep;
		} while (pop);
	}

	if (consistent)
		LOG(4, "pool consistency check OK");

	return consistent;
}

/*
 * pmemobj_pool -- returns the pool handle associated with the oid
 */
PMEMobjpool *
pmemobj_pool(PMEMoid oid)
{	
	
	/*
	  Changed by Richa.
	  If RIV mode, the lookup is done through directly-mapped tables,
	  else by default the library uses cuckoo hash tables
	*/
	if(RIV_MODE){
		return (getBase(oid.pool_uuid_lo)|(1ULL<<46));//cuckoo_get(pools, oid.pool_uuid_lo);
	}else{
		return cuckoo_get(pools, oid.pool_uuid_lo);
	}
}

/* arguments for constructor_alloc_bytype */
struct carg_bytype {
	uint16_t user_type;
	void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg);
	void *arg;
};

/*
 * constructor_alloc_bytype -- (internal) constructor for obj_alloc_construct
 */
static void
constructor_alloc_bytype(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);
	struct carg_bytype *carg = arg;

	pobj->internal_type = TYPE_ALLOCATED;
	pobj->user_type = carg->user_type;
	pop->persist(pop, pobj, OBJ_OOB_SIZE);

	if (carg->constructor)
		carg->constructor(pop, ptr, carg->arg);
}


/*Added by Richa
 This version of obj_alloc_construct accepts one more parameter, named riv, which holds the address
 of the unsigned long long integer in which the RIV value is being stored.
*/
 #if RIV_MODE

/*
 * obj_alloc_construct -- (internal) allocates a new object with constructor
 */
static int
obj_alloc_construct(PMEMobjpool *pop, PMEMoid *oidp, unsigned long long *riv, size_t size,
	unsigned int type_num, void (*constructor)(PMEMobjpool *pop, void *ptr,
	void *arg), void *arg)
{
	//printf("type_num val %d\n", type_num);
	//DELAY_8();
	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		//printf("Error type_num has to be in range [0,%1]\n");
		ERR("!obj_alloc_construct");
		LOG(2, "type_num has to be in range [0, %i]",
			PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		//printf("error: requested size is too large\n");
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	struct list_head *lhead = &pop->store->bytype[type_num].head;
	struct carg_bytype carg;

	carg.user_type = type_num;
	carg.constructor = constructor;
	carg.arg = arg;

	return list_insert_new(pop, lhead, 0, NULL, OID_NULL, 0, size,
				constructor_alloc_bytype, &carg, oidp, riv);
}



#else
	
/*
 * obj_alloc_construct -- (internal) allocates a new object with constructor
 */
static int
obj_alloc_construct(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
	unsigned int type_num, void (*constructor)(PMEMobjpool *pop, void *ptr,
	void *arg), void *arg)
{
	//printf("type_num val %d\n", type_num);
	//DELAY_8();
	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		printf("Error type_num has to be in range [0,%1]\n");
		ERR("!obj_alloc_construct");
		LOG(2, "type_num has to be in range [0, %i]",
			PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		printf("error: requested size is too large\n");
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	struct list_head *lhead = &pop->store->bytype[type_num].head;
	struct carg_bytype carg;

	carg.user_type = type_num;
	carg.constructor = constructor;
	carg.arg = arg;

	return list_insert_new(pop, lhead, 0, NULL, OID_NULL, 0, size,
				constructor_alloc_bytype, &carg, oidp);
}

#endif


//int size_allocated = 0;
/*Added by Richa
 This version of pmemobj_alloc accepts one more parameter, named riv, which holds the address
 of the unsigned long long integer in which the RIV value is being stored.
*/
#if RIV_MODE

/*
 * pmemobj_alloc -- allocates a new object
 */
int
pmemobj_alloc(PMEMobjpool *pop, PMEMoid *oidp, unsigned long long *riv, size_t size,
	unsigned int type_num, void (*constructor)(PMEMobjpool *pop, void *ptr,
	void *arg), void *arg)
{
	int outcome = 0;
	LOG(3, "pop %p oidp %p size %zu type_num %u constructor %p arg %p",
		pop, oidp, size, type_num, constructor, arg);

	//printf("oidp %d\n", oidp);	
/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (size == 0) {
		//printf("allocation with size 0\n");
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	outcome = obj_alloc_construct(pop, oidp, riv, size, type_num, constructor, arg);
	
	return outcome;
}




#else
/*
 * pmemobj_alloc -- allocates a new object
 */
int
pmemobj_alloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
	unsigned int type_num, void (*constructor)(PMEMobjpool *pop, void *ptr,
	void *arg), void *arg)
{
	int outcome = 0;
	LOG(3, "pop %p oidp %p size %zu type_num %u constructor %p arg %p",
		pop, oidp, size, type_num, constructor, arg);
	
	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (size == 0) {
		printf("allocation with size 0\n");
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	outcome = obj_alloc_construct(pop, oidp, size, type_num, constructor, arg);
	return outcome;
}

#endif


/* arguments for constructor_zalloc */
struct carg_alloc {
	size_t size;
};

/* arguments for constructor_realloc and constructor_zrealloc */
struct carg_realloc {
	void *ptr;
	size_t old_size;
	size_t new_size;
	unsigned int user_type;
};

/*
 * constructor_zalloc -- (internal) constructor for pmemobj_zalloc
 */
static void
constructor_zalloc(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_alloc *carg = arg;

	pop->memset_persist(pop, ptr, 0, carg->size);
}



/*
 * pmemobj_zalloc -- allocates a new zeroed object
 */
int
pmemobj_zalloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		unsigned int type_num)
{
	// Added by Richa. riv is sent as an argument to RIV version of obj_alloc_construct
	#if RIV_MODE
		unsigned long long riv_val = 0;
		unsigned long long *riv;
		riv = &riv_val;
	#endif
	LOG(3, "pop %p oidp %p size %zu type_num %u",
			pop, oidp, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (size == 0) {
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	struct carg_alloc carg;
	carg.size = size;
	// Added by Richa
	#if RIV_MODE
		return obj_alloc_construct(pop, oidp, riv, size, type_num,
					constructor_zalloc, &carg);
	#else
		return obj_alloc_construct(pop, oidp, size, type_num,
					constructor_zalloc, &carg);
	#endif
}

/*
 * obj_free -- (internal) free an object
 */
static void
obj_free(PMEMobjpool *pop, PMEMoid *oidp)
{
	struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, *oidp);

	ASSERT(pobj->user_type < PMEMOBJ_NUM_OID_TYPES);

	void *lhead = &pop->store->bytype[pobj->user_type].head;
	if (list_remove_free(pop, lhead, 0, NULL, oidp)){
		//fprintf(stderr, "list remove free failed\n");
		LOG(2, "list_remove_free failed");
	}
}

/* Added by Richa. RIV specific obj_realloc_common
 This function may be call other functions, which require an extra argument, i.e. the pointer to RIV value
 hence, the separate version of the function obj_realloc_common was written for RIV mode.
*/
#if RIV_MODE
/*
 * obj_realloc_common -- (internal) common routine for resizing
 *                          existing objects
 */
static int
obj_realloc_common(PMEMobjpool *pop, struct object_store *store,
	PMEMoid *oidp, size_t size, unsigned int type_num,
	void (*constr_alloc)(PMEMobjpool *pop, void *ptr, void *arg),
	void (*constr_realloc)(PMEMobjpool *pop, void *ptr, void *arg))
{
	unsigned long long riv_val = 0;
	unsigned long long *riv;
	riv = &riv_val;
	/* if OID is NULL just allocate memory */
	if (OBJ_OID_IS_NULL(*oidp)) {
		struct carg_alloc carg;
		carg.size = size;

		/* if size is 0 - do nothing */
		if (size == 0)
			return 0;

		return obj_alloc_construct(pop, oidp, riv, size, type_num,
						constr_alloc, &carg);
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	/* if size is 0 just free */
	if (size == 0) {
		obj_free(pop, oidp);
		return 0;
	}

	struct carg_realloc carg;
	carg.ptr = OBJ_OFF_TO_PTR(pop, oidp->off);
	carg.new_size = size;
	carg.old_size = pmemobj_alloc_usable_size(*oidp);
	carg.user_type = type_num;

	struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, *oidp);
	uint16_t user_type_old = pobj->user_type;

	ASSERT(user_type_old < PMEMOBJ_NUM_OID_TYPES);

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!obj_realloc_construct");
		LOG(2, "type_num has to be in range [0, %u]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	struct list_head *lhead_old = &store->bytype[user_type_old].head;
	if (type_num == user_type_old) {
		int ret = list_realloc(pop, lhead_old, 0, NULL, size,
				constr_realloc, &carg, 0, 0, oidp);
		if (ret)
			LOG(2, "list_realloc failed");

		return ret;
	} else {
		struct list_head *lhead_new = &store->bytype[type_num].head;
		uint64_t user_type_offset = OOB_OFFSET_OF(*oidp, user_type);
		int ret = list_realloc_move(pop, lhead_old, lhead_new, 0, NULL,
				size, constr_realloc, &carg, user_type_offset,
				type_num, oidp);
		if (ret)
			LOG(2, "list_realloc_move failed");

		return ret;
	}
}

#else

/*
 * obj_realloc_common -- (internal) common routine for resizing
 *                          existing objects
 */
static int
obj_realloc_common(PMEMobjpool *pop, struct object_store *store,
	PMEMoid *oidp, size_t size, unsigned int type_num,
	void (*constr_alloc)(PMEMobjpool *pop, void *ptr, void *arg),
	void (*constr_realloc)(PMEMobjpool *pop, void *ptr, void *arg))
{

	/* if OID is NULL just allocate memory */
	if (OBJ_OID_IS_NULL(*oidp)) {
		struct carg_alloc carg;
		carg.size = size;

		/* if size is 0 - do nothing */
		if (size == 0)
			return 0;

		return obj_alloc_construct(pop, oidp, size, type_num,
						constr_alloc, &carg);
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	/* if size is 0 just free */
	if (size == 0) {
		obj_free(pop, oidp);
		return 0;
	}

	struct carg_realloc carg;
	carg.ptr = OBJ_OFF_TO_PTR(pop, oidp->off);
	carg.new_size = size;
	carg.old_size = pmemobj_alloc_usable_size(*oidp);
	carg.user_type = type_num;

	struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, *oidp);
	uint16_t user_type_old = pobj->user_type;

	ASSERT(user_type_old < PMEMOBJ_NUM_OID_TYPES);

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!obj_realloc_construct");
		LOG(2, "type_num has to be in range [0, %u]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	struct list_head *lhead_old = &store->bytype[user_type_old].head;
	if (type_num == user_type_old) {
		int ret = list_realloc(pop, lhead_old, 0, NULL, size,
				constr_realloc, &carg, 0, 0, oidp);
		if (ret)
			LOG(2, "list_realloc failed");

		return ret;
	} else {
		struct list_head *lhead_new = &store->bytype[type_num].head;
		uint64_t user_type_offset = OOB_OFFSET_OF(*oidp, user_type);
		int ret = list_realloc_move(pop, lhead_old, lhead_new, 0, NULL,
				size, constr_realloc, &carg, user_type_offset,
				type_num, oidp);
		if (ret)
			LOG(2, "list_realloc_move failed");

		return ret;
	}
}

	
#endif
/*
 * constructor_realloc -- (internal) constructor for pmemobj_realloc
 */
static void
constructor_realloc(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_realloc *carg = arg;
	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);

	if (ptr != carg->ptr) {
		size_t cpy_size = carg->new_size > carg->old_size ?
			carg->old_size : carg->new_size;

		pop->memcpy_persist(pop, ptr, carg->ptr, cpy_size);

		pobj->internal_type = TYPE_ALLOCATED;
		pobj->user_type = carg->user_type;
		pop->persist(pop, pobj, sizeof (*pobj));
	}
}

/*
 * constructor_zrealloc -- (internal) constructor for pmemobj_zrealloc
 */
static void
constructor_zrealloc(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_realloc *carg = arg;
	struct oob_header *pobj = OOB_HEADER_FROM_PTR(ptr);

	if (ptr != carg->ptr) {
		size_t cpy_size = carg->new_size > carg->old_size ?
			carg->old_size : carg->new_size;

		pop->memcpy_persist(pop, ptr, carg->ptr, cpy_size);

		pobj->internal_type = TYPE_ALLOCATED;
		pobj->user_type = carg->user_type;
		pop->persist(pop, pobj, sizeof (*pobj));
	}

	if (carg->new_size > carg->old_size) {
		size_t grow_len = carg->new_size - carg->old_size;
		void *new_data_ptr = (void *)((uintptr_t)ptr + carg->old_size);

		pop->memset_persist(pop, new_data_ptr, 0, grow_len);
	}
}

/*
 * constructor_zrealloc_root -- (internal) constructor for pmemobj_root
 */
static void
constructor_zrealloc_root(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	VALGRIND_ADD_TO_TX(OOB_HEADER_FROM_PTR(ptr),
		((struct carg_realloc *)arg)->new_size + OBJ_OOB_SIZE);
	constructor_zrealloc(pop, ptr, arg);
	VALGRIND_REMOVE_FROM_TX(OOB_HEADER_FROM_PTR(ptr),
		((struct carg_realloc *)arg)->new_size + OBJ_OOB_SIZE);
}

/*
 * pmemobj_realloc -- resizes an existing object
 */
int
pmemobj_realloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		unsigned int type_num)
{
	LOG(3, "pop %p oid.off 0x%016jx size %zu type_num %u",
		pop, oidp->off, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));

	return obj_realloc_common(pop, pop->store, oidp, size, type_num,
			NULL, constructor_realloc);
}

/*
 * pmemobj_zrealloc -- resizes an existing object, any new space is zeroed.
 */
int
pmemobj_zrealloc(PMEMobjpool *pop, PMEMoid *oidp, size_t size,
		unsigned int type_num)
{
	LOG(3, "pop %p oid.off 0x%016jx size %zu type_num %u",
		pop, oidp->off, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));

	return obj_realloc_common(pop, pop->store, oidp, size, type_num,
			constructor_zalloc, constructor_zrealloc);
}

/* arguments for constructor_strdup */
struct carg_strdup {
	size_t size;
	const char *s;
};

/*
 * constructor_strdup -- (internal) constructor of pmemobj_strndup
 */
static void
constructor_strdup(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct carg_strdup *carg = arg;

	/* copy string */
	pop->memcpy_persist(pop, ptr, carg->s, carg->size);
}

/* Added by Richa
This function may be call other functions, which require an extra argument, i.e. the pointer to RIV value
 hence, the separate version of the function pmemobj_strdup was written for RIV mode.
*/
#if RIV_MODE

/*
 * pmemobj_strndup -- allocates a new object with duplicate of the string s.
 */
int
pmemobj_strdup(PMEMobjpool *pop, PMEMoid *oidp, const char *s,
		unsigned int type_num)
{
	LOG(3, "pop %p oidp %p string %s type_num %u", pop, oidp, s, type_num);
	unsigned long long riv_val = 0;
	unsigned long long *riv;
	riv = &riv_val;
	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!pmemobj_strdup");
		LOG(2, "type_num has to be in range [0, %i]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	if (NULL == s) {
		errno = EINVAL;
		return -1;
	}

	struct carg_strdup carg;
	carg.size = (strlen(s) + 1) * sizeof (char);
	carg.s = s;

	return obj_alloc_construct(pop, oidp, riv, carg.size, type_num,
					constructor_strdup, &carg);
}

#else
	
/*
 * pmemobj_strndup -- allocates a new object with duplicate of the string s.
 */
int
pmemobj_strdup(PMEMobjpool *pop, PMEMoid *oidp, const char *s,
		unsigned int type_num)
{
	LOG(3, "pop %p oidp %p string %s type_num %u", pop, oidp, s, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!pmemobj_strdup");
		LOG(2, "type_num has to be in range [0, %i]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return -1;
	}

	if (NULL == s) {
		errno = EINVAL;
		return -1;
	}

	struct carg_strdup carg;
	carg.size = (strlen(s) + 1) * sizeof (char);
	carg.s = s;

	return obj_alloc_construct(pop, oidp, carg.size, type_num,
					constructor_strdup, &carg);
}

#endif

/*
 * pmemobj_free -- frees an existing object
 */
void
pmemobj_free(PMEMoid *oidp)
{
	LOG(3, "oid.off 0x%016jx", oidp->off);
	
	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	if (oidp->off == 0)
		return;
	// Added by Richa, to fetch the base address of the pool
	#if RIV_MODE
		PMEMobjpool *pop = (getBase(oidp->pool_uuid_lo)|(1ULL<<46));
	#else
		PMEMobjpool *pop = cuckoo_get(pools, oidp->pool_uuid_lo);
	#endif
	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, *oidp));
	
	obj_free(pop, oidp);
}

/*
 * pmemobj_alloc_usable_size -- returns usable size of object
 */
size_t
pmemobj_alloc_usable_size(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	if (oid.off == 0)
		return 0;
	// Added by Richa, to fetch the base address of the pool
	#if RIV_MODE
		PMEMobjpool *pop = (getBase(oid.pool_uuid_lo)|(1ULL<<46));
	#else
		PMEMobjpool *pop = cuckoo_get(pools, oid.pool_uuid_lo);
	#endif
	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	return (pmalloc_usable_size(pop, oid.off - OBJ_OOB_SIZE) -
			OBJ_OOB_SIZE);
}

/*
 * pmemobj_memcpy_persist -- pmemobj version of memcpy
 */
void *
pmemobj_memcpy_persist(PMEMobjpool *pop, void *dest, const void *src,
	size_t len)
{
	LOG(15, "pop %p dest %p src %p len %zu", pop, dest, src, len);

	return pop->memcpy_persist(pop, dest, src, len);
}

/*
 * pmemobj_memset_persist -- pmemobj version of memset
 */
void *
pmemobj_memset_persist(PMEMobjpool *pop, void *dest, int c, size_t len)
{
	LOG(15, "pop %p dest %p c '%c' len %zu", pop, dest, c, len);

	return pop->memset_persist(pop, dest, c, len);
}

/*
 * pmemobj_persist -- pmemobj version of pmem_persist
 */
void
pmemobj_persist(PMEMobjpool *pop, void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);

	pop->persist(pop, addr, len);
}

/*
 * pmemobj_flush -- pmemobj version of pmem_flush
 */
void
pmemobj_flush(PMEMobjpool *pop, void *addr, size_t len)
{
	LOG(15, "pop %p addr %p len %zu", pop, addr, len);

	pop->flush(pop, addr, len);
}

/*
 * pmemobj_drain -- pmemobj version of pmem_drain
 */
void
pmemobj_drain(PMEMobjpool *pop)
{
	LOG(15, "pop %p", pop);

	pop->drain(pop);
}

/*
 * pmemobj_type_num -- returns type number of object
 */
int
pmemobj_type_num(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	if (OBJ_OID_IS_NULL(oid))
		return -1;

	void *ptr = pmemobj_direct(oid);

	struct oob_header *oobh = OOB_HEADER_FROM_PTR(ptr);
	return oobh->user_type;
}

/* arguments for constructor_alloc_root */
struct carg_root {
	size_t size;
};

/*
 * constructor_alloc_root -- (internal) constructor for obj_alloc_root
 */
static void
constructor_alloc_root(PMEMobjpool *pop, void *ptr, void *arg)
{
	LOG(3, "pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct oob_header *ro = OOB_HEADER_FROM_PTR(ptr);
	struct carg_root *carg = arg;

	/* temporarily add atomic root allocation to pmemcheck transaction */
	VALGRIND_ADD_TO_TX(ro, OBJ_OOB_SIZE + carg->size);

	pop->memset_persist(pop, ptr, 0, carg->size);

	ro->internal_type = TYPE_ALLOCATED;
	ro->user_type = POBJ_ROOT_TYPE_NUM;
	ro->size = carg->size;

	VALGRIND_REMOVE_FROM_TX(ro, OBJ_OOB_SIZE + carg->size);

	pop->persist(pop, ro, OBJ_OOB_SIZE);
}

/*
Added by Richa. An extra parameter, named riv, which is pointing to the RIV value, is expected in RIV version of the function obj_alloc_root.
*/

#if RIV_MODE

/*
 * obj_alloc_root -- (internal) allocate root object
 */
static int
obj_alloc_root(PMEMobjpool *pop, struct object_store *store, size_t size, unsigned long long *riv)
{
	LOG(3, "pop %p store %p size %zu", pop, store, size);

	struct list_head *lhead = &store->root.head;
	struct carg_root carg;

	carg.size = size;

	return list_insert_new(pop, lhead, 0, NULL, OID_NULL, 0,
				size, constructor_alloc_root, &carg, NULL, riv);
}

#else
	
/*
 * obj_alloc_root -- (internal) allocate root object
 */
static int
obj_alloc_root(PMEMobjpool *pop, struct object_store *store, size_t size)
{
	LOG(3, "pop %p store %p size %zu", pop, store, size);

	struct list_head *lhead = &store->root.head;
	struct carg_root carg;

	carg.size = size;

	return list_insert_new(pop, lhead, 0, NULL, OID_NULL, 0,
				size, constructor_alloc_root, &carg, NULL);
}


#endif

/*
 * obj_realloc_root -- (internal) reallocate root object
 */
static int
obj_realloc_root(PMEMobjpool *pop, struct object_store *store, size_t size,
	size_t old_size)
{
	LOG(3, "pop %p store %p size %zu old_size %zu",
		pop, store, size, old_size);

	struct list_head *lhead = &store->root.head;
	uint64_t size_offset = OOB_OFFSET_OF(lhead->pe_first, size);
	struct carg_realloc carg;

	carg.ptr = OBJ_OFF_TO_PTR(pop, lhead->pe_first.off);
	carg.old_size = old_size;
	carg.new_size = size;
	carg.user_type = POBJ_ROOT_TYPE_NUM;

	return list_realloc(pop, lhead, 0, NULL, size,
				constructor_zrealloc_root, &carg,
				size_offset, size, &lhead->pe_first);
}

/*
 * pmemobj_root_size -- returns size of the root object
 */
size_t
pmemobj_root_size(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	if (pop->store->root.head.pe_first.off) {
		struct oob_header *ro = OOB_HEADER_FROM_OID(pop,
						pop->store->root.head.pe_first);
		return ro->size;
	} else
		return 0;
}

/*
Added by Richa.
returns the absolute address of the root object of the pool. 
*/
#if RIV_MODE
/*
 * pmemobj_root -- returns root object
 */
unsigned long long 
pmemobj_root(PMEMobjpool *pop, size_t size)
{
	unsigned long long riv;
	
	LOG(3, "pop %p size %zu", pop, size);
	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return 0;
	}

	PMEMoid root;

	pmemobj_mutex_lock(pop, &pop->rootlock);
	if (pop->store->root.head.pe_first.off == 0)
		/* root object list is empty */
		obj_alloc_root(pop, pop->store, size, riv);
	else {
		size_t old_size = pmemobj_root_size(pop);
		if (size > old_size)
			if (obj_realloc_root(pop, pop->store, size, old_size)) {
				pmemobj_mutex_unlock(pop, &pop->rootlock);
				LOG(2, "obj_realloc_root failed");
				return 0;
			}
	}
	root = pop->store->root.head.pe_first;
	//printf("root pool id is %llx\n", root.pool_uuid_lo);
	//printf("riv value is %d\n", *riv);
	//WRIV_RID(*riv, root.pool_uuid_lo);
	//WRIV_OFFSET(*riv, root.off);
	//*riv = *riv|0x2000000000000000ULL;
	
	pmemobj_mutex_unlock(pop, &pop->rootlock);
	riv = pmemobj_direct(root);
	return riv;
}


#else
	
/*
 * pmemobj_root -- returns root object
 */
PMEMoid
pmemobj_root(PMEMobjpool *pop, size_t size)
{
	LOG(3, "pop %p size %zu", pop, size);
	//printf("pop id %u\n", pop->uuid_lo);
	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return OID_NULL;
	}

	PMEMoid root;

	pmemobj_mutex_lock(pop, &pop->rootlock);
	if (pop->store->root.head.pe_first.off == 0)
		/* root object list is empty */
		obj_alloc_root(pop, pop->store, size);
	else {
		size_t old_size = pmemobj_root_size(pop);
		if (size > old_size)
			if (obj_realloc_root(pop, pop->store, size, old_size)) {
				pmemobj_mutex_unlock(pop, &pop->rootlock);
				LOG(2, "obj_realloc_root failed");
				return OID_NULL;
			}
	}
	root = pop->store->root.head.pe_first;
	pmemobj_mutex_unlock(pop, &pop->rootlock);
	return root;
}
	
#endif


/*
 * pmemobj_first - returns first object of specified type
 */
PMEMoid
pmemobj_first(PMEMobjpool *pop, unsigned int type_num)
{
	LOG(3, "pop %p type_num %u", pop, type_num);

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!pmemobj_first");
		LOG(2, "type_num has to be in range [0, %i]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return OID_NULL;
	}

	return pop->store->bytype[type_num].head.pe_first;
}

/*
 * pmemobj_next - returns next object of specified type
 */
PMEMoid
pmemobj_next(PMEMoid oid)
{
	LOG(3, "oid.off 0x%016jx", oid.off);

	if (oid.off == 0)
		return OID_NULL;
	// Added by Richa, to get the base address of the pool.
	#if RIV_MODE
		PMEMobjpool *pop = (getBase(oid.pool_uuid_lo)|(1ULL<<46));
	#else
		PMEMobjpool *pop = cuckoo_get(pools, oid.pool_uuid_lo);
	#endif
	
	ASSERTne(pop, NULL);
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, oid);
	uint16_t user_type = pobj->user_type;

	ASSERT(user_type < PMEMOBJ_NUM_OID_TYPES);

	if (pobj->oob.pe_next.off !=
			pop->store->bytype[user_type].head.pe_first.off)
		return pobj->oob.pe_next;
	else
		return OID_NULL;
}


/*
 * pmemobj_list_insert -- adds object to a list
 */
int
pmemobj_list_insert(PMEMobjpool *pop, size_t pe_offset, void *head,
		    PMEMoid dest, int before, PMEMoid oid)
{
	LOG(3, "pop %p pe_offset %zu head %p dest.off 0x%016jx before %d"
	    " oid.off 0x%016jx",
	    pop, pe_offset, head, dest.off, before, oid.off);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, oid));
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	return list_insert(pop, pe_offset, head, dest, before, oid);
}


/*
Added by Richa. RIV version of pmemobj_list_insert_new, accepts one more parameter, named riv, which points to the RIV value.
*/
#if RIV_MODE

/*
 * pmemobj_list_insert_new -- adds new object to a list
 */
PMEMoid
pmemobj_list_insert_new(PMEMobjpool *pop, size_t pe_offset, void *head,
			PMEMoid dest, int before, size_t size,
			unsigned int type_num,
			void (*constructor)(PMEMobjpool *pop, void *ptr,
			void *arg), void *arg, unsigned long long *riv)
{
	LOG(3, "pop %p pe_offset %zu head %p dest.off 0x%016jx before %d"
	    " size %zu type_num %u",
	    pop, pe_offset, head, dest.off, before, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!pmemobj_list_insert_new");
		LOG(2, "type_num has to be in range [0, %i]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return OID_NULL;
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return OID_NULL;
	}

	struct list_head *lhead = &pop->store->bytype[type_num].head;
	struct carg_bytype carg;

	carg.user_type = type_num;
	carg.constructor = constructor;
	carg.arg = arg;

	PMEMoid retoid = OID_NULL;
	list_insert_new(pop, lhead,
			pe_offset, head, dest, before,
			size, constructor_alloc_bytype, &carg, &retoid, riv);
	return retoid;
}

#else
	
/*
 * pmemobj_list_insert_new -- adds new object to a list
 */
PMEMoid
pmemobj_list_insert_new(PMEMobjpool *pop, size_t pe_offset, void *head,
			PMEMoid dest, int before, size_t size,
			unsigned int type_num,
			void (*constructor)(PMEMobjpool *pop, void *ptr,
			void *arg), void *arg)
{
	LOG(3, "pop %p pe_offset %zu head %p dest.off 0x%016jx before %d"
	    " size %zu type_num %u",
	    pop, pe_offset, head, dest.off, before, size, type_num);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	if (type_num >= PMEMOBJ_NUM_OID_TYPES) {
		errno = EINVAL;
		ERR("!pmemobj_list_insert_new");
		LOG(2, "type_num has to be in range [0, %i]",
		    PMEMOBJ_NUM_OID_TYPES - 1);
		return OID_NULL;
	}

	if (size > PMEMOBJ_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return OID_NULL;
	}

	struct list_head *lhead = &pop->store->bytype[type_num].head;
	struct carg_bytype carg;

	carg.user_type = type_num;
	carg.constructor = constructor;
	carg.arg = arg;

	PMEMoid retoid = OID_NULL;
	list_insert_new(pop, lhead,
			pe_offset, head, dest, before,
			size, constructor_alloc_bytype, &carg, &retoid);
	return retoid;
}



#endif

/*
 * pmemobj_list_remove -- removes object from a list
 */
int
pmemobj_list_remove(PMEMobjpool *pop, size_t pe_offset, void *head,
		    PMEMoid oid, int free)
{
	LOG(3, "pop %p pe_offset %zu head %p oid.off 0x%016jx free %d",
	    pop, pe_offset, head, oid.off, free);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();
	ASSERT(OBJ_OID_IS_VALID(pop, oid));

	if (free) {
		struct oob_header *pobj = OOB_HEADER_FROM_OID(pop, oid);

		ASSERT(pobj->user_type < PMEMOBJ_NUM_OID_TYPES);

		void *lhead = &pop->store->bytype[pobj->user_type].head;
		return list_remove_free(pop, lhead, pe_offset, head, &oid);
	} else
		return list_remove(pop, pe_offset, head, oid);
}

/*
 * pmemobj_list_move -- moves object between lists
 */
int
pmemobj_list_move(PMEMobjpool *pop, size_t pe_old_offset, void *head_old,
			size_t pe_new_offset, void *head_new,
			PMEMoid dest, int before, PMEMoid oid)
{
	LOG(3, "pop %p pe_old_offset %zu pe_new_offset %zu"
	    " head_old %p head_new %p dest.off 0x%016jx"
	    " before %d oid.off 0x%016jx",
	    pop, pe_old_offset, pe_new_offset,
	    head_old, head_new, dest.off, before, oid.off);

	/* log notice message if used inside a transaction */
	_POBJ_DEBUG_NOTICE_IN_TX();

	ASSERT(OBJ_OID_IS_VALID(pop, oid));
	ASSERT(OBJ_OID_IS_VALID(pop, dest));

	return list_move(pop, pe_old_offset, head_old,
				pe_new_offset, head_new,
				dest, before, oid);
}

/*
 * _pobj_debug_notice -- logs notice message if used inside a transaction
 */
void
_pobj_debug_notice(const char *api_name, const char *file, int line)
{
#ifdef	DEBUG
	if (pmemobj_tx_stage() != TX_STAGE_NONE) {
		if (file)
			LOG(4, "Notice: non-transactional API"
				" used inside a transaction (%s in %s:%d)",
				api_name, file, line);
		else
			LOG(4, "Notice: non-transactional API"
				" used inside a transaction (%s)", api_name);
	}
#endif /* DEBUG */
}


