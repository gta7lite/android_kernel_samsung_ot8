/*************************************************************************/ /*!
@File
@Title          Physmem (PMR) abstraction
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Part of the memory management.  This module is responsible for
                the "PMR" abstraction.  A PMR (Physical Memory Resource)
                represents some unit of physical memory which is
                allocated/freed/mapped/unmapped as an indivisible unit
                (higher software levels provide an abstraction above that
                to deal with dividing this down into smaller manageable units).
                Importantly, this module knows nothing of virtual memory, or
                of MMUs etc., with one excusable exception.  We have the
                concept of a "page size", which really means nothing in
                physical memory, but represents a "contiguity quantum" such
                that the higher level modules which map this memory are able
                to verify that it matches the needs of the page size for the
                virtual realm into which it is being mapped.
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */ /**************************************************************************/

#include "img_types.h"
#include "img_defs.h"
#include "pvr_debug.h"
#include "pvrsrv_error.h"

#include "pdump.h"
#include "devicemem_server_utils.h"

#include "osfunc.h"
#include "pdump_km.h"
#include "pdump_physmem.h"
#include "pmr_impl.h"
#include "pmr_os.h"
#include "pvrsrv.h"

#include "allocmem.h"
#include "lock.h"

#if defined(SUPPORT_SECURE_EXPORT)
#include "secure_export.h"
#include "ossecure_export.h"
#endif

#if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO)
#include "ri_server.h"
#endif

/* ourselves */
#include "pmr.h"

#if defined(PVRSRV_ENABLE_LINUX_MMAP_STATS)
#include "mmap_stats.h"
#endif

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#include "process_stats.h"
#include "proc_stats.h"
#endif
#define PMR_FLAG_INTERNAL_SPARSE_ALLOC     (1 << 0)
#define PMR_FLAG_INTERNAL_NO_LAYOUT_CHANGE (1 << 1)
#if defined(SUPPORT_PMR_DEFERRED_FREE)
#define PMR_FLAG_INTERNAL_DEFER_FREE       (1 << 2)
#define PMR_FLAG_INTERNAL_IS_ZOMBIE        (1 << 3)

/* Indicates PMR should be destroyed immediately and not deferred. */
#define PMR_NO_ZOMBIE_FENCE IMG_UINT64_MAX
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */


/* A "context" for the physical memory block resource allocator.
 *
 * Context is probably the wrong word.
 *
 * There is almost certainly only one of these, ever, in the system.
 * But, let's keep the notion of a context anyway, "just-in-case".
 */
static struct _PMR_CTX_
{
	/* For debugging, and PDump, etc., let's issue a forever incrementing
	 * serial number to each allocation.
	 */
	IMG_UINT64 uiNextSerialNum;

	/* For security, we only allow a PMR to be mapped if the caller knows
	 * its key. We can pseudo-randomly generate keys
	 */
	IMG_UINT64 uiNextKey;

	/* For debugging only, I guess: Number of live PMRs */
	ATOMIC_T uiNumLivePMRs;

	/* Lock for this structure */
	POS_LOCK hLock;

	/* In order to seed the uiNextKey, we enforce initialisation at driver
	 * load time. Also, we can debug check at driver unload that the PMR
	 * count is zero.
	 */
	IMG_BOOL bModuleInitialised;
} _gsSingletonPMRContext = { 1, 0, {0}, NULL, IMG_FALSE };

typedef struct _PMR_HEADER_
{
#if defined(SUPPORT_PMR_DEFERRED_FREE)
	/* List node used to put the header on the zombie list
	 * (psDevNode->sPMRZombieList). */
	DLLIST_NODE sZombieNode;
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

	IMG_BOOL bIsZombiePages;
} PMR_HEADER;

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
/*
 * A structure describing zombie pages.
 */
typedef struct _PMR_ZOMBIE_PAGES_
{
	PMR_HEADER sHeader;
	PMR_IMPL_ZOMBIEPAGES pvFactoryPages;
	PFN_FREE_ZOMBIE_PAGES_FN pfnFactoryFreeZombies;
} PMR_ZOMBIE_PAGES;
#endif

/* A PMR. One per physical allocation. May be "shared".
 *
 * "shared" is ambiguous. We need to be careful with terminology.
 * There are two ways in which a PMR may be "shared" and we need to be sure
 * that we are clear which we mean.
 *
 * i)   multiple small allocations living together inside one PMR.
 *
 * ii)  one single allocation filling a PMR but mapped into multiple memory
 *      contexts.
 *
 * This is more important further up the stack - at this level, all we care is
 * that the PMR is being referenced multiple times.
 */
struct _PMR_
{
	/* A Common header structure shared between PMR and PMR-like PMR_ZOMBIE_PAGES object */
	PMR_HEADER sHeader;

	/* This object is strictly refcounted. References include:
	 * - mapping
	 * - live handles (to this object)
	 * - live export handles
	 * (thus it is normal for allocated and exported memory to have a refcount of 3)
	 * The object is destroyed when and only when the refcount reaches 0
	 */

	/* Device node on which this PMR was created and is valid */
	PVRSRV_DEVICE_NODE *psDevNode;

	/* Physical address translation (device <> cpu) is done on a per device
	 * basis which means we need the physical heap info
	 */
	PHYS_HEAP *psPhysHeap;

	ATOMIC_T iRefCount;

	/* CPU mapping count - this is the number of times the PMR has been
	 * mapped to the CPU. It is used to determine when it is safe to permit
	 * modification of a sparse allocation's layout.
	 * Note that the process of mapping also increments iRefCount
	 * independently (as that is used to determine when a PMR may safely
	 * be destroyed).
	 */
	ATOMIC_T iCpuMapCount;

	/* Count of how many reservations refer to this
	 * PMR as a part of a GPU mapping. Must be protected
	 * by PMR lock.
	 */
	IMG_INT32 iAssociatedResCount;

	/* Lock count - this is the number of times PMRLockSysPhysAddresses()
	 * has been called, less the number of PMRUnlockSysPhysAddresses()
	 * calls. This is arguably here for debug reasons only, as the refcount
	 * is already incremented as a matter of course.
	 * Really, this just allows us to trap protocol errors: i.e. calling
	 * PMRSysPhysAddr(), without a lock, or calling
	 * PMRUnlockSysPhysAddresses() too many or too few times.
	 */
	ATOMIC_T iLockCount;

	/* Lock for this structure */
	POS_LOCK hLock;
	/* Internal flags lock */
	POS_SPINLOCK hInternalFlagsLock;

	/* Incrementing serial number to each allocation. */
	IMG_UINT64 uiSerialNum;

	/* For security, we only allow a PMR to be mapped if the caller knows
	 * its key. We can pseudo-randomly generate keys
	 */
	PMR_PASSWORD_T uiKey;

	/* Callbacks for per-flavour functions */
	const PMR_IMPL_FUNCTAB *psFuncTab;

	/* Data associated with the "subtype" */
	PMR_IMPL_PRIVDATA pvFlavourData;

	/* What kind of PMR do we have? */
	PMR_IMPL_TYPE eFlavour;

	/* And for pdump */
	const IMG_CHAR *pszPDumpDefaultMemspaceName;

	/* Allocation annotation */
	IMG_CHAR szAnnotation[DEVMEM_ANNOTATION_MAX_LEN];

#if defined(PDUMP)

	IMG_HANDLE hPDumpAllocHandle;

	IMG_UINT32 uiNumPDumpBlocks;
#endif

	/* Logical size of allocation. "logical", because a PMR can represent
	 * memory that will never physically exist.  This is the amount of
	 * virtual space that the PMR would consume when it's mapped into a
	 * virtual allocation.
	 */
	PMR_SIZE_T uiLogicalSize;

	/* Mapping table for the allocation.
	 * PMR's can be sparse in which case not all the "logic" addresses in
	 * it are valid. We need to know which addresses are and aren't valid
	 * when mapping or reading the PMR.
	 * The mapping table translates "logical" offsets into physical offsets
	 * which is what we always pass to the PMR factory (so it doesn't have
	 * to be concerned about sparseness issues)
	 */
	PMR_MAPPING_TABLE *psMappingTable;

	/* Indicates whether this PMR has been unpinned.
	 * By default, all PMRs are pinned at creation.
	 */
	IMG_BOOL bIsUnpinned;

	/* Minimum Physical Contiguity Guarantee.  Might be called "page size",
	 * but that would be incorrect, as page size is something meaningful
	 * only in virtual realm. This contiguity guarantee provides an
	 * inequality that can be verified/asserted/whatever to ensure that
	 * this PMR conforms to the page size requirement of the place the PMR
	 * gets mapped. (May be used to select an appropriate heap in variable
	 * page size systems)
	 *
	 * The absolutely necessary condition is this:
	 *
	 *    device MMU page size <= actual physical contiguity.
	 *
	 * We go one step further in order to be able to provide an early
	 * warning / early compatibility check and say this:
	 *
	 *     device MMU page size <=
	 *         2**(uiLog2ContiguityGuarantee) <=
	 *             actual physical contiguity.
	 *
	 * In this way, it is possible to make the page table reservation
	 * in the device MMU without even knowing the granularity of the
	 * physical memory (i.e. useful for being able to allocate virtual
	 * before physical)
	 */
	PMR_LOG2ALIGN_T uiLog2ContiguityGuarantee;

	/* Flags. We store a copy of the "PMR flags" (usually a subset of the
	 * flags given at allocation time) and return them to any caller of
	 * PMR_Flags(). The intention of these flags is that the ones stored
	 * here are used to represent permissions, such that no one is able
	 * to map a PMR in a mode in which they are not allowed, e.g.,
	 * writeable for a read-only PMR, etc.
	 */
	PMR_FLAGS_T uiFlags;

	/* Various flags informing about PMR's state:
	 *
	 * SPARSE_ALLOC:
	 *   indicates whether this PMR has been allocated as sparse.
	 *   The condition for this variable to be set at allocation time is:
	 *   (numVirtChunks != numPhysChunks) || (numVirtChunks > 1)
	 *
	 * NO_LAYOUT_CHANGE:
	 *   Flag that conveys mutability of the PMR:
	 *   - set:   indicates the PMR is immutable (no more memory changes)
	 *   - unset: means the memory layout associated with the PMR is mutable
	 *
	 *   A PMR is always mutable by default but is marked immutable on the
	 *   first export for the rest of its life.
	 *
	 *   Also, any PMRs that track the same memory through imports are
	 *   marked immutable as well.
	 *
	 * DEFER_FREE:
	 *   If present the PMR is marked to be freed by the CleanupThread.
	 *
	 * IS_ZOMBIE:
	 *   Indicates if the PMR is in the zombie state (marked for free in the
	 *   CleanupThread). */
	IMG_UINT32 uiInternalFlags;

	/* Do we really need this?
	 * For now we'll keep it, until we know we don't.
	 * NB: this is not the "memory context" in client terms - this is
	 * _purely_ the "PMR" context, of which there is almost certainly only
	 * ever one per system as a whole, but we'll keep the concept anyway,
	 * just-in-case.
	 */
	struct _PMR_CTX_ *psContext;

#if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO)
	/* Stored handle to PMR RI entry */
	void		*hRIHandle;
#endif
};

/* Do we need a struct for the export handle?
 * I'll use one for now, but if nothing goes in it, we'll lose it
 */
struct _PMR_EXPORT_
{
	struct _PMR_ *psPMR;
};

struct _PMR_PAGELIST_
{
	struct _PMR_ *psReferencePMR;
};

void
PMRLockPMR(PMR *psPMR)
{
	OSLockAcquire(psPMR->hLock);
}

void
PMRUnlockPMR(PMR *psPMR)
{
	OSLockRelease(psPMR->hLock);
}

IMG_INT32 PMRGetLiveCount(void)
{
	return OSAtomicRead(&_gsSingletonPMRContext.uiNumLivePMRs);
}

PPVRSRV_DEVICE_NODE PMRGetExportDeviceNode(PMR_EXPORT *psExportPMR)
{
	PPVRSRV_DEVICE_NODE psReturnedDeviceNode = NULL;

	PVR_ASSERT(psExportPMR != NULL);
	if (psExportPMR)
	{
		PVR_ASSERT(psExportPMR->psPMR != NULL);
		if (psExportPMR->psPMR)
		{
			PVR_ASSERT(OSAtomicRead(&psExportPMR->psPMR->iRefCount) > 0);
			if (OSAtomicRead(&psExportPMR->psPMR->iRefCount) > 0)
			{
				psReturnedDeviceNode = PMR_DeviceNode(psExportPMR->psPMR);
			}
		}
	}

	return psReturnedDeviceNode;
}

static PVRSRV_ERROR
_PMRCreate(PMR_SIZE_T uiLogicalSize,
           PMR_SIZE_T uiChunkSize,
           IMG_UINT32 ui32NumPhysChunks,
           IMG_UINT32 ui32NumVirtChunks,
           IMG_UINT32 *pui32MappingTable,
           PMR_LOG2ALIGN_T uiLog2ContiguityGuarantee,
           PMR_FLAGS_T uiFlags,
           PMR **ppsPMR)
{
	void *pvPMRLinAddr;
	PMR *psPMR;
	PMR_MAPPING_TABLE *psMappingTable;
	struct _PMR_CTX_ *psContext;
	IMG_UINT32 i, ui32Temp = 0;
	IMG_UINT32 ui32Remainder;
	PVRSRV_ERROR eError;
	IMG_BOOL bSparse = IMG_FALSE;

	psContext = &_gsSingletonPMRContext;

	/* Do we have a sparse allocation? */
	if ( (ui32NumVirtChunks != ui32NumPhysChunks) ||
			(ui32NumVirtChunks > 1) )
	{
		bSparse = IMG_TRUE;
	}

	/* Extra checks required for sparse PMRs */
	if (uiLogicalSize != uiChunkSize)
	{
		/* Check the logical size and chunk information agree with each other */
		if (uiLogicalSize != (uiChunkSize * ui32NumVirtChunks))
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Bad mapping size (uiLogicalSize = 0x%llx, uiChunkSize = 0x%llx, ui32NumVirtChunks = %d)",
					__func__, (unsigned long long)uiLogicalSize, (unsigned long long)uiChunkSize, ui32NumVirtChunks));
			return PVRSRV_ERROR_PMR_BAD_MAPPINGTABLE_SIZE;
		}

		/* Check that the chunk size is a multiple of the contiguity */
		OSDivide64(uiChunkSize, (1<< uiLog2ContiguityGuarantee), &ui32Remainder);
		if (ui32Remainder)
		{
			PVR_DPF((PVR_DBG_ERROR,
					"%s: Bad chunk size, must be a multiple of the contiguity "
					"(uiChunkSize = 0x%llx, uiLog2ContiguityGuarantee = %u)",
					__func__,
					(unsigned long long) uiChunkSize,
					uiLog2ContiguityGuarantee));
			return PVRSRV_ERROR_PMR_BAD_CHUNK_SIZE;
		}
	}

	pvPMRLinAddr = OSAllocMem(sizeof(*psPMR) + sizeof(*psMappingTable) + sizeof(IMG_UINT32) * ui32NumVirtChunks);
	PVR_RETURN_IF_NOMEM(pvPMRLinAddr);

	psPMR = (PMR *) pvPMRLinAddr;
	psMappingTable = IMG_OFFSET_ADDR(pvPMRLinAddr, sizeof(*psPMR));

	/* Setup the mapping table */
	psMappingTable->uiChunkSize = uiChunkSize;
	psMappingTable->ui32NumVirtChunks = ui32NumVirtChunks;
	psMappingTable->ui32NumPhysChunks = ui32NumPhysChunks;
	OSCachedMemSet(&psMappingTable->aui32Translation[0], 0xFF, sizeof(psMappingTable->aui32Translation[0])*
	               ui32NumVirtChunks);
	for (i=0; i<ui32NumPhysChunks; i++)
	{
		ui32Temp = pui32MappingTable[i];
		if (ui32Temp < ui32NumVirtChunks)
		{
			psMappingTable->aui32Translation[ui32Temp] = ui32Temp;
		}
		else
		{
			OSFreeMem(psPMR);
			return PVRSRV_ERROR_PMR_INVALID_MAP_INDEX_ARRAY;
		}
	}

	eError = OSLockCreate(&psPMR->hLock);
	if (eError != PVRSRV_OK)
	{
		OSFreeMem(psPMR);
		return eError;
	}

	eError = OSSpinLockCreate(&psPMR->hInternalFlagsLock);
	if (eError != PVRSRV_OK)
	{
		OSLockDestroy(psPMR->hLock);
		OSFreeMem(psPMR);
		return eError;
	}

	/* Setup the PMR */
	OSAtomicWrite(&psPMR->iRefCount, 0);
	OSAtomicWrite(&psPMR->iCpuMapCount, 0);
	psPMR->iAssociatedResCount = 0;

	/* If allocation is not made on demand, it will be backed now and
	 * backing will not be removed until the PMR is destroyed, therefore
	 * we can initialise the iLockCount to 1 rather than 0.
	 */
	OSAtomicWrite(&psPMR->iLockCount, (PVRSRV_CHECK_ON_DEMAND(uiFlags) ? 0 : 1));

	psPMR->psContext = psContext;
	psPMR->uiLogicalSize = uiLogicalSize;
	psPMR->uiLog2ContiguityGuarantee = uiLog2ContiguityGuarantee;
	psPMR->uiFlags = uiFlags;
	psPMR->psMappingTable = psMappingTable;
	psPMR->uiInternalFlags = bSparse ? PMR_FLAG_INTERNAL_SPARSE_ALLOC : 0;
	psPMR->bIsUnpinned = IMG_FALSE;
	psPMR->szAnnotation[0] = '\0';
	psPMR->sHeader.bIsZombiePages = IMG_FALSE;

#if defined(SUPPORT_PMR_DEFERRED_FREE)
	dllist_init(&psPMR->sHeader.sZombieNode);
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

#if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO)
	psPMR->hRIHandle = NULL;
#endif
	OSLockAcquire(psContext->hLock);
	psPMR->uiKey = psContext->uiNextKey;
	psPMR->uiSerialNum = psContext->uiNextSerialNum;
	psContext->uiNextKey = (0x80200003 * psContext->uiNextKey)
								^ (0xf00f0081 * (uintptr_t)pvPMRLinAddr);
	psContext->uiNextSerialNum++;
	*ppsPMR = psPMR;
	OSLockRelease(psContext->hLock);

	PVR_DPF((PVR_DBG_MESSAGE, "%s: 0x%p, key:0x%016" IMG_UINT64_FMTSPECX ", numLive:%d",
			__func__, psPMR, psPMR->uiKey, OSAtomicRead(&psPMR->psContext->uiNumLivePMRs)));

	/* Increment live PMR count */
	OSAtomicIncrement(&psContext->uiNumLivePMRs);

	return PVRSRV_OK;
}

/* This function returns true if the PMR is in use and false otherwise.
 * This function is not thread safe and hence the caller
 * needs to ensure the thread safety by explicitly taking
 * the lock on the PMR or through other means */
IMG_BOOL PMRIsPMRLive(PMR *psPMR)
{
	return (OSAtomicRead(&psPMR->iRefCount) > 0);
}

static IMG_UINT32
_Ref(PMR *psPMR)
{
	PVR_ASSERT(OSAtomicRead(&psPMR->iRefCount) >= 0);
	return OSAtomicIncrement(&psPMR->iRefCount);
}

static IMG_UINT32
_Unref(PMR *psPMR)
{
	PVR_ASSERT(OSAtomicRead(&psPMR->iRefCount) > 0);
	return OSAtomicDecrement(&psPMR->iRefCount);
}


static INLINE void _IntFlagSet(PMR *psPMR, const IMG_UINT32 uiValue)
{
	OS_SPINLOCK_FLAGS uiLockingFlags;

	OSSpinLockAcquire(psPMR->hInternalFlagsLock, uiLockingFlags);
	BITMASK_SET(psPMR->uiInternalFlags, uiValue);
	OSSpinLockRelease(psPMR->hInternalFlagsLock, uiLockingFlags);
}

static INLINE void _IntFlagClr(PMR *psPMR, const IMG_UINT32 uiValue)
{
	OS_SPINLOCK_FLAGS uiLockingFlags;

	OSSpinLockAcquire(psPMR->hInternalFlagsLock, uiLockingFlags);
	BITMASK_UNSET(psPMR->uiInternalFlags, uiValue);
	OSSpinLockRelease(psPMR->hInternalFlagsLock, uiLockingFlags);
}

static INLINE IMG_BOOL _IntFlagIsSet(const PMR *psPMR, const IMG_UINT32 uiValue)
{
	OS_SPINLOCK_FLAGS uiLockingFlags;
	IMG_BOOL bIsSet;

	OSSpinLockAcquire(psPMR->hInternalFlagsLock, uiLockingFlags);
	bIsSet = BITMASK_HAS(psPMR->uiInternalFlags, uiValue);
	OSSpinLockRelease(psPMR->hInternalFlagsLock, uiLockingFlags);

	return bIsSet;
}

static INLINE void
_FactoryLock(const PMR_IMPL_FUNCTAB *psFuncTable)
{
	if (psFuncTable->pfnGetPMRFactoryLock != NULL)
	{
		psFuncTable->pfnGetPMRFactoryLock();
	}
}

static INLINE void
_FactoryUnlock(const PMR_IMPL_FUNCTAB *psFuncTable)
{
	if (psFuncTable->pfnReleasePMRFactoryLock != NULL)
	{
		psFuncTable->pfnReleasePMRFactoryLock();
	}
}

static void
_PMRDestroy(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	if (psPMR->psFuncTab->pfnFinalize != NULL)
	{
		PVRSRV_ERROR eError = psPMR->psFuncTab->pfnFinalize(psPMR->pvFlavourData);

		/* PMR unref can be called asynchronously by the kernel or other
		 * third party modules (eg. display) which doesn't go through the
		 * usual services bridge. The same PMR can be referenced simultaneously
		 * in a different path that results in a race condition.
		 * Hence depending on the race condition, a factory may refuse to destroy
		 * the resource associated with this PMR if a reference on it was taken
		 * prior to unref. In that case the PMR factory function returns the error.
		 *
		 * When such an error is encountered, the factory needs to ensure the state
		 * associated with PMR is undisturbed. At this point we just bail out from
		 * freeing the PMR itself. The PMR handle will then be freed at a later point
		 * when the same PMR is unreferenced.
		 * */
		if (PVRSRV_ERROR_PMR_STILL_REFERENCED == eError)
		{
			return;
		}
		PVR_ASSERT (eError == PVRSRV_OK); /* can we do better? */
	}
#if defined(PDUMP)
	PDumpPMRFreePMR(psPMR,
	                psPMR->uiLogicalSize,
	                (1 << psPMR->uiLog2ContiguityGuarantee),
	                psPMR->uiLog2ContiguityGuarantee,
	                psPMR->hPDumpAllocHandle);
#endif

#if defined(PVRSRV_ENABLE_LINUX_MMAP_STATS)
	/* This PMR is about to be destroyed, update its mmap stats record (if present)
	 * to avoid dangling pointer. Additionally, this is required because mmap stats
	 * are identified by PMRs and a new PMR down the line "might" get the same address
	 * as the one we're about to free and we'd like 2 different entries in mmaps
	 * stats for such cases */
	MMapStatsRemovePMR(psPMR);
#endif

#ifdef PVRSRV_NEED_PVR_ASSERT
	/* If not backed on demand, iLockCount should be 1 otherwise it should be 0 */
	PVR_ASSERT(OSAtomicRead(&psPMR->iLockCount) == (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags) ? 0 : 1));
#endif

#if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO)
	/* Delete RI entry */
	if (psPMR->hRIHandle)
	{
		PVRSRV_ERROR eError = RIDeletePMREntryKM(psPMR->hRIHandle);
		PVR_LOG_IF_ERROR(eError, "RIDeletePMREntryKM");
		/* continue destroying the PMR */
	}
#endif /* if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO) */

	/* Decrement live PMR count. Probably only of interest for debugging */
	PVR_ASSERT(OSAtomicRead(&psPMR->psContext->uiNumLivePMRs) > 0);
	OSAtomicDecrement(&psPMR->psContext->uiNumLivePMRs);

	PVR_DPF((PVR_DBG_MESSAGE, "%s: 0x%p, key:0x%016" IMG_UINT64_FMTSPECX ", numLive:%d",
			__func__, psPMR, psPMR->uiKey, OSAtomicRead(&psPMR->psContext->uiNumLivePMRs)));

	OSSpinLockDestroy(psPMR->hInternalFlagsLock);
	OSLockDestroy(psPMR->hLock);
	OSFreeMem(psPMR);
}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
static IMG_BOOL
PMR_IsListNodeZombiePages(const DLLIST_NODE *psNode)
{
	PMR_HEADER *psPMRHeader = IMG_CONTAINER_OF(psNode, PMR_HEADER, sZombieNode);
	PVR_ASSERT(psPMRHeader != NULL);
	return psPMRHeader->bIsZombiePages;
}

static PMR_ZOMBIE_PAGES*
PMR_GetZombiePagesFromNode(const DLLIST_NODE *psNode)
{
	PMR_HEADER *psPMRHeader = IMG_CONTAINER_OF(psNode, PMR_HEADER, sZombieNode);
	PVR_ASSERT(psPMRHeader != NULL);
	return IMG_CONTAINER_OF(psPMRHeader, PMR_ZOMBIE_PAGES, sHeader);
}
#endif /* defined(SUPPORT_PMR_PAGES_DEFERRED_FREE) */

static PMR*
PMR_GetPMRFromNode(const DLLIST_NODE *psNode)
{
	PMR_HEADER *psPMRHeader = IMG_CONTAINER_OF(psNode, PMR_HEADER, sZombieNode);
	PVR_ASSERT(psPMRHeader != NULL);
	return IMG_CONTAINER_OF(psPMRHeader, PMR, sHeader);
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

static void
_UnrefAndMaybeDestroy(PMR *psPMR)
{
	struct _PMR_CTX_ *psContext;
	const PMR_IMPL_FUNCTAB *psFuncTable;
	IMG_INT iRefCount;
#if defined(SUPPORT_PMR_DEFERRED_FREE)
	PVRSRV_DEV_POWER_STATE ePowerState;
	PVRSRV_DEVICE_NODE *psDevNode;
	PVRSRV_ERROR eError;
#endif

	PVR_ASSERT(psPMR != NULL);

	psContext = psPMR->psContext;
	psFuncTable = psPMR->psFuncTab;

	_FactoryLock(psFuncTable);

	iRefCount = _Unref(psPMR);

	if (iRefCount > 0)
	{
		_FactoryUnlock(psFuncTable);
		return;
	}

#if !defined(SUPPORT_PMR_DEFERRED_FREE)
	/* Don't defer PMR destruction in NoHW and PDUMP drivers. */
	_PMRDestroy(psPMR);
#else /* !defined(SUPPORT_PMR_DEFERRED_FREE) */
	psDevNode = psPMR->psDevNode;

	eError = PVRSRVGetDevicePowerState(psDevNode, &ePowerState);
	if (eError != PVRSRV_OK)
	{
		/* Treat unknown power state as ON. */
		ePowerState = PVRSRV_DEV_POWER_STATE_ON;
	}

	/* PMRs that are not marked for deferred free can be freed right away.
	 * Those are the PMRs that have not been mapped to the device.
	 * All PMRs that have been mapped to the device need to go through
	 * the defer free path unless the power is OFF. If power is OFF
	 * the cache invalidation comes as a given. */
	if (!_IntFlagIsSet(psPMR, PMR_FLAG_INTERNAL_DEFER_FREE) ||
	    ePowerState == PVRSRV_DEV_POWER_STATE_OFF)
	{
		_PMRDestroy(psPMR);
	}
	else
	{
		/* Defer freeing the PMR until the Firmware invalidates the caches. */
		PVRSRV_DEVICE_NODE *psDevNode = psPMR->psDevNode;
		OSLockAcquire(psDevNode->hPMRZombieListLock);

		_IntFlagSet(psPMR, PMR_FLAG_INTERNAL_IS_ZOMBIE);

		dllist_add_to_tail(&psDevNode->sPMRZombieList, &psPMR->sHeader.sZombieNode);
		psDevNode->uiPMRZombieCount++;

		/* PMR pages are accounted by the driver/process stats. Those stats
		 * are available on page level hence they need to be adjusted by
		 * the factories. This is done by the pfnZombify callback.
		 * Operation needs to be done while holding hPMRZombieListLock
		 * to prevent CleanupThread from freeing pages while memory stats
		 * accounting is ongoing. */
		if (psPMR->psFuncTab->pfnZombify != NULL)
		{
			PVRSRV_ERROR eError = psPMR->psFuncTab->pfnZombify(psPMR->pvFlavourData, psPMR);
			PVR_LOG_IF_ERROR(eError, "pfnZombify");
		}

		OSLockRelease(psDevNode->hPMRZombieListLock);
	}
#endif /* !defined(SUPPORT_PMR_DEFERRED_FREE) */

	_FactoryUnlock(psFuncTable);
}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
typedef struct _PMR_ZOMBIE_CLEANUP_ITEM_
{
	PVRSRV_CLEANUP_THREAD_WORK sCleanupThreadFn;
	DLLIST_NODE sZombieList;
	PPVRSRV_DEVICE_NODE psDevNode;
	PVRSRV_CLIENT_SYNC_PRIM *psSync;
	IMG_UINT32 uiRequiredSyncValue;
	IMG_UINT32 uiRequiredPowerOffCounter;
} PMR_ZOMBIE_CLEANUP_ITEM;

static INLINE void
_ZombieListLock(PMR_ZOMBIE_CLEANUP_ITEM *psCleanupItem)
{
	OSLockAcquire(psCleanupItem->psDevNode->hPMRZombieListLock);
}

static INLINE void
_ZombieListUnlock(PMR_ZOMBIE_CLEANUP_ITEM *psCleanupItem)
{
	OSLockRelease(psCleanupItem->psDevNode->hPMRZombieListLock);
}

static INLINE IMG_BOOL
_CanNotFreeZombies(const PMR_ZOMBIE_CLEANUP_ITEM *psCleanupItem)
{
	const PVRSRV_DEVICE_NODE *psDevNode = psCleanupItem->psDevNode;

	/* For a zombie PMR to be eligible to be freed either the GPU MMU caches
	 * need to be flushed (the Firmware updates the sync) or the GPU power needs
	 * to be off. */
	return !PVRSRVHasCounter32Advanced(OSReadDeviceMem32(psCleanupItem->psSync->pui32LinAddr),
	                                   psCleanupItem->uiRequiredSyncValue) &&
	       !PVRSRVHasCounter32Advanced(psDevNode->uiPowerOffCounter,
	                                   psCleanupItem->uiRequiredPowerOffCounter);
}

static PVRSRV_ERROR _PmrZombieCleanup(void *pvData)
{
	PMR_ZOMBIE_CLEANUP_ITEM *psCleanupItem = pvData;
	DLLIST_NODE *psNode;
	IMG_BOOL bNeedRetry = IMG_FALSE;
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (_CanNotFreeZombies(psCleanupItem))
	{
		return PVRSRV_ERROR_RETRY;
	}

	do
	{
		/* hPMRZombieListLock will prevent removing a node while the list is
		 * processed. If the lock is already acquired by other process which
		 * intends to remove an item from the list it'll assure the list
		 * consistency.
		 * If this thread acquires the lock first it's possible that another
		 * thread might be holding PMR factory lock. */

		_ZombieListLock(psCleanupItem);
		psNode = dllist_get_next_node(&psCleanupItem->sZombieList);
		_ZombieListUnlock(psCleanupItem);

		if (psNode != NULL)
		{
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
			if (PMR_IsListNodeZombiePages(psNode))
			{
				PMR_ZOMBIE_PAGES* psZombiePages = PMR_GetZombiePagesFromNode(psNode);
				eError = psZombiePages->pfnFactoryFreeZombies(psZombiePages->pvFactoryPages);
				if (eError != PVRSRV_OK)
				{
					PVR_DPF((PVR_DBG_ERROR, "Cannot free zombie pages! Skipping object %p", psZombiePages));
					bNeedRetry = IMG_TRUE;
					continue;
				}
				_ZombieListLock(psCleanupItem);
				dllist_remove_node(psNode);
				psCleanupItem->psDevNode->uiPMRZombieCountInCleanup--;
				_ZombieListUnlock(psCleanupItem);

				OSFreeMem(psZombiePages);
			}
			else
#endif
			{
				PMR* psPMR = PMR_GetPMRFromNode(psNode);
				const PMR_IMPL_FUNCTAB *psFuncTable = psPMR->psFuncTab;
				_FactoryLock(psFuncTable);
				_ZombieListLock(psCleanupItem);
				/* It is possible that the element might have been removed so
				 * we have to check if the PMR is still a zombie.
				 * It's also possible that the PMR has been revived
				 * (PMRDequeueZombieAndRef()), mapped, unmapped and zombified
				 * again while the lock was not held.
				 * Considering above only immediately free the PMR if the
				 * PMR is still a part of this cleanup item. */
				if (psNode == dllist_get_next_node(&psCleanupItem->sZombieList))
				{
					dllist_remove_node(psNode);
					psCleanupItem->psDevNode->uiPMRZombieCountInCleanup--;
					/* Unlock here to avoid locking dependency with the power lock.
					 * It's okay to do it here since the factory lock is the one
					 * that needs to be held during PMR destruction. */
					_ZombieListUnlock(psCleanupItem);
					_PMRDestroy(psPMR);
				}
				else
				{
					_ZombieListUnlock(psCleanupItem);
				}
				_FactoryUnlock(psFuncTable);
			}
		}
	} while (psNode != NULL);

	if (bNeedRetry)
	{
		eError = PVRSRV_ERROR_RETRY;
	}
	else
	{
		OSFreeMem(psCleanupItem);
	}

	return eError;
}

IMG_BOOL PMRQueueZombiesForCleanup(PPVRSRV_DEVICE_NODE psDevNode)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PMR_ZOMBIE_CLEANUP_ITEM *psCleanupItem;

	/* Don't defer the freeing if we are currently unloading the driver
	 * or if the sync has been destroyed */
	if (psPVRSRVData->bUnload || psDevNode->psMMUCacheSyncPrim == NULL)
	{
		return IMG_FALSE;
	}

	OSLockAcquire(psDevNode->hPMRZombieListLock);

	if (dllist_is_empty(&psDevNode->sPMRZombieList))
	{
		OSLockRelease(psDevNode->hPMRZombieListLock);
		return IMG_FALSE;
	}

	psCleanupItem = OSAllocMem(sizeof(*psCleanupItem));
	if (psCleanupItem == NULL)
	{
		OSLockRelease(psDevNode->hPMRZombieListLock);
		return IMG_FALSE;
	}

	psCleanupItem->sCleanupThreadFn.pfnFree = _PmrZombieCleanup;
	psCleanupItem->sCleanupThreadFn.pvData = psCleanupItem;
	psCleanupItem->sCleanupThreadFn.bDependsOnHW = IMG_TRUE;
	CLEANUP_THREAD_SET_RETRY_TIMEOUT(&psCleanupItem->sCleanupThreadFn,
	                                 CLEANUP_THREAD_RETRY_TIMEOUT_MS_DEFAULT);

	psCleanupItem->psDevNode = psDevNode;
	psCleanupItem->psSync = psDevNode->psMMUCacheSyncPrim;
	psCleanupItem->uiRequiredSyncValue = psDevNode->ui32NextMMUInvalidateUpdate;
	psCleanupItem->uiRequiredPowerOffCounter = psDevNode->uiPowerOffCounterNext;

	/* This moves the zombie list to the cleanup item. */
	dllist_replace_head(&psDevNode->sPMRZombieList, &psCleanupItem->sZombieList);
	psDevNode->uiPMRZombieCountInCleanup += psDevNode->uiPMRZombieCount;
	psDevNode->uiPMRZombieCount = 0;

	OSLockRelease(psDevNode->hPMRZombieListLock);

	PVRSRVCleanupThreadAddWork(psDevNode, &psCleanupItem->sCleanupThreadFn);

	return IMG_TRUE;
}

void
PMRDequeueZombieAndRef(PMR *psPMR)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;
	DLLIST_NODE *psThis, *psNext;
	IMG_BOOL bIsOnZombieList = IMG_FALSE;

	PVR_ASSERT(psPMR != NULL);

	psDeviceNode = psPMR->psDevNode;

	/* If this was on a list then it's brought back to life. */
	OSLockAcquire(psDeviceNode->hPMRZombieListLock);

	/* Need to reference this PMR since it was about to be destroyed and its
	 * reference count must be 0 (can't use _Ref() due to the warning). */
	OSAtomicIncrement(&psPMR->iRefCount);

#if defined(SUPPORT_VALIDATION) || defined(DEBUG) || defined(PVR_TESTING_UTILS)
	PVR_LOG(("%s: 0x%p, key:0x%016" IMG_UINT64_FMTSPECX ", numLive:%d",
			__func__, psPMR, psPMR->uiKey, OSAtomicRead(&psPMR->psContext->uiNumLivePMRs)));
#endif

	/* If we got to this point the PMR must be on a list. If it's not
	 * it should mean a race of some sort. */
	PVR_ASSERT(!dllist_is_empty(&psPMR->sHeader.sZombieNode));

	/* For the sake of correct accounting check if the PMR is in the zombie
	 * list or in the cleanup item. */
	dllist_foreach_node(&psDeviceNode->sPMRZombieList, psThis, psNext)
	{
		if (psThis == &psPMR->sHeader.sZombieNode)
		{
			bIsOnZombieList = IMG_TRUE;
			break;
		}
	}

	/* Revive the PMR (remove it from the zombie list) and therefore
	 * prevent it's destruction. */
	dllist_remove_node(&psPMR->sHeader.sZombieNode);
	_IntFlagClr(psPMR, PMR_FLAG_INTERNAL_IS_ZOMBIE);

	if (bIsOnZombieList)
	{
		psDeviceNode->uiPMRZombieCount--;
	}
	else
	{
		psDeviceNode->uiPMRZombieCountInCleanup--;
	}

	OSLockRelease(psDeviceNode->hPMRZombieListLock);
}

void
PMRMarkForDeferFree(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	if (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags))
	{
		/* If PMR pages are allocated on demand the freeing is handled
		 * by `SUPPORT_PMR_PAGES_DEFERRED_FREE` path in
		 * `PMRUnlockSysPhysAddressesNested()`. */
		return;
	}

	_IntFlagSet(psPMR, PMR_FLAG_INTERNAL_DEFER_FREE);
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

static IMG_BOOL _PMRIsSparse(const PMR *psPMR)
{
	return _IntFlagIsSet(psPMR, PMR_FLAG_INTERNAL_SPARSE_ALLOC);
}

PVRSRV_ERROR
PMRCreatePMR(PVRSRV_DEVICE_NODE *psDevNode,
             PHYS_HEAP *psPhysHeap,
             PMR_SIZE_T uiLogicalSize,
             PMR_SIZE_T uiChunkSize,
             IMG_UINT32 ui32NumPhysChunks,
             IMG_UINT32 ui32NumVirtChunks,
             IMG_UINT32 *pui32MappingTable,
             PMR_LOG2ALIGN_T uiLog2ContiguityGuarantee,
             PMR_FLAGS_T uiFlags,
             const IMG_CHAR *pszAnnotation,
             const PMR_IMPL_FUNCTAB *psFuncTab,
             PMR_IMPL_PRIVDATA pvPrivData,
             PMR_IMPL_TYPE eType,
             PMR **ppsPMRPtr,
             IMG_UINT32 ui32PDumpFlags)
{
	PMR *psPMR = NULL;
	PVRSRV_ERROR eError;

	eError = _PMRCreate(uiLogicalSize,
	                    uiChunkSize,
	                    ui32NumPhysChunks,
	                    ui32NumVirtChunks,
	                    pui32MappingTable,
	                    uiLog2ContiguityGuarantee,
	                    uiFlags,
	                    &psPMR);
	PVR_GOTO_IF_ERROR(eError, e0);

	psPMR->psDevNode = psDevNode;
	psPMR->psPhysHeap = psPhysHeap;
	psPMR->psFuncTab = psFuncTab;
	psPMR->pszPDumpDefaultMemspaceName = PhysHeapPDumpMemspaceName(psPhysHeap);
	psPMR->pvFlavourData = pvPrivData;
	psPMR->eFlavour = eType;
	OSAtomicWrite(&psPMR->iRefCount, 1);

	OSStringLCopy(psPMR->szAnnotation, pszAnnotation, DEVMEM_ANNOTATION_MAX_LEN);

#if defined(PDUMP)
	{
		PMR_FLAGS_T uiFlags = psPMR->uiFlags;
		IMG_BOOL bInitialise = IMG_FALSE;
		IMG_UINT32 ui32InitValue = 0;

		if (PVRSRV_CHECK_ZERO_ON_ALLOC(uiFlags))
		{
			bInitialise = IMG_TRUE;
		}
		else if (PVRSRV_CHECK_POISON_ON_ALLOC(uiFlags))
		{
			ui32InitValue = 0xDEADBEEF;
			bInitialise = IMG_TRUE;
		}

		PDumpPMRMallocPMR(psPMR,
		                  (uiChunkSize * ui32NumVirtChunks),
		                  1ULL<<uiLog2ContiguityGuarantee,
		                  uiChunkSize,
		                  ui32NumPhysChunks,
		                  ui32NumVirtChunks,
		                  pui32MappingTable,
		                  uiLog2ContiguityGuarantee,
		                  bInitialise,
		                  ui32InitValue,
		                  &psPMR->hPDumpAllocHandle,
		                  ui32PDumpFlags);
	}
#endif

	*ppsPMRPtr = psPMR;

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR PMRLockSysPhysAddressesNested(PMR *psPMR,
                                           IMG_UINT32 ui32NestingLevel)
{
	PVRSRV_ERROR eError;

	PVR_ASSERT(psPMR != NULL);

	/* Note: taking this lock is not required to protect the PMR reference
	 * count, because the PMR reference count is atomic. Rather, taking
	 * the lock here guarantees that no caller will exit this function
	 * without the underlying physical addresses being locked.
	 */
	OSLockAcquireNested(psPMR->hLock, ui32NestingLevel);
	/* We also count the locks as references, so that the PMR is not freed
	 * while someone is using a physical address.
	 * "lock" here simply means incrementing the refcount. It means the
	 * refcount is multipurpose, but that's okay. We only have to promise
	 * that physical addresses are valid after this point, and remain valid
	 * until the corresponding PMRUnlockSysPhysAddressesOSMem()
	 */
	_Ref(psPMR);

	/* Also count locks separately from other types of references, to
	 * allow for debug assertions
	 */

	/* Only call callback if lockcount transitions from 0 to 1 (or 1 to 2 if not backed on demand) */
	if (OSAtomicIncrement(&psPMR->iLockCount) == (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags) ? 1 : 2))
	{
		if (psPMR->psFuncTab->pfnLockPhysAddresses != NULL)
		{
			/* must always have lock and unlock in pairs! */
			PVR_ASSERT(psPMR->psFuncTab->pfnUnlockPhysAddresses != NULL);

			eError = psPMR->psFuncTab->pfnLockPhysAddresses(psPMR->pvFlavourData);

			PVR_GOTO_IF_ERROR(eError, e1);
		}
	}
	OSLockRelease(psPMR->hLock);

	return PVRSRV_OK;

e1:
	OSAtomicDecrement(&psPMR->iLockCount);
	_Unref(psPMR);
	PVR_ASSERT(OSAtomicRead(&psPMR->iRefCount) != 0);
	OSLockRelease(psPMR->hLock);
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRLockSysPhysAddresses(PMR *psPMR)
{
	return PMRLockSysPhysAddressesNested(psPMR, 0);
}

PVRSRV_ERROR
PMRUnlockSysPhysAddresses(PMR *psPMR)
{
	return PMRUnlockSysPhysAddressesNested(psPMR, 2);
}

PVRSRV_ERROR
PMRUnlockSysPhysAddressesNested(PMR *psPMR, IMG_UINT32 ui32NestingLevel)
{
	PVRSRV_ERROR eError;
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	PMR_IMPL_ZOMBIEPAGES pvZombiePages = NULL;
	PMR_ZOMBIE_PAGES* psPMRZombiePages = NULL;
#endif

	PVR_ASSERT(psPMR != NULL);

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	/* Speculatively preallocate in order to simplify error handling later */
	psPMRZombiePages = OSAllocZMem(sizeof(PMR_ZOMBIE_PAGES));
	PVR_GOTO_IF_NOMEM(psPMRZombiePages, eError, e0);
#endif

	/* Acquiring the lock here, as well as during the Lock operation ensures
	 * the lock count hitting zero and the unlocking of the phys addresses is
	 * an atomic operation
	 */
	OSLockAcquireNested(psPMR->hLock, ui32NestingLevel);
	PVR_ASSERT(OSAtomicRead(&psPMR->iLockCount) > (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags) ? 0 : 1));

	if (OSAtomicDecrement(&psPMR->iLockCount) == (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags) ? 0 : 1))
	{
		if (psPMR->psFuncTab->pfnUnlockPhysAddresses != NULL)
		{
			PVR_ASSERT(psPMR->psFuncTab->pfnLockPhysAddresses != NULL);

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
			eError = psPMR->psFuncTab->pfnUnlockPhysAddresses(psPMR->pvFlavourData,
			                                                  &pvZombiePages);
#else
			eError = psPMR->psFuncTab->pfnUnlockPhysAddresses(psPMR->pvFlavourData);
#endif

			/* must never fail */
			PVR_ASSERT(eError == PVRSRV_OK);
		}
	}

	OSLockRelease(psPMR->hLock);

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	if (pvZombiePages != NULL)
	{
		PVRSRV_DEV_POWER_STATE ePowerState;
		PVRSRV_DEVICE_NODE *psDevNode;

		psDevNode = PMR_DeviceNode(psPMR);
		eError = PVRSRVGetDevicePowerState(psDevNode, &ePowerState);
		if (eError != PVRSRV_OK)
		{
			/* Treat unknown power state as ON. */
			ePowerState = PVRSRV_DEV_POWER_STATE_ON;
		}

		if (ePowerState == PVRSRV_DEV_POWER_STATE_OFF)
		{
			/* Free preallocated psPMRZombiePages as these won't be used*/
			OSFreeMem(psPMRZombiePages);

			eError = psPMR->psFuncTab->pfnFreeZombiePages(pvZombiePages);
			PVR_LOG_GOTO_IF_ERROR(eError, "Error when trying to free zombies immediately.", e0);
		}
		else
		{
			PVR_ASSERT(psPMRZombiePages != NULL);
			psPMRZombiePages->sHeader.bIsZombiePages = IMG_TRUE;
			psPMRZombiePages->pfnFactoryFreeZombies = psPMR->psFuncTab->pfnFreeZombiePages;
			psPMRZombiePages->pvFactoryPages = pvZombiePages;

			OSLockAcquire(psDevNode->hPMRZombieListLock);
			dllist_add_to_tail(&psDevNode->sPMRZombieList, &psPMRZombiePages->sHeader.sZombieNode);
			psDevNode->uiPMRZombieCount++;
			OSLockRelease(psDevNode->hPMRZombieListLock);
		}
	}
	else
	{
		OSFreeMem(psPMRZombiePages);
	}
#endif

	/* We also count the locks as references, so that the PMR is not
	 * freed while someone is using a physical address.
	 */
	_UnrefAndMaybeDestroy(psPMR);

	return PVRSRV_OK;

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
e0:
#endif
	return eError;
}

PVRSRV_ERROR
PMRUnpinPMR(PMR *psPMR, IMG_BOOL bDevMapped)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	PVR_ASSERT(psPMR != NULL);

	OSLockAcquire(psPMR->hLock);
	/* Stop if we still have references on the PMR */
	if (   ( bDevMapped && (OSAtomicRead(&psPMR->iRefCount) > 2))
			|| (!bDevMapped && (OSAtomicRead(&psPMR->iRefCount) > 1)) )
	{
		OSLockRelease(psPMR->hLock);
		PVR_DPF((PVR_DBG_ERROR,
				"%s: PMR is still referenced %u times. "
				"That means this PMR is probably exported or used somewhere else. "
				"Allowed are 2 references if it is mapped to device, otherwise 1.",
				__func__,
				OSAtomicRead(&psPMR->iRefCount)));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_STILL_REFERENCED, e_exit);
	}
	OSLockRelease(psPMR->hLock);

	if (psPMR->psFuncTab->pfnUnpinMem != NULL)
	{
		eError = psPMR->psFuncTab->pfnUnpinMem(psPMR->pvFlavourData);
		if (eError == PVRSRV_OK)
		{
			psPMR->bIsUnpinned = IMG_TRUE;
		}
	}

e_exit:
	return eError;
}

PVRSRV_ERROR
PMRPinPMR(PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	PVR_ASSERT(psPMR != NULL);

	if (psPMR->psFuncTab->pfnPinMem != NULL)
	{
		eError = psPMR->psFuncTab->pfnPinMem(psPMR->pvFlavourData,
		                                     psPMR->psMappingTable);
		if (eError == PVRSRV_OK)
		{
			psPMR->bIsUnpinned = IMG_FALSE;
		}
	}

	return eError;
}

PVRSRV_ERROR
PMRMakeLocalImportHandle(PMR *psPMR,
                         PMR **ppsPMR)
{
	PMRRefPMR(psPMR);
	*ppsPMR = psPMR;
	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRUnmakeLocalImportHandle(PMR *psPMR)
{
	PMRUnrefPMR(psPMR);
	return PVRSRV_OK;
}

/*
	Note:
	We pass back the PMR as it was passed in as a different handle type
	(DEVMEM_MEM_IMPORT) and it allows us to change the import structure
	type if we should need to embed any meta data in it.
 */
PVRSRV_ERROR
PMRLocalImportPMR(PMR *psPMR,
                  PMR **ppsPMR,
                  IMG_DEVMEM_SIZE_T *puiSize,
                  IMG_DEVMEM_ALIGN_T *puiAlign)
{
	_Ref(psPMR);

	/* Return the PMR */
	*ppsPMR = psPMR;
	*puiSize = psPMR->uiLogicalSize;
	*puiAlign = 1ULL << psPMR->uiLog2ContiguityGuarantee;
	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRGetUID(PMR *psPMR,
          IMG_UINT64 *pui64UID)
{
	PVR_ASSERT(psPMR != NULL);

	*pui64UID = psPMR->uiSerialNum;

	return PVRSRV_OK;
}

#if defined(SUPPORT_INSECURE_EXPORT)
PVRSRV_ERROR
PMRExportPMR(PMR *psPMR,
             PMR_EXPORT **ppsPMRExportPtr,
             PMR_SIZE_T *puiSize,
             PMR_LOG2ALIGN_T *puiLog2Contig,
             PMR_PASSWORD_T *puiPassword)
{
	IMG_UINT64 uiPassword;
	PMR_EXPORT *psPMRExport;

	uiPassword = psPMR->uiKey;

	psPMRExport = OSAllocMem(sizeof(*psPMRExport));
	PVR_RETURN_IF_NOMEM(psPMRExport);

	psPMRExport->psPMR = psPMR;
	_Ref(psPMR);
	/* The layout of a PMR can't change once exported
	 * to make sure the importers view of the memory is
	 * the same as exporter. */
	PMR_SetLayoutFixed(psPMR, IMG_TRUE);

	*ppsPMRExportPtr = psPMRExport;
	*puiSize = psPMR->uiLogicalSize;
	*puiLog2Contig = psPMR->uiLog2ContiguityGuarantee;
	*puiPassword = uiPassword;

	return PVRSRV_OK;
}


PVRSRV_ERROR
PMRUnexportPMR(PMR_EXPORT *psPMRExport)
{
	PVR_ASSERT(psPMRExport != NULL);
	PVR_ASSERT(psPMRExport->psPMR != NULL);
	PVR_ASSERT(OSAtomicRead(&psPMRExport->psPMR->iRefCount) > 0);

	_UnrefAndMaybeDestroy(psPMRExport->psPMR);

	OSFreeMem(psPMRExport);

	return PVRSRV_OK;
}


PVRSRV_ERROR
PMRImportPMR(PMR_EXPORT *psPMRExport,
             PMR_PASSWORD_T uiPassword,
             PMR_SIZE_T uiSize,
             PMR_LOG2ALIGN_T uiLog2Contig,
             PMR **ppsPMR)
{
	PMR *psPMR;

	PVR_ASSERT(OSAtomicRead(&psPMRExport->psPMR->iRefCount) > 0);

	psPMR = psPMRExport->psPMR;

	PVR_ASSERT((PMR_IsMemLayoutFixed(psPMR) == IMG_TRUE));

	if (psPMR->uiKey != uiPassword)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PMRImport: Import failed, password specified does not match the export"));
		return PVRSRV_ERROR_PMR_WRONG_PASSWORD_OR_STALE_PMR;
	}

	if (psPMR->uiLogicalSize != uiSize || psPMR->uiLog2ContiguityGuarantee != uiLog2Contig)
	{
		return PVRSRV_ERROR_PMR_MISMATCHED_ATTRIBUTES;
	}

	_Ref(psPMR);

	*ppsPMR = psPMR;

	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRUnimportPMR(PMR *psPMR)
{
	_UnrefAndMaybeDestroy(psPMR);

	return PVRSRV_OK;
}

#else /* if defined(SUPPORT_INSECURE_EXPORT) */

PVRSRV_ERROR
PMRExportPMR(PMR *psPMR,
             PMR_EXPORT **ppsPMRExportPtr,
             PMR_SIZE_T *puiSize,
             PMR_LOG2ALIGN_T *puiLog2Contig,
             PMR_PASSWORD_T *puiPassword)
{
	PVR_UNREFERENCED_PARAMETER(psPMR);
	PVR_UNREFERENCED_PARAMETER(ppsPMRExportPtr);
	PVR_UNREFERENCED_PARAMETER(puiSize);
	PVR_UNREFERENCED_PARAMETER(puiLog2Contig);
	PVR_UNREFERENCED_PARAMETER(puiPassword);

	return PVRSRV_OK;
}


PVRSRV_ERROR
PMRUnexportPMR(PMR_EXPORT *psPMRExport)
{
	PVR_UNREFERENCED_PARAMETER(psPMRExport);
	return PVRSRV_OK;
}


PVRSRV_ERROR
PMRImportPMR(PMR_EXPORT *psPMRExport,
             PMR_PASSWORD_T uiPassword,
             PMR_SIZE_T uiSize,
             PMR_LOG2ALIGN_T uiLog2Contig,
             PMR **ppsPMR)
{
	PVR_UNREFERENCED_PARAMETER(psPMRExport);
	PVR_UNREFERENCED_PARAMETER(uiPassword);
	PVR_UNREFERENCED_PARAMETER(uiSize);
	PVR_UNREFERENCED_PARAMETER(uiLog2Contig);
	PVR_UNREFERENCED_PARAMETER(ppsPMR);

	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRUnimportPMR(PMR *psPMR)
{
	PVR_UNREFERENCED_PARAMETER(psPMR);
	return PVRSRV_OK;
}
#endif /* if defined(SUPPORT_INSECURE_EXPORT) */

#if defined(SUPPORT_SECURE_EXPORT)
PVRSRV_ERROR PMRSecureUnexportPMR(PMR *psPMR)
{
	_UnrefAndMaybeDestroy(psPMR);
	return PVRSRV_OK;
}

static PVRSRV_ERROR _ReleaseSecurePMR(void *psExport)
{
	return PMRSecureUnexportPMR(psExport);
}

PVRSRV_ERROR PMRSecureExportPMR(CONNECTION_DATA *psConnection,
                                PVRSRV_DEVICE_NODE * psDevNode,
                                PMR *psPMR,
                                IMG_SECURE_TYPE *phSecure,
                                PMR **ppsPMR,
                                CONNECTION_DATA **ppsSecureConnection)
{
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psDevNode);
	PVR_UNREFERENCED_PARAMETER(ppsSecureConnection);

	/* We are acquiring reference to PMR here because OSSecureExport
	 * releases bridge lock and PMR lock for a moment and we don't want PMR
	 * to be removed by other thread in the meantime. */
	_Ref(psPMR);

	eError = OSSecureExport("secure_pmr",
	                        _ReleaseSecurePMR,
	                        (void *) psPMR,
	                        phSecure);
	PVR_GOTO_IF_ERROR(eError, e0);

	*ppsPMR = psPMR;

	/* Mark the PMR immutable once exported
	 * This allows the importers and exporter to have
	 * the same view of the memory */
	PMR_SetLayoutFixed(psPMR, IMG_TRUE);

	return PVRSRV_OK;
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	_UnrefAndMaybeDestroy(psPMR);
	return eError;
}

PVRSRV_ERROR PMRSecureImportPMR(CONNECTION_DATA *psConnection,
                                PVRSRV_DEVICE_NODE *psDevNode,
                                IMG_SECURE_TYPE hSecure,
                                PMR **ppsPMR,
                                IMG_DEVMEM_SIZE_T *puiSize,
                                IMG_DEVMEM_ALIGN_T *puiAlign)
{
	PVRSRV_ERROR eError;
	PMR *psPMR;

	PVR_UNREFERENCED_PARAMETER(psConnection);

	eError = OSSecureImport(hSecure, (void **) &psPMR);
	PVR_GOTO_IF_ERROR(eError, e0);

	if (psPMR->psDevNode != psDevNode)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: PMR invalid for this device", __func__));
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}

	_Ref(psPMR);
	/* The PMR should be immutable once exported
	 * This allows the importers and exporter to have
	 * the same view of the memory */
	PVR_ASSERT(PMR_IsMemLayoutFixed(psPMR) == IMG_TRUE);

	/* Return the PMR */
	*ppsPMR = psPMR;
	*puiSize = psPMR->uiLogicalSize;
	*puiAlign = 1 << psPMR->uiLog2ContiguityGuarantee;
	return PVRSRV_OK;
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR PMRSecureUnimportPMR(PMR *psPMR)
{
	_UnrefAndMaybeDestroy(psPMR);
	return PVRSRV_OK;
}
#endif

#if defined(PVRSRV_ENABLE_GPU_MEMORY_INFO)
PVRSRV_ERROR
PMRStoreRIHandle(PMR *psPMR,
                 void *hRIHandle)
{
	PVR_ASSERT(psPMR != NULL);

	psPMR->hRIHandle = hRIHandle;
	return PVRSRV_OK;
}
#endif

static PVRSRV_ERROR
_PMRAcquireKernelMappingData(PMR *psPMR,
                             size_t uiLogicalOffset,
                             size_t uiSize,
                             void **ppvKernelAddressOut,
                             size_t *puiLengthOut,
                             IMG_HANDLE *phPrivOut,
                             IMG_BOOL bMapSparse)
{
	PVRSRV_ERROR eError;
	void *pvKernelAddress;
	IMG_HANDLE hPriv;

	PVR_ASSERT(psPMR != NULL);

	if (_PMRIsSparse(psPMR) && !bMapSparse)
	{
		/* Mapping of sparse allocations must be signalled. */
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}

	/* Acquire/Release functions must be overridden in pairs */
	if (psPMR->psFuncTab->pfnAcquireKernelMappingData == NULL)
	{
		PVR_ASSERT (psPMR->psFuncTab->pfnReleaseKernelMappingData == NULL);

		/* If PMR implementation does not supply this pair of
		 * functions, it means they do not permit the PMR to be mapped
		 * into kernel memory at all
		 */
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_NOT_PERMITTED, e0);
	}
	PVR_ASSERT (psPMR->psFuncTab->pfnReleaseKernelMappingData != NULL);

	eError = psPMR->psFuncTab->pfnAcquireKernelMappingData(psPMR->pvFlavourData,
	                                                       uiLogicalOffset,
	                                                       uiSize,
	                                                       &pvKernelAddress,
	                                                       &hPriv,
	                                                       psPMR->uiFlags);
	PVR_GOTO_IF_ERROR(eError, e0);

	*ppvKernelAddressOut = pvKernelAddress;
	if (uiSize == 0)
	{
		/* Zero size means map in the whole PMR ... */
		*puiLengthOut = (size_t)psPMR->uiLogicalSize;
	}
	else if (uiSize > (1 << psPMR->uiLog2ContiguityGuarantee))
	{
		/* ... map in the requested pages ... */
		*puiLengthOut = uiSize;
	}
	else
	{
		/* ... otherwise we just map in one page */
		*puiLengthOut = 1 << psPMR->uiLog2ContiguityGuarantee;
	}
	*phPrivOut = hPriv;

	return PVRSRV_OK;

e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRAcquireKernelMappingData(PMR *psPMR,
                            size_t uiLogicalOffset,
                            size_t uiSize,
                            void **ppvKernelAddressOut,
                            size_t *puiLengthOut,
                            IMG_HANDLE *phPrivOut)
{
	return _PMRAcquireKernelMappingData(psPMR,
	                                    uiLogicalOffset,
	                                    uiSize,
	                                    ppvKernelAddressOut,
	                                    puiLengthOut,
	                                    phPrivOut,
	                                    IMG_FALSE);
}

PVRSRV_ERROR
PMRAcquireSparseKernelMappingData(PMR *psPMR,
                                  size_t uiLogicalOffset,
                                  size_t uiSize,
                                  void **ppvKernelAddressOut,
                                  size_t *puiLengthOut,
                                  IMG_HANDLE *phPrivOut)
{
	return _PMRAcquireKernelMappingData(psPMR,
	                                    uiLogicalOffset,
	                                    uiSize,
	                                    ppvKernelAddressOut,
	                                    puiLengthOut,
	                                    phPrivOut,
	                                    IMG_TRUE);
}

PVRSRV_ERROR
PMRReleaseKernelMappingData(PMR *psPMR,
                            IMG_HANDLE hPriv)
{
	PVR_ASSERT (psPMR->psFuncTab->pfnAcquireKernelMappingData != NULL);
	PVR_ASSERT (psPMR->psFuncTab->pfnReleaseKernelMappingData != NULL);

	psPMR->psFuncTab->pfnReleaseKernelMappingData(psPMR->pvFlavourData,
	                                              hPriv);

	return PVRSRV_OK;
}

#if defined(INTEGRITY_OS)

PVRSRV_ERROR
PMRMapMemoryObject(PMR *psPMR,
                   IMG_HANDLE *phMemObj,
                   void **pvClientAddr,
                   IMG_HANDLE *phPrivOut)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_HANDLE hPriv = *phPrivOut;

	PVR_ASSERT (psPMR->psFuncTab->pfnMapMemoryObject != NULL);

	eError = psPMR->psFuncTab->pfnMapMemoryObject(hPriv, phMemObj, pvClientAddr, phPrivOut);

	return eError;
}

PVRSRV_ERROR
PMRUnmapMemoryObject(PMR *psPMR,
                     IMG_HANDLE hPriv)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	PVR_ASSERT (psPMR->psFuncTab->pfnUnmapMemoryObject != NULL);

	eError = psPMR->psFuncTab->pfnUnmapMemoryObject(hPriv);

	return eError;
}

#endif /* INTEGRITY_OS */

/*
	_PMRLogicalOffsetToPhysicalOffset

	Translate between the "logical" offset which the upper levels
	provide and the physical offset which is what the PMR
	factories works on.

	As well as returning the physical offset we return the number of
	bytes remaining till the next chunk and if this chunk is valid.

	For multi-page operations, upper layers communicate their
	Log2PageSize else argument is redundant (set to zero).
 */

static PVRSRV_ERROR
_PMRLogicalOffsetToPhysicalOffset(const PMR *psPMR,
                                  IMG_UINT32 ui32Log2PageSize,
                                  IMG_UINT32 ui32NumOfPages,
                                  IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                                  IMG_DEVMEM_OFFSET_T *puiPhysicalOffset,
                                  IMG_UINT32 *pui32BytesRemain,
                                  IMG_BOOL *bValid)
{
	PMR_MAPPING_TABLE *psMappingTable = psPMR->psMappingTable;
	IMG_DEVMEM_OFFSET_T uiPageSize = 1ULL << ui32Log2PageSize;
	IMG_DEVMEM_OFFSET_T uiOffset = uiLogicalOffset;
	IMG_UINT64 ui64ChunkIndex;
	IMG_UINT32 ui32Remain;
	IMG_UINT32 idx;

	/* Must be translating at least a page */
	PVR_ASSERT(ui32NumOfPages);

	if (psMappingTable->ui32NumPhysChunks == psMappingTable->ui32NumVirtChunks)
	{
		/* Fast path the common case, as logical and physical offsets are
			equal we assume the ui32NumOfPages span is also valid */
		*pui32BytesRemain = TRUNCATE_64BITS_TO_32BITS(psPMR->uiLogicalSize - uiOffset);
		puiPhysicalOffset[0] = uiOffset;
		bValid[0] = IMG_TRUE;

		if (ui32NumOfPages > 1)
		{
			/* initial offset may not be page aligned, round down */
			uiOffset &= ~(uiPageSize-1);
			for (idx=1; idx < ui32NumOfPages; idx++)
			{
				uiOffset += uiPageSize;
				puiPhysicalOffset[idx] = uiOffset;
				bValid[idx] = IMG_TRUE;
			}
		}
	}
	else
	{
		for (idx=0; idx < ui32NumOfPages; idx++)
		{
			ui64ChunkIndex = OSDivide64r64(
					uiOffset,
					TRUNCATE_64BITS_TO_32BITS(psMappingTable->uiChunkSize),
					&ui32Remain);

			/* In some cases ui32NumOfPages can come from the user space which
			 * means that the uiOffset could go out-of-bounds when the number
			 * of pages is invalid. */
			if (ui64ChunkIndex >= psMappingTable->ui32NumVirtChunks)
			{
				return PVRSRV_ERROR_BAD_MAPPING;
			}

			if (psMappingTable->aui32Translation[ui64ChunkIndex] == TRANSLATION_INVALID)
			{
				bValid[idx] = IMG_FALSE;
			}
			else
			{
				bValid[idx] = IMG_TRUE;
			}

			if (idx == 0)
			{
				if (ui32Remain == 0)
				{
					/* Start of chunk so return the chunk size */
					*pui32BytesRemain = TRUNCATE_64BITS_TO_32BITS(psMappingTable->uiChunkSize);
				}
				else
				{
					*pui32BytesRemain = TRUNCATE_64BITS_TO_32BITS(psMappingTable->uiChunkSize - ui32Remain);
				}

				/* initial offset may not be page aligned, round down */
				uiOffset &= ~(uiPageSize-1);
			}

			puiPhysicalOffset[idx] = psMappingTable->aui32Translation[ui64ChunkIndex] * psMappingTable->uiChunkSize + ui32Remain;

			uiOffset += uiPageSize;
		}
	}

	return PVRSRV_OK;
}

static PVRSRV_ERROR
_PMR_ReadBytesPhysical(PMR *psPMR,
                       IMG_DEVMEM_OFFSET_T uiPhysicalOffset,
                       IMG_UINT8 *pcBuffer,
                       size_t uiBufSz,
                       size_t *puiNumBytes)
{
	PVRSRV_ERROR eError;

	if (psPMR->psFuncTab->pfnReadBytes != NULL)
	{
		/* defer to callback if present */

		eError = PMRLockSysPhysAddresses(psPMR);
		PVR_GOTO_IF_ERROR(eError, e0);

		eError = psPMR->psFuncTab->pfnReadBytes(psPMR->pvFlavourData,
		                                        uiPhysicalOffset,
		                                        pcBuffer,
		                                        uiBufSz,
		                                        puiNumBytes);
		PMRUnlockSysPhysAddresses(psPMR);
		PVR_GOTO_IF_ERROR(eError, e0);
	}
	else if (psPMR->psFuncTab->pfnAcquireKernelMappingData)
	{
		/* "default" handler for reading bytes */

		IMG_HANDLE hKernelMappingHandle;
		IMG_UINT8 *pcKernelAddress;

		eError = psPMR->psFuncTab->pfnAcquireKernelMappingData(psPMR->pvFlavourData,
		                                                       (size_t) uiPhysicalOffset,
		                                                       uiBufSz,
		                                                       (void **)&pcKernelAddress,
		                                                       &hKernelMappingHandle,
		                                                       psPMR->uiFlags);
		PVR_GOTO_IF_ERROR(eError, e0);

		/* Use the conservative 'DeviceMemCopy' here because we can't
		 * know if this PMR will be mapped cached.
		 */

		OSDeviceMemCopy(&pcBuffer[0], pcKernelAddress, uiBufSz);
		*puiNumBytes = uiBufSz;

		psPMR->psFuncTab->pfnReleaseKernelMappingData(psPMR->pvFlavourData,
		                                              hKernelMappingHandle);
	}
	else
	{
		OSPanic();
		PVR_LOG_GOTO_WITH_ERROR("psPMR->psFuncTab", eError, PVRSRV_ERROR_INVALID_PARAMS, e0);
	}

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	*puiNumBytes = 0;
	return eError;
}

PVRSRV_ERROR
PMR_ReadBytes(PMR *psPMR,
              IMG_DEVMEM_OFFSET_T uiLogicalOffset,
              IMG_UINT8 *pcBuffer,
              size_t uiBufSz,
              size_t *puiNumBytes)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_DEVMEM_OFFSET_T uiPhysicalOffset;
	size_t uiBytesCopied = 0;

	/* Check for integer overflow as uiLogicalOffset might come from the client */
	if (uiLogicalOffset + uiBufSz < uiLogicalOffset)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (uiLogicalOffset + uiBufSz > psPMR->uiLogicalSize)
	{
		uiBufSz = TRUNCATE_64BITS_TO_32BITS(psPMR->uiLogicalSize - uiLogicalOffset);
	}
	PVR_ASSERT(uiBufSz > 0);
	PVR_ASSERT(uiBufSz <= psPMR->uiLogicalSize);

	/* PMR implementations can override this. If they don't, a "default"
	 * handler uses kernel virtual mappings.  If the kernel can't
	 * provide a kernel virtual mapping, this function fails.
	 */
	PVR_ASSERT(psPMR->psFuncTab->pfnAcquireKernelMappingData != NULL ||
	           psPMR->psFuncTab->pfnReadBytes != NULL);

	while (uiBytesCopied != uiBufSz)
	{
		IMG_UINT32 ui32Remain;
		size_t uiBytesToCopy;
		size_t uiRead;
		IMG_BOOL bValid;

		eError = _PMRLogicalOffsetToPhysicalOffset(psPMR,
		                                           0,
		                                           1,
		                                           uiLogicalOffset,
		                                           &uiPhysicalOffset,
		                                           &ui32Remain,
		                                           &bValid);
		PVR_LOG_RETURN_IF_ERROR(eError, "_PMRLogicalOffsetToPhysicalOffset");

		/* Copy till either then end of the chunk or end
		 * of the buffer
		 */
		uiBytesToCopy = MIN(uiBufSz - uiBytesCopied, ui32Remain);

		if (bValid)
		{
			/* Read the data from the PMR */
			eError = _PMR_ReadBytesPhysical(psPMR,
			                                uiPhysicalOffset,
			                                &pcBuffer[uiBytesCopied],
			                                uiBytesToCopy,
			                                &uiRead);
			if ((eError != PVRSRV_OK) || (uiRead != uiBytesToCopy))
			{
				PVR_DPF((PVR_DBG_ERROR,
						"%s: Failed to read chunk (eError = %s, uiRead = " IMG_SIZE_FMTSPEC " uiBytesToCopy = " IMG_SIZE_FMTSPEC ")",
						__func__,
						PVRSRVGetErrorString(eError),
						uiRead,
						uiBytesToCopy));
				/* Bail out as soon as we hit an error */
				break;
			}
		}
		else
		{
			PVR_DPF((PVR_DBG_WARNING,
					"%s: Invalid phys offset at logical offset (" IMG_DEVMEM_OFFSET_FMTSPEC ") logical size (" IMG_DEVMEM_OFFSET_FMTSPEC ")",
					__func__,
					uiLogicalOffset,
					psPMR->uiLogicalSize));
			/* Fill invalid chunks with 0 */
			OSCachedMemSet(&pcBuffer[uiBytesCopied], 0, uiBytesToCopy);
			uiRead = uiBytesToCopy;
			eError = PVRSRV_ERROR_FAILED_TO_GET_PHYS_ADDR;
		}
		uiLogicalOffset += uiRead;
		uiBytesCopied += uiRead;
	}

	*puiNumBytes = uiBytesCopied;
	return eError;
}

static PVRSRV_ERROR
_PMR_WriteBytesPhysical(PMR *psPMR,
                        IMG_DEVMEM_OFFSET_T uiPhysicalOffset,
                        IMG_UINT8 *pcBuffer,
                        size_t uiBufSz,
                        size_t *puiNumBytes)
{
	PVRSRV_ERROR eError;

	if (psPMR->psFuncTab->pfnWriteBytes != NULL)
	{
		/* defer to callback if present */

		eError = PMRLockSysPhysAddresses(psPMR);
		PVR_GOTO_IF_ERROR(eError, e0);

		eError = psPMR->psFuncTab->pfnWriteBytes(psPMR->pvFlavourData,
		                                         uiPhysicalOffset,
		                                         pcBuffer,
		                                         uiBufSz,
		                                         puiNumBytes);
		PMRUnlockSysPhysAddresses(psPMR);
		PVR_GOTO_IF_ERROR(eError, e0);
	}
	else if (psPMR->psFuncTab->pfnAcquireKernelMappingData)
	{
		/* "default" handler for reading bytes */

		IMG_HANDLE hKernelMappingHandle;
		IMG_UINT8 *pcKernelAddress;

		eError = psPMR->psFuncTab->pfnAcquireKernelMappingData(psPMR->pvFlavourData,
		                                                       (size_t) uiPhysicalOffset,
		                                                       uiBufSz,
		                                                       (void **)&pcKernelAddress,
		                                                       &hKernelMappingHandle,
		                                                       psPMR->uiFlags);
		PVR_GOTO_IF_ERROR(eError, e0);

		/* Use the conservative 'DeviceMemCopy' here because we can't know
		 * if this PMR will be mapped cached.
		 */

		OSDeviceMemCopy(pcKernelAddress, &pcBuffer[0], uiBufSz);
		*puiNumBytes = uiBufSz;

		psPMR->psFuncTab->pfnReleaseKernelMappingData(psPMR->pvFlavourData,
		                                              hKernelMappingHandle);
	}
	else
	{
		/* The write callback is optional as it's only required by the
		 * debug tools
		 */
		OSPanic();
		PVR_LOG_GOTO_WITH_ERROR("psPMR->psFuncTab", eError, PVRSRV_ERROR_PMR_NOT_PERMITTED, e0);
	}

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	*puiNumBytes = 0;
	return eError;
}

PVRSRV_ERROR
PMR_WriteBytes(PMR *psPMR,
               IMG_DEVMEM_OFFSET_T uiLogicalOffset,
               IMG_UINT8 *pcBuffer,
               size_t uiBufSz,
               size_t *puiNumBytes)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_DEVMEM_OFFSET_T uiPhysicalOffset;
	size_t uiBytesCopied = 0;

	/* Check for integer overflow as uiLogicalOffset might come from the client */
	if (uiLogicalOffset + uiBufSz < uiLogicalOffset)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (uiLogicalOffset + uiBufSz > psPMR->uiLogicalSize)
	{
		uiBufSz = TRUNCATE_64BITS_TO_32BITS(psPMR->uiLogicalSize - uiLogicalOffset);
	}
	PVR_ASSERT(uiBufSz > 0);
	PVR_ASSERT(uiBufSz <= psPMR->uiLogicalSize);

	/* PMR implementations can override this. If they don't, a "default"
	 * handler uses kernel virtual mappings. If the kernel can't provide
	 * a kernel virtual mapping, this function fails.
	 */
	PVR_ASSERT(psPMR->psFuncTab->pfnAcquireKernelMappingData != NULL ||
	           psPMR->psFuncTab->pfnWriteBytes != NULL);

	while (uiBytesCopied != uiBufSz)
	{
		IMG_UINT32 ui32Remain;
		size_t uiBytesToCopy;
		size_t uiWrite;
		IMG_BOOL bValid;

		eError = _PMRLogicalOffsetToPhysicalOffset(psPMR,
		                                           0,
		                                           1,
		                                           uiLogicalOffset,
		                                           &uiPhysicalOffset,
		                                           &ui32Remain,
		                                           &bValid);
		PVR_LOG_RETURN_IF_ERROR(eError, "_PMRLogicalOffsetToPhysicalOffset");

		/* Copy till either then end of the chunk or end of the buffer
		 */
		uiBytesToCopy = MIN(uiBufSz - uiBytesCopied, ui32Remain);

		if (bValid)
		{
			/* Write the data to the PMR */
			eError = _PMR_WriteBytesPhysical(psPMR,
			                                 uiPhysicalOffset,
			                                 &pcBuffer[uiBytesCopied],
			                                 uiBytesToCopy,
			                                 &uiWrite);
			if ((eError != PVRSRV_OK) || (uiWrite != uiBytesToCopy))
			{
				PVR_DPF((PVR_DBG_ERROR,
						"%s: Failed to read chunk (eError = %s, uiWrite = " IMG_SIZE_FMTSPEC " uiBytesToCopy = " IMG_SIZE_FMTSPEC ")",
						__func__,
						PVRSRVGetErrorString(eError),
						uiWrite,
						uiBytesToCopy));
				/* Bail out as soon as we hit an error */
				break;
			}
		}
		else
		{
			/* Ignore writes to invalid pages */
			uiWrite = uiBytesToCopy;
		}
		uiLogicalOffset += uiWrite;
		uiBytesCopied += uiWrite;
	}

	*puiNumBytes = uiBytesCopied;
	return eError;
}

PVRSRV_ERROR
PMRMMapPMR(PMR *psPMR, PMR_MMAP_DATA pOSMMapData, PVRSRV_MEMALLOCFLAGS_T uiFlags)
{
	/* if writeable mapping is requested on non-writeable PMR then fail */
	PVR_RETURN_IF_FALSE(PVRSRV_CHECK_CPU_WRITEABLE(psPMR->uiFlags) ||
	                    !PVRSRV_CHECK_CPU_WRITEABLE(uiFlags),
	                    PVRSRV_ERROR_PMR_NOT_PERMITTED);

	if (psPMR->psFuncTab->pfnMMap)
	{
		return psPMR->psFuncTab->pfnMMap(psPMR->pvFlavourData, psPMR, pOSMMapData);
	}

	return OSMMapPMRGeneric(psPMR, pOSMMapData);
}

void
PMRRefPMR(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	_Ref(psPMR);
}

PVRSRV_ERROR
PMRUnrefPMR(PMR *psPMR)
{
	_UnrefAndMaybeDestroy(psPMR);
	return PVRSRV_OK;
}

void
PMRRefPMR2(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	_Ref(psPMR);
}

void
PMRUnrefPMR2(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	_UnrefAndMaybeDestroy(psPMR);
}

PVRSRV_ERROR
PMRUnrefUnlockPMR(PMR *psPMR)
{
	PMRUnlockSysPhysAddresses(psPMR);

	PMRUnrefPMR(psPMR);

	return PVRSRV_OK;
}

#define PMR_MAPCOUNT_MIN 0
#define PMR_MAPCOUNT_MAX IMG_INT32_MAX
void
PMRCpuMapCountIncr(PMR *psPMR)
{
	if (OSAtomicAddUnless(&psPMR->iCpuMapCount, 1, PMR_MAPCOUNT_MAX) == PMR_MAPCOUNT_MAX)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: iCpuMapCount for PMR: @0x%p (%s) has overflowed.",
		                        __func__,
		                        psPMR,
		                        psPMR->szAnnotation));
		OSWarnOn(1);
	}
}

void
PMRCpuMapCountDecr(PMR *psPMR)
{
	if (OSAtomicSubtractUnless(&psPMR->iCpuMapCount, 1, PMR_MAPCOUNT_MIN) == PMR_MAPCOUNT_MIN)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: iCpuMapCount (now %d) for PMR: @0x%p (%s) has underflowed.",
		                        __func__,
		                        (IMG_INT32) OSAtomicRead(&psPMR->iCpuMapCount),
		                        psPMR,
		                        psPMR->szAnnotation));
		OSWarnOn(1);
	}
}

IMG_BOOL
PMR_IsCpuMapped(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return (OSAtomicRead(&psPMR->iCpuMapCount) > 0);
}

void
PMRGpuResCountIncr(PMR *psPMR)
{
	if (psPMR->iAssociatedResCount == PMR_MAPCOUNT_MAX)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: iAssociatedResCount for PMR: @0x%p (%s) has overflowed.",
		                        __func__,
		                        psPMR,
		                        psPMR->szAnnotation));
		OSWarnOn(1);
		return;
	}

	psPMR->iAssociatedResCount++;
}

void
PMRGpuResCountDecr(PMR *psPMR)
{
	if (psPMR->iAssociatedResCount == PMR_MAPCOUNT_MIN)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: iAssociatedResCount (now %d) for PMR: @0x%p (%s) has underflowed.",
		                        __func__,
		                        psPMR->iAssociatedResCount,
		                        psPMR,
		                        psPMR->szAnnotation));
		OSWarnOn(1);
		return;
	}

	psPMR->iAssociatedResCount--;
}

IMG_BOOL
PMR_IsGpuMultiMapped(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return psPMR->iAssociatedResCount > 1;
}

PVRSRV_DEVICE_NODE *
PMR_DeviceNode(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return psPMR->psDevNode;
}

PMR_FLAGS_T
PMR_Flags(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return psPMR->uiFlags;
}

IMG_BOOL
PMR_IsSparse(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return _PMRIsSparse(psPMR);
}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
IMG_BOOL
PMR_IsZombie(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return _IntFlagIsSet(psPMR, PMR_FLAG_INTERNAL_IS_ZOMBIE);
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

IMG_BOOL
PMR_IsUnpinned(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);

	return psPMR->bIsUnpinned;
}

/* Function that alters the mutability property
 * of the PMR
 * Setting it to TRUE makes sure the PMR memory layout
 * can't be changed through future calls */
void
PMR_SetLayoutFixed(PMR *psPMR, IMG_BOOL bFlag)
{
	PVR_ASSERT(psPMR != NULL);

	if (bFlag)
	{
		_IntFlagSet(psPMR, PMR_FLAG_INTERNAL_NO_LAYOUT_CHANGE);
	}
	else
	{
		_IntFlagClr(psPMR, PMR_FLAG_INTERNAL_NO_LAYOUT_CHANGE);
	}

}

IMG_BOOL PMR_IsMemLayoutFixed(PMR *psPMR)
{

	PVR_ASSERT(psPMR != NULL);

	return _IntFlagIsSet(psPMR, PMR_FLAG_INTERNAL_NO_LAYOUT_CHANGE);
}

PVRSRV_ERROR
PMR_LogicalSize(const PMR *psPMR,
                IMG_DEVMEM_SIZE_T *puiLogicalSize)
{
	PVR_ASSERT(psPMR != NULL);

	*puiLogicalSize = psPMR->uiLogicalSize;
	return PVRSRV_OK;
}

PVRSRV_ERROR
PMR_PhysicalSize(const PMR *psPMR,
                 IMG_DEVMEM_SIZE_T *puiPhysicalSize)
{
	PVR_ASSERT(psPMR != NULL);

	/* iLockCount will be > 0 for any backed PMR (backed on demand or not) */
	if ((OSAtomicRead(&psPMR->iLockCount) > 0) && !psPMR->bIsUnpinned)
	{
		if (_PMRIsSparse(psPMR))
		{
			*puiPhysicalSize = psPMR->psMappingTable->uiChunkSize * psPMR->psMappingTable->ui32NumPhysChunks;
		}
		else
		{
			*puiPhysicalSize = psPMR->uiLogicalSize;
		}
	}
	else
	{
		*puiPhysicalSize = 0;
	}
	return PVRSRV_OK;
}

PHYS_HEAP *
PMR_PhysHeap(const PMR *psPMR)
{
	return psPMR->psPhysHeap;
}

PVRSRV_ERROR
PMR_IsOffsetValid(const PMR *psPMR,
                  IMG_UINT32 ui32Log2PageSize,
                  IMG_UINT32 ui32NumOfPages,
                  IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                  IMG_BOOL *pbValid)
{
	IMG_DEVMEM_OFFSET_T auiPhysicalOffset[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_UINT32 aui32BytesRemain[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEVMEM_OFFSET_T *puiPhysicalOffset = auiPhysicalOffset;
	IMG_UINT32 *pui32BytesRemain = aui32BytesRemain;
	PVRSRV_ERROR eError = PVRSRV_OK;

	PVR_ASSERT(psPMR != NULL);
	PVR_ASSERT(psPMR->uiLogicalSize >= uiLogicalOffset);

	if (ui32NumOfPages > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		puiPhysicalOffset = OSAllocMem(ui32NumOfPages * sizeof(IMG_DEVMEM_OFFSET_T));
		PVR_GOTO_IF_NOMEM(puiPhysicalOffset, eError, e0);

		pui32BytesRemain = OSAllocMem(ui32NumOfPages * sizeof(IMG_UINT32));
		PVR_GOTO_IF_NOMEM(pui32BytesRemain, eError, e0);
	}

	eError = _PMRLogicalOffsetToPhysicalOffset(psPMR,
	                                           ui32Log2PageSize,
	                                           ui32NumOfPages,
	                                           uiLogicalOffset,
	                                           puiPhysicalOffset,
	                                           pui32BytesRemain,
	                                           pbValid);
	PVR_LOG_IF_ERROR(eError, "_PMRLogicalOffsetToPhysicalOffset");

e0:
	if (puiPhysicalOffset != auiPhysicalOffset && puiPhysicalOffset != NULL)
	{
		OSFreeMem(puiPhysicalOffset);
	}

	if (pui32BytesRemain != aui32BytesRemain && pui32BytesRemain != NULL)
	{
		OSFreeMem(pui32BytesRemain);
	}

	return eError;
}

PMR_MAPPING_TABLE *
PMR_GetMappigTable(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return psPMR->psMappingTable;

}

IMG_UINT32
PMR_GetLog2Contiguity(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return psPMR->uiLog2ContiguityGuarantee;
}

IMG_UINT32 PMRGetMaxChunkCount(PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return (PMR_MAX_SUPPORTED_SIZE >> psPMR->uiLog2ContiguityGuarantee);
}

const IMG_CHAR *
PMR_GetAnnotation(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return psPMR->szAnnotation;
}

PMR_IMPL_TYPE
PMR_GetType(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return psPMR->eFlavour;
}

IMG_CHAR *
PMR_GetTypeStr(const PMR *psPMR)
{
	static IMG_CHAR *pszFlavour[] = {
#define X(type) #type
		PMR_IMPL_TYPES
#undef X
	};

	if (psPMR->eFlavour >= PMR_TYPE_LAST)
	{
		return "INVALID";
	}

	return pszFlavour[psPMR->eFlavour];
}

IMG_INT32
PMR_GetRefCount(const PMR *psPMR)
{
	PVR_ASSERT(psPMR != NULL);
	return OSAtomicRead(&psPMR->iRefCount);
}

/* must have called PMRLockSysPhysAddresses() before calling this! */
PVRSRV_ERROR
PMR_DevPhysAddr(const PMR *psPMR,
                IMG_UINT32 ui32Log2PageSize,
                IMG_UINT32 ui32NumOfPages,
                IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                IMG_DEV_PHYADDR *psDevAddrPtr,
                IMG_BOOL *pbValid)
{
	IMG_UINT32 ui32Remain;
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_DEVMEM_OFFSET_T auiPhysicalOffset[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEVMEM_OFFSET_T *puiPhysicalOffset = auiPhysicalOffset;

	PVR_ASSERT(psPMR != NULL);
	PVR_ASSERT(ui32NumOfPages > 0);
	PVR_ASSERT(psPMR->psFuncTab->pfnDevPhysAddr != NULL);

#ifdef PVRSRV_NEED_PVR_ASSERT
	PVR_ASSERT(OSAtomicRead(&psPMR->iLockCount) > (PVRSRV_CHECK_ON_DEMAND(psPMR->uiFlags) ? 0 : 1));
#endif

	if (ui32NumOfPages > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		puiPhysicalOffset = OSAllocMem(ui32NumOfPages * sizeof(IMG_DEVMEM_OFFSET_T));
		PVR_RETURN_IF_NOMEM(puiPhysicalOffset);
	}

	eError = _PMRLogicalOffsetToPhysicalOffset(psPMR,
	                                  ui32Log2PageSize,
	                                  ui32NumOfPages,
	                                  uiLogicalOffset,
	                                  puiPhysicalOffset,
	                                  &ui32Remain,
	                                  pbValid);
	PVR_LOG_GOTO_IF_ERROR(eError, "_PMRLogicalOffsetToPhysicalOffset", FreeOffsetArray);
	if (*pbValid || _PMRIsSparse(psPMR))
	{
		/* Sparse PMR may not always have the first page valid */
		eError = psPMR->psFuncTab->pfnDevPhysAddr(psPMR->pvFlavourData,
		                                          ui32Log2PageSize,
		                                          ui32NumOfPages,
		                                          puiPhysicalOffset,
		                                          pbValid,
		                                          psDevAddrPtr);
		PVR_GOTO_IF_ERROR(eError, FreeOffsetArray);

#if defined(PVR_PMR_TRANSLATE_UMA_ADDRESSES)
		/* Currently excluded from the default build because of performance
		 * concerns.
		 * We do not need this part in all systems because the GPU has the same
		 * address view of system RAM as the CPU.
		 * Alternatively this could be implemented as part of the PMR-factories
		 * directly */
		if (PhysHeapGetType(psPMR->psPhysHeap) == PHYS_HEAP_TYPE_UMA ||
		    PhysHeapGetType(psPMR->psPhysHeap) == PHYS_HEAP_TYPE_DMA)
		{
			IMG_UINT32 i;
			IMG_DEV_PHYADDR sDevPAddrCorrected;

			/* Copy the translated addresses to the correct array */
			for (i = 0; i < ui32NumOfPages; i++)
			{
				PhysHeapCpuPAddrToDevPAddr(psPMR->psPhysHeap,
				                           1,
				                           &sDevPAddrCorrected,
				                           (IMG_CPU_PHYADDR *) &psDevAddrPtr[i]);
				psDevAddrPtr[i].uiAddr = sDevPAddrCorrected.uiAddr;
			}
		}
#endif
	}

FreeOffsetArray:
	if (puiPhysicalOffset != auiPhysicalOffset)
	{
		OSFreeMem(puiPhysicalOffset);
	}

	return eError;
}

/* must have called PMRLockSysPhysAddresses() before calling this! */
PVRSRV_ERROR
PMR_CpuPhysAddr(const PMR *psPMR,
                IMG_UINT32 ui32Log2PageSize,
                IMG_UINT32 ui32NumOfPages,
                IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                IMG_CPU_PHYADDR *psCpuAddrPtr,
                IMG_BOOL *pbValid)
{
	PVRSRV_ERROR eError;
	IMG_DEV_PHYADDR asDevPAddr[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEV_PHYADDR *psDevPAddr = asDevPAddr;

	if (ui32NumOfPages > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		psDevPAddr = OSAllocMem(ui32NumOfPages * sizeof(IMG_DEV_PHYADDR));
		PVR_GOTO_IF_NOMEM(psDevPAddr, eError, e0);
	}

	eError = PMR_DevPhysAddr(psPMR, ui32Log2PageSize, ui32NumOfPages,
	                         uiLogicalOffset, psDevPAddr, pbValid);
	PVR_GOTO_IF_ERROR(eError, e1);
	PhysHeapDevPAddrToCpuPAddr(psPMR->psPhysHeap, ui32NumOfPages, psCpuAddrPtr, psDevPAddr);

	if (ui32NumOfPages > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		OSFreeMem(psDevPAddr);
	}

	return PVRSRV_OK;
e1:
	if (psDevPAddr != asDevPAddr)
	{
		OSFreeMem(psDevPAddr);
	}
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR PMR_ChangeSparseMem(PMR *psPMR,
                                 IMG_UINT32 ui32AllocPageCount,
                                 IMG_UINT32 *pai32AllocIndices,
                                 IMG_UINT32 ui32FreePageCount,
                                 IMG_UINT32 *pai32FreeIndices,
                                 IMG_UINT32 uiSparseFlags)
{
	PVRSRV_ERROR eError;

	PMRLockPMR(psPMR);

	eError = PMR_ChangeSparseMemUnlocked(psPMR,
	                                     ui32AllocPageCount,
	                                     pai32AllocIndices,
	                                     ui32FreePageCount,
	                                     pai32FreeIndices,
	                                     uiSparseFlags);

	PMRUnlockPMR(psPMR);

	return eError;
}

PVRSRV_ERROR PMR_ChangeSparseMemUnlocked(PMR *psPMR,
                                         IMG_UINT32 ui32AllocPageCount,
                                         IMG_UINT32 *pai32AllocIndices,
                                         IMG_UINT32 ui32FreePageCount,
                                         IMG_UINT32 *pai32FreeIndices,
                                         IMG_UINT32 uiSparseFlags)
{
	PVRSRV_ERROR eError;
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	PMR_IMPL_ZOMBIEPAGES pvZombiePages = NULL;
	PMR_ZOMBIE_PAGES* psPMRZombiePages = NULL;
#endif

	if (PMR_IsMemLayoutFixed(psPMR) || PMR_IsCpuMapped(psPMR))
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: This PMR layout cannot be changed - PMR_IsMemLayoutFixed()=%c, _PMR_IsMapped()=%c",
				__func__,
				PMR_IsMemLayoutFixed(psPMR) ? 'Y' : 'n',
				PMR_IsCpuMapped(psPMR) ? 'Y' : 'n'));
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}

	if (NULL == psPMR->psFuncTab->pfnChangeSparseMem)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: This type of sparse PMR cannot be changed.",
				__func__));
		return PVRSRV_ERROR_NOT_IMPLEMENTED;
	}

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	if (uiSparseFlags & SPARSE_RESIZE_FREE)
	{
		/* Speculatively preallocate in order to simplify error handling later */
		psPMRZombiePages = OSAllocZMem(sizeof(PMR_ZOMBIE_PAGES));
		PVR_GOTO_IF_NOMEM(psPMRZombiePages, eError, e0);
	}
#endif

	eError = psPMR->psFuncTab->pfnChangeSparseMem(psPMR->pvFlavourData,
	                                              psPMR,
	                                              ui32AllocPageCount,
	                                              pai32AllocIndices,
	                                              ui32FreePageCount,
	                                              pai32FreeIndices,
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	                                              &pvZombiePages,
#endif
	                                              uiSparseFlags);
	if (eError != PVRSRV_OK)
	{
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
		if (eError == PVRSRV_ERROR_PMR_FAILED_TO_ALLOC_PAGES)
		{
			PVRSRVStatsUpdateOOMStats(PVRSRV_PROCESS_STAT_TYPE_OOM_PHYSMEM_COUNT,
						  OSGetCurrentClientProcessIDKM());
		}
#endif
		goto e1;
	}

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	if (pvZombiePages != NULL)
	{
		PVRSRV_DEV_POWER_STATE ePowerState;
		PVRSRV_DEVICE_NODE *psDevNode;

		psDevNode = PMR_DeviceNode(psPMR);
		eError = PVRSRVGetDevicePowerState(psDevNode, &ePowerState);
		if (eError != PVRSRV_OK)
		{
			/* Treat unknown power state as ON. */
			ePowerState = PVRSRV_DEV_POWER_STATE_ON;
		}

		if (ePowerState == PVRSRV_DEV_POWER_STATE_OFF)
		{
			/* Free preallocated psPMRZombiePages as these won't be used*/
			OSFreeMem(psPMRZombiePages);

			eError = psPMR->psFuncTab->pfnFreeZombiePages(pvZombiePages);
			PVR_LOG_GOTO_IF_ERROR(eError, "Error when trying to free zombies immediately.", e0);
		}
		else
		{
			PVR_ASSERT(psPMRZombiePages != NULL);
			psPMRZombiePages->sHeader.bIsZombiePages = IMG_TRUE;
			psPMRZombiePages->pfnFactoryFreeZombies = psPMR->psFuncTab->pfnFreeZombiePages;
			psPMRZombiePages->pvFactoryPages = pvZombiePages;

			OSLockAcquire(psDevNode->hPMRZombieListLock);
			dllist_add_to_tail(&psDevNode->sPMRZombieList, &psPMRZombiePages->sHeader.sZombieNode);
			psDevNode->uiPMRZombieCount++;
			OSLockRelease(psDevNode->hPMRZombieListLock);
		}
	}
	else
	{
		/* Free psPMRZombiePages as change sparse has not produced zombie pages */
		OSFreeMem(psPMRZombiePages);
	}
#endif

#if defined(PDUMP)
	{
		IMG_BOOL bInitialise = IMG_FALSE;
		IMG_UINT32 ui32InitValue = 0;

		if (PVRSRV_CHECK_ZERO_ON_ALLOC(PMR_Flags(psPMR)))
		{
			bInitialise = IMG_TRUE;
		}
		else if (PVRSRV_CHECK_POISON_ON_ALLOC(PMR_Flags(psPMR)))
		{
			ui32InitValue = 0xDEADBEEF;
			bInitialise = IMG_TRUE;
		}

		PDumpPMRChangeSparsePMR(psPMR,
		                        1 << psPMR->uiLog2ContiguityGuarantee,
		                        ui32AllocPageCount,
		                        pai32AllocIndices,
		                        ui32FreePageCount,
		                        pai32FreeIndices,
		                        bInitialise,
		                        ui32InitValue,
		                        &psPMR->hPDumpAllocHandle);
	}

#endif

	return PVRSRV_OK;
e1:
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	if (uiSparseFlags & SPARSE_RESIZE_FREE)
	{
		OSFreeMem(psPMRZombiePages);
	}
e0:
#endif
	return eError;
}

PVRSRV_ERROR PMR_ChangeSparseMemCPUMap(PMR *psPMR,
                                       IMG_UINT64 sCpuVAddrBase,
                                       IMG_UINT32 ui32AllocPageCount,
                                       IMG_UINT32 *pai32AllocIndices,
                                       IMG_UINT32 ui32FreePageCount,
                                       IMG_UINT32 *pai32FreeIndices)
{
	PVRSRV_ERROR eError;

	PMRLockPMR(psPMR);
	if ((NULL == psPMR->psFuncTab) ||
			(NULL == psPMR->psFuncTab->pfnChangeSparseMemCPUMap))
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: This type of sparse PMR cannot be changed.",
				__func__));
		PMRUnlockPMR(psPMR);
		return PVRSRV_ERROR_NOT_IMPLEMENTED;
	}

	if (PMR_IsMemLayoutFixed(psPMR) == IMG_TRUE)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: This PMR layout cannot be changed",
				__func__));
		PMRUnlockPMR(psPMR);
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}

	eError = psPMR->psFuncTab->pfnChangeSparseMemCPUMap(psPMR->pvFlavourData,
	                                                    psPMR,
	                                                    sCpuVAddrBase,
	                                                    ui32AllocPageCount,
	                                                    pai32AllocIndices,
	                                                    ui32FreePageCount,
	                                                    pai32FreeIndices);

	PMRUnlockPMR(psPMR);
	return eError;
}


#if defined(PDUMP)

static PVRSRV_ERROR
_PMR_PDumpSymbolicAddrPhysical(const PMR *psPMR,
                               IMG_DEVMEM_OFFSET_T uiPhysicalOffset,
                               IMG_UINT32 ui32MemspaceNameLen,
                               IMG_CHAR *pszMemspaceName,
                               IMG_UINT32 ui32SymbolicAddrLen,
                               IMG_CHAR *pszSymbolicAddr,
                               IMG_DEVMEM_OFFSET_T *puiNewOffset,
                               IMG_DEVMEM_OFFSET_T *puiNextSymName)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

#if defined(SUPPORT_SECURITY_VALIDATION)
	if (PVRSRV_CHECK_SECURE_FW_CODE(psPMR->uiFlags) ||
	    PVRSRV_CHECK_SECURE_FW_DATA(psPMR->uiFlags) ||
	    PVRSRV_CHECK_SECURE_BUFFER(psPMR->uiFlags)   )
	{
		PVRSRV_DEVICE_NODE *psDevNode = psPMR->psDevNode;

		eError = psDevNode->pfnGetSecurePDumpMemspace(psPMR->psDevNode,
		                                              psPMR->uiFlags,
		                                              pszMemspaceName,
		                                              ui32MemspaceNameLen);

		PVR_RETURN_IF_ERROR(eError);
	}
	else
#endif
	if (DevmemCPUCacheCoherency(psPMR->psDevNode, psPMR->uiFlags) &&
			DevmemDeviceCacheCoherency(psPMR->psDevNode, psPMR->uiFlags))
	{
		OSSNPrintf(pszMemspaceName,
		           ui32MemspaceNameLen,
		           PMR_MEMSPACE_CACHE_COHERENT_FMTSPEC,
		           psPMR->pszPDumpDefaultMemspaceName);
	}
	else
	{
		OSSNPrintf(pszMemspaceName, ui32MemspaceNameLen, PMR_MEMSPACE_FMTSPEC,
		           psPMR->pszPDumpDefaultMemspaceName);
	}

	OSSNPrintf(pszSymbolicAddr,
	           ui32SymbolicAddrLen,
	           PMR_SYMBOLICADDR_FMTSPEC,
	           PMR_DEFAULT_PREFIX,
	           psPMR->uiSerialNum,
	           uiPhysicalOffset >> PMR_GetLog2Contiguity(psPMR),
	           psPMR->szAnnotation);

	if (pszSymbolicAddr)
	{
		PDumpMakeStringValid(pszSymbolicAddr, OSStringLength(pszSymbolicAddr));
	}


	*puiNewOffset = uiPhysicalOffset & ((1 << PMR_GetLog2Contiguity(psPMR))-1);
	*puiNextSymName = (IMG_DEVMEM_OFFSET_T) (((uiPhysicalOffset >> PMR_GetLog2Contiguity(psPMR))+1)
			<< PMR_GetLog2Contiguity(psPMR));

	return eError;
}


PVRSRV_ERROR
PMR_PDumpSymbolicAddr(const PMR *psPMR,
                      IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                      IMG_UINT32 ui32MemspaceNameLen,
                      IMG_CHAR *pszMemspaceName,
                      IMG_UINT32 ui32SymbolicAddrLen,
                      IMG_CHAR *pszSymbolicAddr,
                      IMG_DEVMEM_OFFSET_T *puiNewOffset,
                      IMG_DEVMEM_OFFSET_T *puiNextSymName
)
{
	IMG_DEVMEM_OFFSET_T uiPhysicalOffset;
	IMG_UINT32 ui32Remain;
	IMG_BOOL bValid;
	PVRSRV_ERROR eError;

	PVR_ASSERT(uiLogicalOffset < psPMR->uiLogicalSize);

	eError = _PMRLogicalOffsetToPhysicalOffset(psPMR,
	                                           0,
	                                           1,
	                                           uiLogicalOffset,
	                                           &uiPhysicalOffset,
	                                           &ui32Remain,
	                                           &bValid);
	PVR_LOG_RETURN_IF_ERROR(eError, "_PMRLogicalOffsetToPhysicalOffset");

	if (!bValid)
	{
		/* For sparse allocations, for a given logical address, there
		 * may not be a physical memory backing, the virtual range can
		 * still be valid.
		 */
		uiPhysicalOffset = uiLogicalOffset;
	}

	return _PMR_PDumpSymbolicAddrPhysical(psPMR,
	                                      uiPhysicalOffset,
	                                      ui32MemspaceNameLen,
	                                      pszMemspaceName,
	                                      ui32SymbolicAddrLen,
	                                      pszSymbolicAddr,
	                                      puiNewOffset,
	                                      puiNextSymName);
}

/*!
 * @brief Writes a WRW command to the script2 buffer, representing a
 *        dword write to a physical allocation. Size is always
 *        sizeof(IMG_UINT32).
 * @param psPMR - PMR object representing allocation
 * @param uiLogicalOffset - offset
 * @param ui32Value - value to write
 * @param uiPDumpFlags - pdump flags
 * @return PVRSRV_ERROR
 */
PVRSRV_ERROR
PMRPDumpLoadMemValue32(PMR *psPMR,
                       IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                       IMG_UINT32 ui32Value,
                       PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpSymbolicOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 uiPMRPageSize = 1 << psPMR->uiLog2ContiguityGuarantee;

	PVR_ASSERT(uiLogicalOffset + sizeof(ui32Value) <= psPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiLogicalOffset & (uiPMRPageSize-1)) + sizeof(ui32Value))
			<= uiPMRPageSize));

	eError = PMRLockSysPhysAddresses(psPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Get the symbolic address of the PMR */
	eError = PMR_PDumpSymbolicAddr(psPMR,
	                               uiLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Write the WRW script command */
	eError = PDumpPMRWRW32(aszMemspaceName,
	                       aszSymbolicName,
	                       uiPDumpSymbolicOffset,
	                       ui32Value,
	                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);

	eError = PMRUnlockSysPhysAddresses(psPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	return PVRSRV_OK;
}

/*!
 * @brief Writes a RDW followed by a WRW command to the pdump script to perform
 *        an effective copy from memory to memory. Memory copied is of size
 *        sizeof(IMG_UINT32)
 *
 * @param psDstPMR - PMR object representing allocation of destination
 * @param uiDstLogicalOffset - destination offset
 * @param psSrcPMR - PMR object representing allocation of source
 * @param uiSrcLogicalOffset - source offset
 * @param pszTmpVar - pdump temporary variable used during the copy
 * @param uiPDumpFlags - pdump flags
 * @return PVRSRV_ERROR
 */
PVRSRV_ERROR
PMRPDumpCopyMem32(PMR *psDstPMR,
                  IMG_DEVMEM_OFFSET_T uiDstLogicalOffset,
                  PMR *psSrcPMR,
                  IMG_DEVMEM_OFFSET_T uiSrcLogicalOffset,
                  const IMG_CHAR *pszTmpVar,
                  PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpSymbolicOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	const IMG_UINT32 uiDstPMRPageSize = 1 << psDstPMR->uiLog2ContiguityGuarantee;
	const IMG_UINT32 uiSrcPMRPageSize = 1 << psSrcPMR->uiLog2ContiguityGuarantee;

	PVR_ASSERT(uiSrcLogicalOffset + sizeof(IMG_UINT32) <= psSrcPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiSrcLogicalOffset & (uiSrcPMRPageSize-1)) + sizeof(IMG_UINT32))
			<= uiSrcPMRPageSize));

	PVR_ASSERT(uiDstLogicalOffset + sizeof(IMG_UINT32) <= psDstPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiDstLogicalOffset & (uiDstPMRPageSize-1)) + sizeof(IMG_UINT32))
			<= uiDstPMRPageSize));


	eError = PMRLockSysPhysAddresses(psSrcPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Get the symbolic address of the source PMR */
	eError = PMR_PDumpSymbolicAddr(psSrcPMR,
	                               uiSrcLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Issue PDump read command */
	eError = PDumpPMRRDW32MemToInternalVar(pszTmpVar,
	                                       aszMemspaceName,
	                                       aszSymbolicName,
	                                       uiPDumpSymbolicOffset,
	                                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);

	eError = PMRUnlockSysPhysAddresses(psSrcPMR);
	PVR_ASSERT(eError == PVRSRV_OK);



	eError = PMRLockSysPhysAddresses(psDstPMR);
	PVR_ASSERT(eError == PVRSRV_OK);


	/* Get the symbolic address of the destination PMR */
	eError = PMR_PDumpSymbolicAddr(psDstPMR,
	                               uiDstLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);


	/* Write the WRW script command */
	eError = PDumpPMRWRW32InternalVarToMem(aszMemspaceName,
	                                       aszSymbolicName,
	                                       uiPDumpSymbolicOffset,
	                                       pszTmpVar,
	                                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);


	eError = PMRUnlockSysPhysAddresses(psDstPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	return PVRSRV_OK;
}

/*!
 * @brief Writes a WRW64 command to the script2 buffer, representing a
 *        dword write to a physical allocation. Size is always
 *        sizeof(IMG_UINT64).
 * @param psPMR - PMR object representing allocation
 * @param uiLogicalOffset - offset
 * @param ui64Value - value to write
 * @param uiPDumpFlags - pdump flags
 * @return PVRSRV_ERROR
 */
PVRSRV_ERROR
PMRPDumpLoadMemValue64(PMR *psPMR,
                       IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                       IMG_UINT64 ui64Value,
                       PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpSymbolicOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 uiPMRPageSize = 1 << psPMR->uiLog2ContiguityGuarantee;


	PVR_ASSERT(uiLogicalOffset + sizeof(ui64Value) <= psPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiLogicalOffset & (uiPMRPageSize-1)) + sizeof(ui64Value))
			<= uiPMRPageSize));

	eError = PMRLockSysPhysAddresses(psPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Get the symbolic address of the PMR */
	eError = PMR_PDumpSymbolicAddr(psPMR,
	                               uiLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Write the WRW script command */
	eError = PDumpPMRWRW64(aszMemspaceName,
	                       aszSymbolicName,
	                       uiPDumpSymbolicOffset,
	                       ui64Value,
	                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);

	eError = PMRUnlockSysPhysAddresses(psPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	return PVRSRV_OK;
}

/*!
 * @brief Writes a RDW64 followed by a WRW64 command to the pdump script to
 *        perform an effective copy from memory to memory. Memory copied is of
 *        size sizeof(IMG_UINT32)
 *
 * @param psDstPMR - PMR object representing allocation of destination
 * @param uiDstLogicalOffset - destination offset
 * @param psSrcPMR - PMR object representing allocation of source
 * @param uiSrcLogicalOffset - source offset
 * @param pszTmpVar - pdump temporary variable used during the copy
 * @param uiPDumpFlags - pdump flags
 * @return PVRSRV_ERROR
 */
PVRSRV_ERROR
PMRPDumpCopyMem64(PMR *psDstPMR,
                  IMG_DEVMEM_OFFSET_T uiDstLogicalOffset,
                  PMR *psSrcPMR,
                  IMG_DEVMEM_OFFSET_T uiSrcLogicalOffset,
                  const IMG_CHAR *pszTmpVar,
                  PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpSymbolicOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	const IMG_UINT32 uiDstPMRPageSize = 1 << psDstPMR->uiLog2ContiguityGuarantee;
	const IMG_UINT32 uiSrcPMRPageSize = 1 << psSrcPMR->uiLog2ContiguityGuarantee;

	PVR_ASSERT(uiSrcLogicalOffset + sizeof(IMG_UINT32) <= psSrcPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiSrcLogicalOffset & (uiSrcPMRPageSize-1)) + sizeof(IMG_UINT32))
			<= uiSrcPMRPageSize));

	PVR_ASSERT(uiDstLogicalOffset + sizeof(IMG_UINT32) <= psDstPMR->uiLogicalSize);
	/* Especially make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiDstLogicalOffset & (uiDstPMRPageSize-1)) + sizeof(IMG_UINT32))
			<= uiDstPMRPageSize));


	eError = PMRLockSysPhysAddresses(psSrcPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Get the symbolic address of the source PMR */
	eError = PMR_PDumpSymbolicAddr(psSrcPMR,
	                               uiSrcLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);

	/* Issue PDump read command */
	eError = PDumpPMRRDW64MemToInternalVar(pszTmpVar,
	                                       aszMemspaceName,
	                                       aszSymbolicName,
	                                       uiPDumpSymbolicOffset,
	                                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);

	eError = PMRUnlockSysPhysAddresses(psSrcPMR);
	PVR_ASSERT(eError == PVRSRV_OK);



	eError = PMRLockSysPhysAddresses(psDstPMR);
	PVR_ASSERT(eError == PVRSRV_OK);


	/* Get the symbolic address of the destination PMR */
	eError = PMR_PDumpSymbolicAddr(psDstPMR,
	                               uiDstLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpSymbolicOffset,
	                               &uiNextSymName);
	PVR_ASSERT(eError == PVRSRV_OK);


	/* Write the WRW script command */
	eError = PDumpPMRWRW64InternalVarToMem(aszMemspaceName,
	                                       aszSymbolicName,
	                                       uiPDumpSymbolicOffset,
	                                       pszTmpVar,
	                                       uiPDumpFlags);
	PVR_ASSERT(eError == PVRSRV_OK);


	eError = PMRUnlockSysPhysAddresses(psDstPMR);
	PVR_ASSERT(eError == PVRSRV_OK);

	return PVRSRV_OK;
}

/*!
 * @brief PDumps the contents of the given allocation.
 * If bZero is IMG_TRUE then the zero page in the parameter stream is used
 * as the source of data, rather than the allocation's actual backing.
 * @param psPMR - PMR object representing allocation
 * @param uiLogicalOffset - Offset to write at
 * @param uiSize - Number of bytes to write
 * @param uiPDumpFlags - PDump flags
 * @param bZero - Use the PDump zero page as the source
 * @return PVRSRV_ERROR
 */
PVRSRV_ERROR
PMRPDumpLoadMem(PMR *psPMR,
                IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                IMG_DEVMEM_SIZE_T uiSize,
                PDUMP_FLAGS_T uiPDumpFlags,
                IMG_BOOL bZero)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiOutOffset;
	IMG_DEVMEM_OFFSET_T uiCurrentOffset = uiLogicalOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName = 0;
	const IMG_CHAR *pszParamStreamFileName;
	PDUMP_FILEOFFSET_T uiParamStreamFileOffset;

	/* required when !bZero */
#define PMR_MAX_PDUMP_BUFSZ (1<<21)
	IMG_CHAR aszParamStreamFilename[PDUMP_PARAM_MAX_FILE_NAME];
	IMG_UINT8 *pcBuffer = NULL;
	size_t uiBufSz;
	IMG_BOOL bValid;
	IMG_DEVMEM_SIZE_T uiSizeRemain = uiSize;

	PVR_ASSERT(uiLogicalOffset + uiSize <= psPMR->uiLogicalSize);

	/* Check if pdump client is connected */
	if (!PDumpIsContCaptureOn())
	{
		/* Dumping of memory in Pdump buffer will be rejected for no client connected case.
		 * So return early and save reading of data from PMR. */
		return PVRSRV_OK;
	}

	/* Get the correct PDump stream file name */
	if (bZero)
	{
		PDumpCommentWithFlags(uiPDumpFlags,
		                      "Zeroing allocation (" IMG_DEVMEM_SIZE_FMTSPEC " bytes)",
		                      uiSize);

		/* get the zero page information. it is constant for this function */
		PDumpGetParameterZeroPageInfo(&uiParamStreamFileOffset,
		                              &uiBufSz,
		                              &pszParamStreamFileName);
	}
	else
	{

		uiBufSz = 1 << PMR_GetLog2Contiguity(psPMR);
		PVR_ASSERT((1 << PMR_GetLog2Contiguity(psPMR)) <= PMR_MAX_PDUMP_BUFSZ);

		pcBuffer = OSAllocMem(uiBufSz);

		PVR_LOG_RETURN_IF_NOMEM(pcBuffer, "OSAllocMem");

		eError = PMRLockSysPhysAddresses(psPMR);
		PVR_ASSERT(eError == PVRSRV_OK);

		pszParamStreamFileName = aszParamStreamFilename;
	}

	/* Loop over all touched symbolic addresses of the PMR and
	 * emit LDBs to load the contents. */
	while (uiCurrentOffset < (uiLogicalOffset + uiSize))
	{
		/* Get the correct symbolic name for the current offset */
		eError = PMR_PDumpSymbolicAddr(psPMR,
		                               uiCurrentOffset,
		                               sizeof(aszMemspaceName),
		                               &aszMemspaceName[0],
		                               sizeof(aszSymbolicName),
		                               &aszSymbolicName[0],
		                               &uiOutOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);
		PVR_ASSERT((uiNextSymName - uiCurrentOffset) <= uiBufSz);

		PMR_IsOffsetValid(psPMR,
		                  0,
		                  1,
		                  uiCurrentOffset,
		                  &bValid);

		/* Either just LDB the zeros or read from the PMR and store that
		 * in the pdump stream */
		if (bValid)
		{
			size_t uiNumBytes;

			if (bZero)
			{
				uiNumBytes = MIN(uiSizeRemain, uiNextSymName - uiCurrentOffset);
			}
			else
			{
				IMG_DEVMEM_OFFSET_T uiReadOffset;
				uiReadOffset = ((uiNextSymName > (uiLogicalOffset + uiSize)) ?
						uiLogicalOffset + uiSize - uiCurrentOffset :
						uiNextSymName - uiCurrentOffset);

				eError = PMR_ReadBytes(psPMR,
				                       uiCurrentOffset,
				                       pcBuffer,
				                       uiReadOffset,
				                       &uiNumBytes);
				PVR_ASSERT(eError == PVRSRV_OK);

				eError = PDumpWriteParameterBlob(pcBuffer,
				                          uiNumBytes,
				                          uiPDumpFlags,
				                          &aszParamStreamFilename[0],
				                          sizeof(aszParamStreamFilename),
				                          &uiParamStreamFileOffset);
				if (eError == PVRSRV_ERROR_PDUMP_NOT_ALLOWED)
				{
					/* Write to parameter file prevented under the flags and
					 * current state of the driver so skip further writes.
					 */
					eError = PVRSRV_OK;
				}
				else if (eError != PVRSRV_OK)
				{
					PDUMP_ERROR(eError, "Failed to write PMR memory to parameter file");
				}
			}

			/* Emit the LDB command to the current symbolic address */
			eError = PDumpPMRLDB(aszMemspaceName,
			                     aszSymbolicName,
			                     uiOutOffset,
			                     uiNumBytes,
			                     pszParamStreamFileName,
			                     uiParamStreamFileOffset,
			                     uiPDumpFlags);
			uiSizeRemain = uiSizeRemain - uiNumBytes;
		}
		uiCurrentOffset = uiNextSymName;
	}

	if (!bZero)
	{
		eError = PMRUnlockSysPhysAddresses(psPMR);
		PVR_ASSERT(eError == PVRSRV_OK);

		OSFreeMem(pcBuffer);
	}

	return PVRSRV_OK;
}



PVRSRV_ERROR
PMRPDumpSaveToFile(const PMR *psPMR,
                   IMG_DEVMEM_OFFSET_T uiLogicalOffset,
                   IMG_DEVMEM_SIZE_T uiSize,
                   IMG_UINT32 uiArraySize,
                   const IMG_CHAR *pszFilename,
                   IMG_UINT32 uiFileOffset)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiOutOffset;
	IMG_DEVMEM_OFFSET_T uiCurrentOffset = uiLogicalOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName = 0;
	IMG_UINT32 uiCurrentFileOffset = uiFileOffset;

	PVR_UNREFERENCED_PARAMETER(uiArraySize);

	PVR_ASSERT(uiLogicalOffset + uiSize <= psPMR->uiLogicalSize);

	while (uiCurrentOffset < (uiLogicalOffset + uiSize))
	{
		IMG_DEVMEM_OFFSET_T uiReadOffset;

		eError = PMR_PDumpSymbolicAddr(psPMR,
		                               uiCurrentOffset,
		                               sizeof(aszMemspaceName),
		                               &aszMemspaceName[0],
		                               sizeof(aszSymbolicName),
		                               &aszSymbolicName[0],
		                               &uiOutOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);
		PVR_ASSERT(uiNextSymName <= psPMR->uiLogicalSize);

		uiReadOffset = ((uiNextSymName > (uiLogicalOffset + uiSize)) ?
				uiLogicalOffset + uiSize - uiCurrentOffset :
				uiNextSymName - uiCurrentOffset);

		eError = PDumpPMRSAB(aszMemspaceName,
		                     aszSymbolicName,
		                     uiOutOffset,
		                     uiReadOffset,
		                     pszFilename,
		                     uiCurrentFileOffset);
		PVR_ASSERT(eError == PVRSRV_OK);

		uiCurrentFileOffset += uiNextSymName - uiCurrentOffset;
		uiCurrentOffset = uiNextSymName;
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRPDumpPol32(const PMR *psPMR,
              IMG_DEVMEM_OFFSET_T uiLogicalOffset,
              IMG_UINT32 ui32Value,
              IMG_UINT32 ui32Mask,
              PDUMP_POLL_OPERATOR eOperator,
              PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 uiPMRPageSize = 1 << psPMR->uiLog2ContiguityGuarantee;

	/* Make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiLogicalOffset & (uiPMRPageSize-1)) + sizeof(ui32Value))
			<= uiPMRPageSize));

	eError = PMR_PDumpSymbolicAddr(psPMR,
	                               uiLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpOffset,
	                               &uiNextSymName);
	PVR_GOTO_IF_ERROR(eError, e0);

#define _MEMPOLL_DELAY		(1000)
#define _MEMPOLL_COUNT		(2000000000 / _MEMPOLL_DELAY)

	eError = PDumpPMRPOL(aszMemspaceName,
	                     aszSymbolicName,
	                     uiPDumpOffset,
	                     ui32Value,
	                     ui32Mask,
	                     eOperator,
	                     _MEMPOLL_COUNT,
	                     _MEMPOLL_DELAY,
	                     uiPDumpFlags);
	PVR_GOTO_IF_ERROR(eError, e0);

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRPDumpCheck32(const PMR *psPMR,
				IMG_DEVMEM_OFFSET_T uiLogicalOffset,
				IMG_UINT32 ui32Value,
				IMG_UINT32 ui32Mask,
				PDUMP_POLL_OPERATOR eOperator,
				PDUMP_FLAGS_T uiPDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 uiPMRPageSize = 1 << psPMR->uiLog2ContiguityGuarantee;

	/* Make sure to not cross a block boundary */
	PVR_ASSERT(( ((uiLogicalOffset & (uiPMRPageSize-1)) + sizeof(ui32Value))
	           < uiPMRPageSize));

	eError = PMR_PDumpSymbolicAddr(psPMR,
	                               uiLogicalOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpOffset,
	                               &uiNextSymName);
	if (eError != PVRSRV_OK)
	{
		goto e0;
	}

	eError = PDumpPMRPOL(aszMemspaceName,
	                     aszSymbolicName,
	                     uiPDumpOffset,
	                     ui32Value,
	                     ui32Mask,
	                     eOperator,
	                     1,
	                     1,
	                     uiPDumpFlags);
	if (eError != PVRSRV_OK)
	{
		goto e0;
	}

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRPDumpCBP(const PMR *psPMR,
            IMG_DEVMEM_OFFSET_T uiReadOffset,
            IMG_DEVMEM_OFFSET_T uiWriteOffset,
            IMG_DEVMEM_SIZE_T uiPacketSize,
            IMG_DEVMEM_SIZE_T uiBufferSize)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPDumpOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;

	eError = PMR_PDumpSymbolicAddr(psPMR,
	                               uiReadOffset,
	                               sizeof(aszMemspaceName),
	                               &aszMemspaceName[0],
	                               sizeof(aszSymbolicName),
	                               &aszSymbolicName[0],
	                               &uiPDumpOffset,
	                               &uiNextSymName);
	PVR_GOTO_IF_ERROR(eError, e0);

	eError = PDumpPMRCBP(aszMemspaceName,
	                     aszSymbolicName,
	                     uiPDumpOffset,
	                     uiWriteOffset,
	                     uiPacketSize,
	                     uiBufferSize);
	PVR_GOTO_IF_ERROR(eError, e0);

	return PVRSRV_OK;

	/* Error exit paths follow */
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

void
PDumpPMRChangeSparsePMR(PMR *psPMR,
                        IMG_UINT32 uiBlockSize,
                        IMG_UINT32 ui32AllocPageCount,
                        IMG_UINT32 *pai32AllocIndices,
                        IMG_UINT32 ui32FreePageCount,
                        IMG_UINT32 *pai32FreeIndices,
                        IMG_BOOL bInitialise,
                        IMG_UINT32 ui32InitValue,
                        IMG_HANDLE *phPDumpAllocInfoOut)
{
	PVRSRV_ERROR eError;
	IMG_HANDLE *phPDumpAllocInfo = (IMG_HANDLE*) psPMR->hPDumpAllocHandle;

	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 i, uiIndex;

	/* Remove pages from the PMR */
	for (i = 0; i < ui32FreePageCount; i++)
	{
		uiIndex = pai32FreeIndices[i];

		eError = PDumpFree(phPDumpAllocInfo[uiIndex]);
		PVR_ASSERT(eError == PVRSRV_OK);
		phPDumpAllocInfo[uiIndex] = NULL;
	}

	/* Add new pages to the PMR */
	for (i = 0; i < ui32AllocPageCount; i++)
	{
		uiIndex = pai32AllocIndices[i];

		PVR_ASSERT(phPDumpAllocInfo[uiIndex] == NULL);

		eError = PMR_PDumpSymbolicAddr(psPMR,
		                               uiIndex * uiBlockSize,
		                               sizeof(aszMemspaceName),
		                               &aszMemspaceName[0],
		                               sizeof(aszSymbolicName),
		                               &aszSymbolicName[0],
		                               &uiOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);

		eError = PDumpMalloc(aszMemspaceName,
		                     aszSymbolicName,
		                     uiBlockSize,
		                     uiBlockSize,
		                     bInitialise,
		                     ui32InitValue,
		                     &phPDumpAllocInfo[uiIndex],
		                     PDUMP_NONE);
		PVR_ASSERT(eError == PVRSRV_OK);
	}

	/* (IMG_HANDLE) <- (IMG_HANDLE*) */
	*phPDumpAllocInfoOut = (IMG_HANDLE) phPDumpAllocInfo;
}

void
PDumpPMRFreePMR(PMR *psPMR,
                IMG_DEVMEM_SIZE_T uiSize,
                IMG_DEVMEM_ALIGN_T uiBlockSize,
                IMG_UINT32 uiLog2Contiguity,
                IMG_HANDLE hPDumpAllocationInfoHandle)
{
	PVRSRV_ERROR eError;
	IMG_UINT32 i;

	/* (IMG_HANDLE*) <- (IMG_HANDLE) */
	IMG_HANDLE *ahPDumpAllocHandleArray = (IMG_HANDLE*) hPDumpAllocationInfoHandle;

	for (i = 0; i < psPMR->uiNumPDumpBlocks; i++)
	{
		if (ahPDumpAllocHandleArray[i] != NULL)
		{
			eError = PDumpFree(ahPDumpAllocHandleArray[i]);
			PVR_ASSERT(eError == PVRSRV_OK);
			ahPDumpAllocHandleArray[i] = NULL;
		}
	}

	OSFreeMem(ahPDumpAllocHandleArray);
}


void
PDumpPMRMallocPMR(PMR *psPMR,
                  IMG_DEVMEM_SIZE_T uiSize,
                  IMG_DEVMEM_ALIGN_T uiBlockSize,
                  IMG_UINT32 ui32ChunkSize,
                  IMG_UINT32 ui32NumPhysChunks,
                  IMG_UINT32 ui32NumVirtChunks,
                  IMG_UINT32 *puiMappingTable,
                  IMG_UINT32 uiLog2Contiguity,
                  IMG_BOOL bInitialise,
                  IMG_UINT32 ui32InitValue,
                  IMG_HANDLE *phPDumpAllocInfoOut,
                  IMG_UINT32 ui32PDumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_HANDLE *phPDumpAllocInfo;

	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
	IMG_UINT32 uiNumPhysBlocks;
	IMG_UINT32 uiNumVirtBlocks;
	IMG_UINT32 i, uiIndex;

	if (PMR_IsSparse(psPMR))
	{
		uiNumPhysBlocks = (ui32ChunkSize * ui32NumPhysChunks) >> uiLog2Contiguity;
		/* Make sure we did not cut off anything */
		PVR_ASSERT(uiNumPhysBlocks << uiLog2Contiguity == (ui32ChunkSize * ui32NumPhysChunks));
	}
	else
	{
		uiNumPhysBlocks = uiSize >> uiLog2Contiguity;
		/* Make sure we did not cut off anything */
		PVR_ASSERT(uiNumPhysBlocks << uiLog2Contiguity == uiSize);
	}

	uiNumVirtBlocks = uiSize >> uiLog2Contiguity;
	PVR_ASSERT(uiNumVirtBlocks << uiLog2Contiguity == uiSize);

	psPMR->uiNumPDumpBlocks = uiNumVirtBlocks;

	phPDumpAllocInfo = (IMG_HANDLE*) OSAllocZMem(uiNumVirtBlocks * sizeof(IMG_HANDLE));


	for (i = 0; i < uiNumPhysBlocks; i++)
	{
		uiIndex = PMR_IsSparse(psPMR) ? puiMappingTable[i] : i;

		eError = PMR_PDumpSymbolicAddr(psPMR,
		                               uiIndex * uiBlockSize,
		                               sizeof(aszMemspaceName),
		                               &aszMemspaceName[0],
		                               sizeof(aszSymbolicName),
		                               &aszSymbolicName[0],
		                               &uiOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);

		eError = PDumpMalloc(aszMemspaceName,
		                     aszSymbolicName,
		                     uiBlockSize,
		                     uiBlockSize,
		                     bInitialise,
		                     ui32InitValue,
		                     &phPDumpAllocInfo[uiIndex],
		                     ui32PDumpFlags);
		PVR_ASSERT(eError == PVRSRV_OK);
	}

	/* (IMG_HANDLE) <- (IMG_HANDLE*) */
	*phPDumpAllocInfoOut = (IMG_HANDLE) phPDumpAllocInfo;

}
#endif	/* PDUMP */


void *PMRGetPrivateData(const PMR *psPMR,
                        const PMR_IMPL_FUNCTAB *psFuncTab)
{
	return (psFuncTab == psPMR->psFuncTab) ? psPMR->pvFlavourData : NULL;
}

#define PMR_PM_WORD_SIZE 4

PVRSRV_ERROR
PMRWritePMPageList(/* Target PMR, offset, and length */
		PMR *psPageListPMR,
		IMG_DEVMEM_OFFSET_T uiTableOffset,
		IMG_DEVMEM_SIZE_T  uiTableLength,
		/* Referenced PMR, and "page" granularity */
		PMR *psReferencePMR,
		IMG_DEVMEM_LOG2ALIGN_T uiLog2PageSize,
		PMR_PAGELIST **ppsPageList)
{
	PVRSRV_ERROR eError;
	IMG_DEVMEM_SIZE_T uiWordSize;
	IMG_UINT32 uiNumPages;
	IMG_UINT32 uiPageIndex;
	PMR_FLAGS_T uiFlags = psPageListPMR->uiFlags;
	PMR_PAGELIST *psPageList;
#if defined(PDUMP)
	IMG_CHAR aszTableEntryMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszTableEntrySymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiTableEntryPDumpOffset;
	IMG_CHAR aszPageMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszPageSymbolicName[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiPagePDumpOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;
#endif
#if !defined(NO_HARDWARE)
	IMG_UINT32 uiPageListPageSize = 1 << psPageListPMR->uiLog2ContiguityGuarantee;
	IMG_UINT64 uiPageListPMRPage = 0;
	IMG_UINT64 uiPrevPageListPMRPage = 0;
	IMG_HANDLE hPrivData = NULL;
	void *pvKernAddr = NULL;
	IMG_UINT32 *pui32DataPtr = NULL;
	IMG_DEV_PHYADDR asDevPAddr[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_BOOL abValid[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEV_PHYADDR *pasDevAddrPtr;
	IMG_BOOL *pbPageIsValid;
#endif

	uiWordSize = PMR_PM_WORD_SIZE;

	/* check we're being asked to write the same number of 4-byte units as there are pages */
	uiNumPages = (IMG_UINT32)(psReferencePMR->uiLogicalSize >> uiLog2PageSize);

	if ((PMR_SIZE_T)uiNumPages << uiLog2PageSize != psReferencePMR->uiLogicalSize)
	{
		/* Strictly speaking, it's possible to provoke this error in two ways:
			(i) if it's not a whole multiple of the page size; or
			(ii) if there are more than 4 billion pages.
			The latter is unlikely. :) but the check is required in order to justify the cast.
		 */
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_NOT_PAGE_MULTIPLE, return_error);
	}
	uiWordSize = (IMG_UINT32)uiTableLength / uiNumPages;
	if (uiNumPages * uiWordSize != uiTableLength)
	{
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_NOT_PAGE_MULTIPLE, return_error);
	}

	/* Check for integer overflow */
	PVR_GOTO_IF_INVALID_PARAM(uiTableOffset + uiTableLength > uiTableOffset, eError, return_error);
	/* Check we're not being asked to write off the end of the PMR */
	PVR_GOTO_IF_INVALID_PARAM(uiTableOffset + uiTableLength <= psPageListPMR->uiLogicalSize, eError, return_error);

	/* the PMR into which we are writing must not be user CPU mappable: */
	if (PVRSRV_CHECK_CPU_READABLE(uiFlags) || PVRSRV_CHECK_CPU_WRITEABLE(uiFlags))
	{
		PVR_DPF((PVR_DBG_ERROR,
		         "Masked flags = 0x%" PVRSRV_MEMALLOCFLAGS_FMTSPEC,
		         (uiFlags & (PVRSRV_MEMALLOCFLAG_CPU_READABLE | PVRSRV_MEMALLOCFLAG_CPU_WRITEABLE))));
		PVR_DPF((PVR_DBG_ERROR,
		         "Page list PMR allows CPU mapping (0x%" PVRSRV_MEMALLOCFLAGS_FMTSPEC ")",
		         uiFlags));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_DEVICEMEM_INVALID_PMR_FLAGS, return_error);
	}

	/* the PMR into which we are writing must not be user CPU cacheable: */
	if (PVRSRV_CHECK_CPU_CACHE_INCOHERENT(uiFlags) ||
		PVRSRV_CHECK_CPU_CACHE_COHERENT(uiFlags) ||
		PVRSRV_CHECK_CPU_CACHED(uiFlags))
	{
		PVR_DPF((PVR_DBG_ERROR,
		         "Masked flags = 0x%" PVRSRV_MEMALLOCFLAGS_FMTSPEC,
		         (PMR_FLAGS_T)(uiFlags &  PVRSRV_MEMALLOCFLAG_CPU_CACHE_MODE_MASK)));
		PVR_DPF((PVR_DBG_ERROR,
		         "Page list PMR allows CPU caching (0x%" PVRSRV_MEMALLOCFLAGS_FMTSPEC ")",
		         uiFlags));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_DEVICEMEM_INVALID_PMR_FLAGS, return_error);
	}

	if (_PMRIsSparse(psPageListPMR))
	{
		PVR_LOG_GOTO_WITH_ERROR("psPageListPMR", eError, PVRSRV_ERROR_INVALID_PARAMS, return_error);
	}

	if (_PMRIsSparse(psReferencePMR))
	{
		PVR_LOG_GOTO_WITH_ERROR("psReferencePMR", eError, PVRSRV_ERROR_INVALID_PARAMS, return_error);
	}

	psPageList = OSAllocMem(sizeof(PMR_PAGELIST));
	PVR_LOG_GOTO_IF_NOMEM(psPageList, eError, return_error);

	psPageList->psReferencePMR = psReferencePMR;

	/* Need to lock down the physical addresses of the reference PMR */
	/* N.B.  This also checks that the requested "contiguity" is achievable */
	eError = PMRLockSysPhysAddresses(psReferencePMR);
	PVR_GOTO_IF_ERROR(eError, free_page_list);

#if !defined(NO_HARDWARE)
	if (uiNumPages > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		pasDevAddrPtr = OSAllocMem(uiNumPages * sizeof(IMG_DEV_PHYADDR));
		PVR_LOG_GOTO_IF_NOMEM(pasDevAddrPtr, eError, unlock_phys_addrs);

		pbPageIsValid = OSAllocMem(uiNumPages * sizeof(IMG_BOOL));
		PVR_LOG_GOTO_IF_NOMEM(pbPageIsValid, eError, free_devaddr_array);
	}
	else
	{
		pasDevAddrPtr = asDevPAddr;
		pbPageIsValid = abValid;
	}

	eError = PMR_DevPhysAddr(psReferencePMR, uiLog2PageSize, uiNumPages, 0,
	                         pasDevAddrPtr, pbPageIsValid);
	PVR_LOG_GOTO_IF_ERROR(eError, "PMR_DevPhysAddr", free_valid_array);
#endif

	for (uiPageIndex = 0; uiPageIndex < uiNumPages; uiPageIndex++)
	{
		IMG_DEVMEM_OFFSET_T uiPMROffset = uiTableOffset + (uiWordSize * uiPageIndex);

#if defined(PDUMP)
		eError = PMR_PDumpSymbolicAddr(psPageListPMR,
		                               uiPMROffset,
		                               sizeof(aszTableEntryMemspaceName),
		                               &aszTableEntryMemspaceName[0],
		                               sizeof(aszTableEntrySymbolicName),
		                               &aszTableEntrySymbolicName[0],
		                               &uiTableEntryPDumpOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);

		eError = PMR_PDumpSymbolicAddr(psReferencePMR,
		                               (IMG_DEVMEM_OFFSET_T)uiPageIndex << uiLog2PageSize,
		                               sizeof(aszPageMemspaceName),
		                               &aszPageMemspaceName[0],
		                               sizeof(aszPageSymbolicName),
		                               &aszPageSymbolicName[0],
		                               &uiPagePDumpOffset,
		                               &uiNextSymName);
		PVR_ASSERT(eError == PVRSRV_OK);

		eError = PDumpWriteShiftedMaskedValue(/* destination */
		                                      aszTableEntryMemspaceName,
		                                      aszTableEntrySymbolicName,
		                                      uiTableEntryPDumpOffset,
		                                      /* source */
		                                      aszPageMemspaceName,
		                                      aszPageSymbolicName,
		                                      uiPagePDumpOffset,
		                                      /* shift right */
		                                      uiLog2PageSize,
		                                      /* shift left */
		                                      0,
		                                      /* mask */
		                                      0xffffffff,
		                                      /* word size */
		                                      uiWordSize,
		                                      /* flags */
		                                      PDUMP_FLAGS_CONTINUOUS);
		PVR_ASSERT(eError == PVRSRV_OK);
#else
		PVR_UNREFERENCED_PARAMETER(uiPMROffset);
#endif

#if !defined(NO_HARDWARE)

		/*
			We check for sparse PMR's at function entry, but as we can,
			check that every page is valid
		 */
		PVR_ASSERT(pbPageIsValid[uiPageIndex]);
		PVR_ASSERT(pasDevAddrPtr[uiPageIndex].uiAddr != 0);
		PVR_ASSERT(((pasDevAddrPtr[uiPageIndex].uiAddr >> uiLog2PageSize) & 0xFFFFFFFF00000000ll) == 0);

		uiPageListPMRPage = uiPMROffset >> psReferencePMR->uiLog2ContiguityGuarantee;

		if ((pui32DataPtr == NULL) || (uiPageListPMRPage != uiPrevPageListPMRPage))
		{
			size_t uiMappingOffset = uiPMROffset & (~(uiPageListPageSize - 1));
			size_t uiMappedSize;

			/* If we already had a page list mapped, we need to unmap it... */
			if (pui32DataPtr != NULL)
			{
				PMRReleaseKernelMappingData(psPageListPMR, hPrivData);
			}

			eError = PMRAcquireKernelMappingData(psPageListPMR,
			                                     uiMappingOffset,
			                                     uiPageListPageSize,
			                                     &pvKernAddr,
			                                     &uiMappedSize,
			                                     &hPrivData);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "Error mapping page list PMR page (%" IMG_UINT64_FMTSPEC ") into kernel (%d)",
						uiPageListPMRPage, eError));
				goto free_valid_array;
			}

			uiPrevPageListPMRPage = uiPageListPMRPage;
			PVR_ASSERT(uiMappedSize >= uiPageListPageSize);
			PVR_ASSERT(pvKernAddr != NULL);

			pui32DataPtr = IMG_OFFSET_ADDR(pvKernAddr, (uiPMROffset & (uiPageListPageSize - 1)));
		}

		PVR_ASSERT(((pasDevAddrPtr[uiPageIndex].uiAddr >> uiLog2PageSize) & 0xFFFFFFFF00000000ll) == 0);

		/* Write the physical page index into the page list PMR */
		*pui32DataPtr++ = TRUNCATE_64BITS_TO_32BITS(pasDevAddrPtr[uiPageIndex].uiAddr >> uiLog2PageSize);

		/* Last page so unmap */
		if (uiPageIndex == (uiNumPages - 1))
		{
			PMRReleaseKernelMappingData(psPageListPMR, hPrivData);
		}
#endif
	}

#if !defined(NO_HARDWARE)
	if (pasDevAddrPtr != asDevPAddr)
	{
		OSFreeMem(pbPageIsValid);
		OSFreeMem(pasDevAddrPtr);
	}
#endif
	*ppsPageList = psPageList;
	return PVRSRV_OK;

	/* Error exit paths follow */
#if !defined(NO_HARDWARE)

free_valid_array:
	if (pbPageIsValid != abValid)
	{
		OSFreeMem(pbPageIsValid);
	}

free_devaddr_array:
	if (pasDevAddrPtr != asDevPAddr)
	{
		OSFreeMem(pasDevAddrPtr);
	}

unlock_phys_addrs:
	PMRUnlockSysPhysAddresses(psReferencePMR);
#endif

free_page_list:
	OSFreeMem(psPageList);

return_error:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}


PVRSRV_ERROR
PMRUnwritePMPageList(PMR_PAGELIST *psPageList)
{
	PVRSRV_ERROR eError;

	eError = PMRUnlockSysPhysAddresses(psPageList->psReferencePMR);
	PVR_ASSERT(eError == PVRSRV_OK);
	OSFreeMem(psPageList);

	return PVRSRV_OK;
}

PVRSRV_ERROR
PMRZeroingPMR(PMR *psPMR,
              IMG_DEVMEM_LOG2ALIGN_T uiLog2PageSize)
{
	IMG_UINT32 uiNumPages;
	IMG_UINT32 uiPageIndex;
	IMG_UINT32 ui32PageSize = 1 << uiLog2PageSize;
	IMG_HANDLE hPrivData = NULL;
	void *pvKernAddr = NULL;
	PVRSRV_ERROR eError = PVRSRV_OK;
	size_t uiMappedSize;

	PVR_ASSERT(psPMR);

	/* Calculate number of pages in this PMR */
	uiNumPages = (IMG_UINT32)(psPMR->uiLogicalSize >> uiLog2PageSize);

	/* Verify the logical Size is a multiple or the physical page size */
	if ((PMR_SIZE_T)uiNumPages << uiLog2PageSize != psPMR->uiLogicalSize)
	{
		PVR_DPF((PVR_DBG_ERROR,
		         "%s: PMR is not a multiple of %u",
		         __func__,
		         ui32PageSize));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_NOT_PAGE_MULTIPLE, MultiPage_Error);
	}

	if (_PMRIsSparse(psPMR))
	{
		PVR_LOG_GOTO_WITH_ERROR("psPMR", eError, PVRSRV_ERROR_INVALID_PARAMS, Sparse_Error);
	}

	/* Scan through all pages of the PMR */
	for (uiPageIndex = 0; uiPageIndex < uiNumPages; uiPageIndex++)
	{
		/* map the physical page (for a given PMR offset) into kernel space */
		eError = PMRAcquireKernelMappingData(psPMR,
		                                     (size_t)uiPageIndex << uiLog2PageSize,
		                                     ui32PageSize,
		                                     &pvKernAddr,
		                                     &uiMappedSize,
		                                     &hPrivData);
		PVR_LOG_GOTO_IF_ERROR(eError, "PMRAcquireKernelMappingData", AcquireKernelMapping_Error);

		/* ensure the mapped page size is the same as the physical page size */
		if (uiMappedSize != ui32PageSize)
		{
			PVR_DPF((PVR_DBG_ERROR,
			         "%s: Physical Page size = 0x%08x, Size of Mapping = 0x%016" IMG_UINT64_FMTSPECx,
			         __func__,
			         ui32PageSize,
			         (IMG_UINT64)uiMappedSize));
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_INVALID_PARAMS, MappingSize_Error);
		}

		/* Use the conservative 'DeviceMemSet' here because we can't know
		 * if this PMR will be mapped cached.
		 */
		OSDeviceMemSet(pvKernAddr, 0, ui32PageSize);

		/* release mapping */
		PMRReleaseKernelMappingData(psPMR, hPrivData);

	}

	PVR_DPF((PVR_DBG_MESSAGE,
	         "%s: Zeroing PMR %p done (num pages %u, page size %u)",
	         __func__,
	         psPMR,
	         uiNumPages,
	         ui32PageSize));

	return PVRSRV_OK;


	/* Error handling */

MappingSize_Error:
	PMRReleaseKernelMappingData(psPMR, hPrivData);

AcquireKernelMapping_Error:
Sparse_Error:
MultiPage_Error:

	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRDumpPageList(PMR *psPMR,
                IMG_DEVMEM_LOG2ALIGN_T uiLog2PageSize)
{
	IMG_DEV_PHYADDR sDevAddrPtr;
	IMG_UINT32 uiNumPages;
	IMG_UINT32 uiPageIndex;
	IMG_BOOL bPageIsValid;
	IMG_UINT32 ui32Col = 16;
	IMG_UINT32 ui32SizePerCol = 11;
	IMG_UINT32 ui32ByteCount = 0;
	IMG_CHAR pszBuffer[16 /* ui32Col */ * 11 /* ui32SizePerCol */ + 1];
	PVRSRV_ERROR eError = PVRSRV_OK;

	/* Get number of pages */
	uiNumPages = (IMG_UINT32)(psPMR->uiLogicalSize >> uiLog2PageSize);

	/* Verify the logical Size is a multiple or the physical page size */
	if ((PMR_SIZE_T)uiNumPages << uiLog2PageSize != psPMR->uiLogicalSize)
	{
		PVR_DPF((PVR_DBG_ERROR, "PMRPrintPageList: PMR is not a multiple of %u", 1 << uiLog2PageSize));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_NOT_PAGE_MULTIPLE, MultiPage_Error);
	}

	if (_PMRIsSparse(psPMR))
	{
		PVR_LOG_GOTO_WITH_ERROR("psPMR", eError, PVRSRV_ERROR_INVALID_PARAMS, Sparse_Error);
	}

	PVR_LOG(("    PMR %p, Number of pages %u, Log2PageSize %d", psPMR, uiNumPages, uiLog2PageSize));

	/* Print the address of the physical pages */
	for (uiPageIndex = 0; uiPageIndex < uiNumPages; uiPageIndex++)
	{
		/* Get Device physical Address */
		eError = PMR_DevPhysAddr(psPMR,
		                         uiLog2PageSize,
		                         1,
		                         (IMG_DEVMEM_OFFSET_T)uiPageIndex << uiLog2PageSize,
		                         &sDevAddrPtr,
		                         &bPageIsValid);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "PMRPrintPageList: PMR %p failed to get DevPhysAddr with error %u",
					psPMR,
					eError));
			goto DevPhysAddr_Error;
		}

		ui32ByteCount += OSSNPrintf(pszBuffer + ui32ByteCount, ui32SizePerCol + 1, "%08x ", (IMG_UINT32)(sDevAddrPtr.uiAddr >> uiLog2PageSize));
		PVR_ASSERT(ui32ByteCount < ui32Col * ui32SizePerCol);

		if (uiPageIndex % ui32Col == ui32Col-1)
		{
			PVR_LOG(("      Phys Page: %s", pszBuffer));
			ui32ByteCount = 0;
		}
	}
	if (ui32ByteCount > 0)
	{
		PVR_LOG(("      Phys Page: %s", pszBuffer));
	}

	return PVRSRV_OK;

	/* Error handling */
DevPhysAddr_Error:
Sparse_Error:
MultiPage_Error:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

PVRSRV_ERROR
PMRInit(void)
{
	PVRSRV_ERROR eError;

	/* Singleton PMR context already initialised */
	if (_gsSingletonPMRContext.bModuleInitialised)
	{
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_UNRECOVERABLE_ERROR, out);
	}

	eError = OSLockCreate(&_gsSingletonPMRContext.hLock);
	PVR_LOG_GOTO_IF_ERROR(eError, "OSLockCreate", out);

	_gsSingletonPMRContext.uiNextSerialNum = 1;

	_gsSingletonPMRContext.uiNextKey = 0x8300f001 * (uintptr_t)&_gsSingletonPMRContext;

	_gsSingletonPMRContext.bModuleInitialised = IMG_TRUE;

	OSAtomicWrite(&_gsSingletonPMRContext.uiNumLivePMRs, 0);

#if defined(PVRSRV_ENABLE_LINUX_MMAP_STATS)
	eError = MMapStatsInit();
	PVR_LOG_GOTO_IF_ERROR(eError, "MMapStatsInit", destroy_context_lock);
#endif

	return PVRSRV_OK;

#if defined(PVRSRV_ENABLE_LINUX_MMAP_STATS)
destroy_context_lock:
	OSLockDestroy(_gsSingletonPMRContext.hLock);
	_gsSingletonPMRContext.hLock = NULL;
#endif
out:
	return eError;
}

PVRSRV_ERROR
PMRDeInit(void)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (psPVRSRVData->eServicesState != PVRSRV_SERVICES_STATE_OK)
	{
		goto out;
	}

	/* Singleton PMR context is not initialised */
	if (!_gsSingletonPMRContext.bModuleInitialised)
	{
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_UNRECOVERABLE_ERROR, out);
	}

#if defined(PVRSRV_ENABLE_LINUX_MMAP_STATS)
	MMapStatsDeInit();
#endif

	if (OSAtomicRead(&_gsSingletonPMRContext.uiNumLivePMRs) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Error: %d live PMRs remain",
				__func__,
				OSAtomicRead(&_gsSingletonPMRContext.uiNumLivePMRs)));
		PVR_DPF((PVR_DBG_ERROR, "%s: This is an unrecoverable error; a subsequent crash is inevitable",
				__func__));
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_UNRECOVERABLE_ERROR, out);
	}

	if (_gsSingletonPMRContext.hLock != NULL)
	{
		OSLockDestroy(_gsSingletonPMRContext.hLock);
	}

	_gsSingletonPMRContext.bModuleInitialised = IMG_FALSE;

out:
	PVR_ASSERT(eError == PVRSRV_OK);
	return eError;
}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
PVRSRV_ERROR
PMRInitDevice(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	PVRSRV_ERROR eError;

	eError = OSLockCreate(&psDeviceNode->hPMRZombieListLock);
	PVR_LOG_RETURN_IF_ERROR(eError, "OSLockCreate");

	dllist_init(&psDeviceNode->sPMRZombieList);
	psDeviceNode->uiPMRZombieCount = 0;
	psDeviceNode->uiPMRZombieCountInCleanup = 0;

	return PVRSRV_OK;
}

void
PMRFreeZombies(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	DECLARE_DLLIST(sZombieList);
	DLLIST_NODE *psThis, *psNext;
	IMG_INT32 uiZombieCount;

	OSLockAcquire(psDeviceNode->hPMRZombieListLock);
	/* Move the zombie list to a local copy. The original list will become
	 * an empty list. This will allow us to process the list without holding
	 * the list lock. */
	dllist_replace_head(&psDeviceNode->sPMRZombieList, &sZombieList);
	uiZombieCount = psDeviceNode->uiPMRZombieCount;
	psDeviceNode->uiPMRZombieCount = 0;
	OSLockRelease(psDeviceNode->hPMRZombieListLock);

	dllist_foreach_node(&sZombieList, psThis, psNext)
	{
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
		if (PMR_IsListNodeZombiePages(psThis))
		{
			PVRSRV_ERROR eError;
			PMR_ZOMBIE_PAGES* psZombiePages = PMR_GetZombiePagesFromNode(psThis);

			eError = psZombiePages->pfnFactoryFreeZombies(psZombiePages->pvFactoryPages);
			if (eError != PVRSRV_OK)
			{
				/* In case of failure to free zombie pages, remove it from
				 * the sZombieList and add back to the original list. */
				OSLockAcquire(psDeviceNode->hPMRZombieListLock);
				dllist_remove_node(psThis);
				dllist_add_to_tail(&psDeviceNode->sPMRZombieList, psThis);
				psDeviceNode->uiPMRZombieCount++;
				OSLockRelease(psDeviceNode->hPMRZombieListLock);

				PVR_DPF((PVR_DBG_ERROR, "Cannot free zombie pages!"));
				continue;
			}

			dllist_remove_node(psThis);
			OSFreeMem(psZombiePages);
		}
		else
#endif
		{
			PMR *psPMR = PMR_GetPMRFromNode(psThis);
			const PMR_IMPL_FUNCTAB *psFuncTable = psPMR->psFuncTab;

			dllist_remove_node(psThis);

			_FactoryLock(psFuncTable);
			_PMRDestroy(psPMR);
			_FactoryUnlock(psFuncTable);
		}

		uiZombieCount--;
	}

	PVR_ASSERT(uiZombieCount == 0);
}

void
PMRDumpZombies(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	DLLIST_NODE *psThis, *psNext;

	OSLockAcquire(psDeviceNode->hPMRZombieListLock);

	PVR_DPF((PVR_DBG_ERROR, "Items in zombie list: %u",
	        psDeviceNode->uiPMRZombieCount));

	dllist_foreach_node(&psDeviceNode->sPMRZombieList, psThis, psNext)
	{
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
		if (PMR_IsListNodeZombiePages(psThis))
		{
			PMR_ZOMBIE_PAGES* psZombiePages = PMR_GetZombiePagesFromNode(psThis);
			PVR_DPF((PVR_DBG_ERROR, "Zombie Pages = %p", psZombiePages));
		}
		else
#endif
		{
			PMR *psPMR = PMR_GetPMRFromNode(psThis);
			PVR_DPF((PVR_DBG_ERROR, "PMR = %px, Flavour = %s, Annotation: %s",
			        psPMR, PMR_GetTypeStr(psPMR), PMR_GetAnnotation(psPMR)));
		}
	}

	OSLockRelease(psDeviceNode->hPMRZombieListLock);
}

void
PMRDeInitDevice(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	PMRFreeZombies(psDeviceNode);

	OSLockDestroy(psDeviceNode->hPMRZombieListLock);
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */
