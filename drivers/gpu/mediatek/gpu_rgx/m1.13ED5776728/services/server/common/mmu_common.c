/*************************************************************************/ /*!
@File
@Title          Common MMU Management
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Implements basic low level control of MMU.
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
 */ /***************************************************************************/

#include "devicemem_server_utils.h"

/* Our own interface */
#include "mmu_common.h"
#include "pvr_ricommon.h"

#include "rgxmmudefs_km.h"
/*
Interfaces to other modules:

Let's keep this graph up-to-date:

   +-----------+
   | devicemem |
   +-----------+
         |
   +============+
   | mmu_common |
   +============+
         |
         +-----------------+
         |                 |
    +---------+      +----------+
    |   pmr   |      |  device  |
    +---------+      +----------+
 */

#include "img_types.h"
#include "img_defs.h"
#include "osfunc.h"
#include "allocmem.h"
#if defined(PDUMP)
#include "pdump_km.h"
#include "pdump_physmem.h"
#endif
#include "pmr.h"
/* include/ */
#include "pvr_debug.h"
#include "pvr_notifier.h"
#include "pvrsrv_error.h"
#include "pvrsrv.h"
#include "htbuffer.h"

#include "physmem.h"
#if defined(SUPPORT_GPUVIRT_VALIDATION)
#include "physmem_lma.h"
#endif

#include "dllist.h"

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#include "process_stats.h"
#include "proc_stats.h"
#endif

/* #define MMU_OBJECT_REFCOUNT_DEBUGING 1 */
#if defined(MMU_OBJECT_REFCOUNT_DEBUGING)
#define MMU_OBJ_DBG(x)	PVR_DPF(x)
#else
#define MMU_OBJ_DBG(x)
#endif

#define DUMMY_PAGE                "DUMMY_PAGE"
#define DEV_ZERO_PAGE             "DEV_ZERO_PAGE"
#define PVR_DUMMY_PAGE_INIT_VALUE 0
#define PVR_ZERO_PAGE_INIT_VALUE  0

/*!
 * Refcounted structure that is shared between the context and
 * the cleanup thread items.
 * It is used to keep track of all cleanup items and whether the creating
 * MMU context has been destroyed and therefore is not allowed to be
 * accessed any more.
 *
 * The cleanup thread is used to defer the freeing of the page tables
 * because we have to make sure that the MMU cache has been invalidated.
 * If we don't take care of this the MMU might partially access cached
 * and uncached tables which might lead to inconsistencies and in the
 * worst case to MMU pending faults on random memory.
 */
typedef struct _MMU_CTX_CLEANUP_DATA_
{
	/*! Refcount to know when this structure can be destroyed */
	ATOMIC_T iRef;
	/*! Protect items in this structure, especially the refcount */
	POS_LOCK hCleanupLock;
	/*! List of all cleanup items currently in flight */
	DLLIST_NODE sMMUCtxCleanupItemsHead;
	/*! Was the MMU context destroyed and should not be accessed any more? */
	IMG_BOOL bMMUContextExists;
#if defined(SUPPORT_GPUVIRT_VALIDATION)
	/*! Associated OSid for this context */
	IMG_UINT32 ui32OSid;
#endif	/* defined(SUPPORT_GPUVIRT_VALIDATION) */
} MMU_CTX_CLEANUP_DATA;


/*!
 * Structure holding one or more page tables that need to be
 * freed after the MMU cache has been flushed which is signalled when
 * the stored sync has a value that is <= the required value.
 */
typedef struct _MMU_CLEANUP_ITEM_
{
	/*! Cleanup thread data */
	PVRSRV_CLEANUP_THREAD_WORK sCleanupThreadFn;
	/*! List to hold all the MMU_MEMORY_MAPPINGs, i.e. page tables */
	DLLIST_NODE sMMUMappingHead;
	/*! Node of the cleanup item list for the context */
	DLLIST_NODE sMMUCtxCleanupItem;
	/* Pointer to the cleanup meta data */
	MMU_CTX_CLEANUP_DATA *psMMUCtxCleanupData;
	/* Sync to query if the MMU cache was flushed */
	PVRSRV_CLIENT_SYNC_PRIM *psSync;
	/*! The update value of the sync to signal that the cache was flushed */
	IMG_UINT32 uiRequiredSyncVal;
	/*! The update value of the power off counter */
	IMG_UINT32 uiRequiredPowerOffCounter;
	/*! The device node needed to free the page tables */
	PVRSRV_DEVICE_NODE *psDevNode;
} MMU_CLEANUP_ITEM;

/*!
	All physical allocations and frees are relative to this context, so
	we would get all the allocations of PCs, PDs, and PTs from the same
	RA.

	We have one per MMU context in case we have mixed UMA/LMA devices
	within the same system.
 */
typedef struct _MMU_PHYSMEM_CONTEXT_
{
	/*! Associated MMU_CONTEXT */
	struct _MMU_CONTEXT_ *psMMUContext;

	/*! Parent device node */
	PVRSRV_DEVICE_NODE *psDevNode;

	/*! Refcount so we know when to free up the arena */
	IMG_UINT32 uiNumAllocations;

	/*! Arena from which physical memory is derived */
	RA_ARENA *psPhysMemRA;
	/*! Arena name */
	IMG_CHAR *pszPhysMemRAName;
	/*! Size of arena name string */
	size_t uiPhysMemRANameAllocSize;

	/*! Meta data for deferred cleanup */
	MMU_CTX_CLEANUP_DATA *psCleanupData;
	/*! Temporary list of all deferred MMU_MEMORY_MAPPINGs. */
	DLLIST_NODE sTmpMMUMappingHead;

#if defined(SUPPORT_GPUVIRT_VALIDATION)
	IMG_UINT32 ui32OSid;
	IMG_UINT32 ui32OSidReg;
	IMG_BOOL   bOSidAxiProt;
#endif

} MMU_PHYSMEM_CONTEXT;

/*!
	Mapping structure for MMU memory allocation
 */
typedef struct _MMU_MEMORY_MAPPING_
{
	/*! Physmem context to allocate from */
	MMU_PHYSMEM_CONTEXT		*psContext;
	/*! OS/system Handle for this allocation */
	PG_HANDLE				sMemHandle;
	/*! CPU virtual address of this allocation */
	void					*pvCpuVAddr;
	/*! Device physical address of this allocation */
	IMG_DEV_PHYADDR			sDevPAddr;
	/*! Size of this allocation */
	size_t					uiSize;
	/*! Number of current mappings of this allocation */
	IMG_UINT32				uiCpuVAddrRefCount;
	/*! Node for the defer free list */
	DLLIST_NODE				sMMUMappingItem;
} MMU_MEMORY_MAPPING;

/*!
	Memory descriptor for MMU objects. There can be more than one memory
	descriptor per MMU memory allocation.
 */
typedef struct _MMU_MEMORY_DESC_
{
	/* NB: bValid is set if this descriptor describes physical
	   memory.  This allows "empty" descriptors to exist, such that we
	   can allocate them in batches. */
	/*! Does this MMU object have physical backing */
	IMG_BOOL				bValid;
	/*! Device Physical address of physical backing */
	IMG_DEV_PHYADDR			sDevPAddr;
	/*! CPU virtual address of physical backing */
	void					*pvCpuVAddr;
	/*! Mapping data for this MMU object */
	MMU_MEMORY_MAPPING		*psMapping;
	/*! Memdesc offset into the psMapping */
	IMG_UINT32 uiOffset;
	/*! Size of the Memdesc */
	IMG_UINT32 uiSize;
} MMU_MEMORY_DESC;

/*!
	MMU levelx structure. This is generic and is used
	for all levels (PC, PD, PT).
 */
typedef struct _MMU_Levelx_INFO_
{
	/*! The Number of entries in this level */
	IMG_UINT32 ui32NumOfEntries;

	/*! Number of times this level has been reference. Note: For Level1 (PTE)
	    we still take/drop the reference when setting up the page tables rather
	    then at map/unmap time as this simplifies things */
	IMG_UINT32 ui32RefCount;

	/*! MemDesc for this level */
	MMU_MEMORY_DESC sMemDesc;

	/*! Array of infos for the next level. Must be last member in structure */
	struct _MMU_Levelx_INFO_ *apsNextLevel[1];
} MMU_Levelx_INFO;

/*!
	MMU context structure
 */
struct _MMU_CONTEXT_
{
	/*! Originating Connection */
	CONNECTION_DATA *psConnection;

	MMU_DEVICEATTRIBS *psDevAttrs;

	/*! For allocation and deallocation of the physical memory where
	    the pagetables live */
	struct _MMU_PHYSMEM_CONTEXT_ *psPhysMemCtx;

#if defined(PDUMP)
	/*! PDump context ID (required for PDump commands with virtual addresses) */
	IMG_UINT32 uiPDumpContextID;

	/*! The refcount of the PDump context ID */
	IMG_UINT32 ui32PDumpContextIDRefCount;
#endif

	/*! MMU cache invalidation flags (only used on Volcanic driver) */
	ATOMIC_T sCacheFlags;

	/*! Lock to ensure exclusive access when manipulating the MMU context or
	 * reading and using its content
	 */
	POS_LOCK hLock;

	/*! Base level info structure. Must be last member in structure */
	MMU_Levelx_INFO sBaseLevelInfo;
	/* NO OTHER MEMBERS AFTER THIS STRUCTURE ! */
};

static const IMG_DEV_PHYADDR gsBadDevPhyAddr = {MMU_BAD_PHYS_ADDR};

#if defined(DEBUG)
#include "log2.h"
#endif


static PVRSRV_ERROR
MMU_UnmapPagesUnlocked(MMU_CONTEXT *psMMUContext,
                       PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
                       IMG_DEV_VIRTADDR sDevVAddrBase,
                       IMG_UINT32 ui32PageCount,
                       IMG_UINT32 *pai32FreeIndices,
                       IMG_UINT32 uiLog2PageSize);

static PVRSRV_ERROR
MMU_UnmapPMRFastUnlocked(MMU_CONTEXT *psMMUContext,
                         IMG_DEV_VIRTADDR sDevVAddrBase,
                         IMG_UINT32 ui32PageCount,
                         IMG_UINT32 uiLog2PageSize);

/*****************************************************************************
 *                          Utility functions                                *
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       _FreeMMUMapping

@Description    Free a given dllist of MMU_MEMORY_MAPPINGs and the page tables
                they represent.

@Input          psDevNode           Device node

@Input          psTmpMMUMappingHead List of MMU_MEMORY_MAPPINGs to free
 */
/*****************************************************************************/
static void
_FreeMMUMapping(PVRSRV_DEVICE_NODE *psDevNode,
                PDLLIST_NODE psTmpMMUMappingHead)
{
	PDLLIST_NODE psNode, psNextNode;

	/* Free the current list unconditionally */
	dllist_foreach_node(psTmpMMUMappingHead,
	                    psNode,
	                    psNextNode)
	{
		MMU_MEMORY_MAPPING *psMapping = IMG_CONTAINER_OF(psNode,
		                                                 MMU_MEMORY_MAPPING,
		                                                 sMMUMappingItem);

		psDevNode->sDevMMUPxSetup.pfnDevPxFree(psDevNode, &psMapping->sMemHandle);
		dllist_remove_node(psNode);
		OSFreeMem(psMapping);
	}
}

/*************************************************************************/ /*!
@Function       _CleanupThread_FreeMMUMapping

@Description    Function to be executed by the cleanup thread to free
                MMU_MEMORY_MAPPINGs after the MMU cache has been invalidated.

                This function will request a MMU cache invalidate once and
                retry to free the MMU_MEMORY_MAPPINGs until the invalidate
                has been executed.

                If the memory context that created this cleanup item has been
                destroyed in the meantime this function will directly free the
                MMU_MEMORY_MAPPINGs without waiting for any MMU cache
                invalidation.

@Input          pvData           Cleanup data in form of a MMU_CLEANUP_ITEM

@Return         PVRSRV_OK if successful otherwise PVRSRV_ERROR_RETRY
 */
/*****************************************************************************/
static PVRSRV_ERROR
_CleanupThread_FreeMMUMapping(void* pvData)
{
	PVRSRV_ERROR eError;
	MMU_CLEANUP_ITEM *psCleanup = (MMU_CLEANUP_ITEM *)pvData;
	MMU_CTX_CLEANUP_DATA *psMMUCtxCleanupData = psCleanup->psMMUCtxCleanupData;
	PVRSRV_DEVICE_NODE *psDevNode = psCleanup->psDevNode;
	IMG_BOOL bFreeNow;

	OSLockAcquire(psMMUCtxCleanupData->hCleanupLock);

	/* Don't attempt to free anything when the context has been destroyed.
	 * Especially don't access any device specific structures any more!*/
	if (!psMMUCtxCleanupData->bMMUContextExists)
	{
		OSFreeMem(psCleanup);
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_OK, e0);
	}

	if (psCleanup->psSync == NULL)
	{
		/* Kick to invalidate the MMU caches and get sync info */
		eError = psDevNode->pfnMMUCacheInvalidateKick(psDevNode,
		                                     &psCleanup->uiRequiredSyncVal,
		                                     IMG_TRUE);
		if (eError != PVRSRV_OK)
		{
			OSLockRelease(psMMUCtxCleanupData->hCleanupLock);
			return PVRSRV_ERROR_RETRY;
		}
		psCleanup->psSync = psDevNode->psMMUCacheSyncPrim;
	}

	/* Has the invalidate executed (sync is updated when the Firmware performs
	 * the invalidate)? */
	bFreeNow = PVRSRVHasCounter32Advanced(OSReadDeviceMem32(psCleanup->psSync->pui32LinAddr),
	                                      psCleanup->uiRequiredSyncVal);

	/* Has there been a power off? */
	bFreeNow = bFreeNow || PVRSRVHasCounter32Advanced(psDevNode->uiPowerOffCounter,
	                                                  psCleanup->uiRequiredPowerOffCounter);

#if defined(NO_HARDWARE)
	/* In NOHW the syncs will never be updated so just free the tables */
	bFreeNow = IMG_TRUE;
#endif
	/* If the Invalidate operation is not completed, check if the operation timed out */
	if (!bFreeNow)
	{
		IMG_UINT32 uiTimeStart = psCleanup->sCleanupThreadFn.ui32TimeStart;
		IMG_UINT32 uiTimeEnd = psCleanup->sCleanupThreadFn.ui32TimeEnd;

		/* If the time left for the completion of invalidate operation is
		 * within 500ms of time-out, consider the operation as timed out */
		if ((uiTimeEnd - uiTimeStart - 500) <= (OSClockms() - uiTimeStart))
		{
			/* Consider the operation is timed out */
			bFreeNow = IMG_TRUE;
		}
	}

	/* Free if the invalidate operation completed or the operation itself timed out */
	if (bFreeNow)
	{
		_FreeMMUMapping(psDevNode, &psCleanup->sMMUMappingHead);

		dllist_remove_node(&psCleanup->sMMUCtxCleanupItem);

		OSFreeMem(psCleanup);

		eError = PVRSRV_OK;
	}
	else
	{
		eError = PVRSRV_ERROR_RETRY;
	}

e0:

	/* If this cleanup task has been successfully executed we can
	 * decrease the context cleanup data refcount. Successfully
	 * means here that the MMU_MEMORY_MAPPINGs have been freed by
	 * either this cleanup task of when the MMU context has been
	 * destroyed. */
	if (eError == PVRSRV_OK)
	{
		OSLockRelease(psMMUCtxCleanupData->hCleanupLock);

		if (OSAtomicDecrement(&psMMUCtxCleanupData->iRef) == 0)
		{
			OSLockDestroy(psMMUCtxCleanupData->hCleanupLock);
			OSFreeMem(psMMUCtxCleanupData);
		}
	}
	else
	{
		OSLockRelease(psMMUCtxCleanupData->hCleanupLock);
	}


	return eError;
}

/*************************************************************************/ /*!
@Function       _SetupCleanup_FreeMMUMapping

@Description    Setup a cleanup item for the cleanup thread that will
                kick off a MMU invalidate request and free the associated
                MMU_MEMORY_MAPPINGs when the invalidate was successful.

@Input          psPhysMemCtx        The current MMU physmem context
 */
/*****************************************************************************/
static void
_SetupCleanup_FreeMMUMapping(MMU_PHYSMEM_CONTEXT *psPhysMemCtx)
{

	MMU_CLEANUP_ITEM *psCleanupItem;
	MMU_CTX_CLEANUP_DATA *psCleanupData = psPhysMemCtx->psCleanupData;
	PVRSRV_DEVICE_NODE *psDevNode = psPhysMemCtx->psDevNode;

	if (dllist_is_empty(&psPhysMemCtx->sTmpMMUMappingHead))
	{
		goto e0;
	}

#if !defined(SUPPORT_MMU_PENDING_FAULT_PROTECTION)
	/* If users deactivated this we immediately free the page tables */
	goto e1;
#endif

	/* Don't defer the freeing if we are currently unloading the driver
	 * or if the sync has been destroyed */
	if (PVRSRVGetPVRSRVData()->bUnload ||
			psDevNode->psMMUCacheSyncPrim == NULL)
	{
		goto e1;
	}

	/* Allocate a cleanup item */
	psCleanupItem = OSAllocMem(sizeof(*psCleanupItem));
	if (!psCleanupItem)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: Failed to get memory for deferred page table cleanup. "
				"Freeing tables immediately",
				__func__));
		goto e1;
	}

	/* Set sync to NULL to indicate we did not interact with
	 * the FW yet. Kicking off an MMU cache invalidate should
	 * be done in the cleanup thread to not waste time here. */
	psCleanupItem->psSync = NULL;
	psCleanupItem->uiRequiredSyncVal = 0;
	psCleanupItem->uiRequiredPowerOffCounter = psDevNode->uiPowerOffCounterNext;
	psCleanupItem->psDevNode = psDevNode;
	psCleanupItem->psMMUCtxCleanupData = psCleanupData;

	OSAtomicIncrement(&psCleanupData->iRef);

	/* Move the page tables to free to the cleanup item */
	dllist_replace_head(&psPhysMemCtx->sTmpMMUMappingHead,
	                    &psCleanupItem->sMMUMappingHead);

	/* Add the cleanup item itself to the context list */
	dllist_add_to_tail(&psCleanupData->sMMUCtxCleanupItemsHead,
	                   &psCleanupItem->sMMUCtxCleanupItem);

	/* Setup the cleanup thread data and add the work item */
	psCleanupItem->sCleanupThreadFn.pfnFree = _CleanupThread_FreeMMUMapping;
	psCleanupItem->sCleanupThreadFn.pvData = psCleanupItem;
	psCleanupItem->sCleanupThreadFn.bDependsOnHW = IMG_TRUE;
	CLEANUP_THREAD_SET_RETRY_TIMEOUT(&psCleanupItem->sCleanupThreadFn,
	                                 CLEANUP_THREAD_RETRY_TIMEOUT_MS_DEFAULT);

	PVRSRVCleanupThreadAddWork(psDevNode, &psCleanupItem->sCleanupThreadFn);

	return;

e1:
	/* Free the page tables now */
	_FreeMMUMapping(psDevNode, &psPhysMemCtx->sTmpMMUMappingHead);
e0:
	return;
}

/*************************************************************************/ /*!
@Function       _CalcPCEIdx

@Description    Calculate the page catalogue index

@Input          sDevVAddr           Device virtual address

@Input          psDevVAddrConfig    Configuration of the virtual address

@Input          bRoundUp            Round up the index

@Return         The page catalogue index
 */
/*****************************************************************************/
static IMG_UINT32 _CalcPCEIdx(IMG_DEV_VIRTADDR sDevVAddr,
                              const MMU_DEVVADDR_CONFIG *psDevVAddrConfig,
                              IMG_BOOL bRoundUp)
{
	IMG_DEV_VIRTADDR sTmpDevVAddr;
	IMG_UINT32 ui32RetVal;

	sTmpDevVAddr = sDevVAddr;

	if (bRoundUp)
	{
		sTmpDevVAddr.uiAddr--;
	}
	ui32RetVal = (IMG_UINT32) ((sTmpDevVAddr.uiAddr & psDevVAddrConfig->uiPCIndexMask)
			>> psDevVAddrConfig->uiPCIndexShift);

	if (bRoundUp)
	{
		ui32RetVal++;
	}

	return ui32RetVal;
}


/*************************************************************************/ /*!
@Function       _CalcPDEIdx

@Description    Calculate the page directory index

@Input          sDevVAddr           Device virtual address

@Input          psDevVAddrConfig    Configuration of the virtual address

@Input          bRoundUp            Round up the index

@Return         The page directory index
 */
/*****************************************************************************/
static IMG_UINT32 _CalcPDEIdx(IMG_DEV_VIRTADDR sDevVAddr,
                              const MMU_DEVVADDR_CONFIG *psDevVAddrConfig,
                              IMG_BOOL bRoundUp)
{
	IMG_DEV_VIRTADDR sTmpDevVAddr;
	IMG_UINT32 ui32RetVal;

	sTmpDevVAddr = sDevVAddr;

	if (bRoundUp)
	{
		sTmpDevVAddr.uiAddr--;
	}
	ui32RetVal = (IMG_UINT32) ((sTmpDevVAddr.uiAddr & psDevVAddrConfig->uiPDIndexMask)
			>> psDevVAddrConfig->uiPDIndexShift);

	if (bRoundUp)
	{
		ui32RetVal++;
	}

	return ui32RetVal;
}


/*************************************************************************/ /*!
@Function       _CalcPTEIdx

@Description    Calculate the page entry index

@Input          sDevVAddr           Device virtual address

@Input          psDevVAddrConfig    Configuration of the virtual address

@Input          bRoundUp            Round up the index

@Return         The page entry index
 */
/*****************************************************************************/
static IMG_UINT32 _CalcPTEIdx(IMG_DEV_VIRTADDR sDevVAddr,
                              const MMU_DEVVADDR_CONFIG *psDevVAddrConfig,
                              IMG_BOOL bRoundUp)
{
	IMG_DEV_VIRTADDR sTmpDevVAddr;
	IMG_UINT32 ui32RetVal;

	sTmpDevVAddr = sDevVAddr;
	sTmpDevVAddr.uiAddr -= psDevVAddrConfig->uiOffsetInBytes;
	if (bRoundUp)
	{
		sTmpDevVAddr.uiAddr--;
	}
	ui32RetVal = (IMG_UINT32) ((sTmpDevVAddr.uiAddr & psDevVAddrConfig->uiPTIndexMask)
			>> psDevVAddrConfig->uiPTIndexShift);

	if (bRoundUp)
	{
		ui32RetVal++;
	}

	return ui32RetVal;
}

#if defined(RGX_BRN71422_TARGET_HARDWARE_PHYSICAL_ADDR)
/*
 * RGXMapBRN71422TargetPhysicalAddress
 *
 * Set-up a special MMU tree mapping with a single page that eventually points to
 * RGX_BRN71422_TARGET_HARDWARE_PHYSICAL_ADDR.
 *
 * PC entries are 32b, with the last 4 bits being 0 except for the LSB bit that should be 1 (Valid). Addr is 4KB aligned.
 * PD entries are 64b, with addr in bits 39:5 and everything else 0 except for LSB bit that is Valid. Addr is byte aligned?
 * PT entries are 64b, with phy addr in bits 39:12 and everything else 0 except for LSB bit that is Valid. Addr is 4KB aligned.
 * So, we can construct the page tables in a single page like this:
 *   0x00 : PCE (PCE index 0)
 *   0x04 : 0x0
 *   0x08 : PDEa (PDE index 1)
 *   0x0C : PDEb
 *   0x10 : PTEa (PTE index 2)
 *   0x14 : PTEb
 *
 * With the PCE and the PDE pointing to this same page. 
 * The VA address that we are mapping is therefore:
 *  VA = PCE_idx*PCE_size + PDE_idx*PDE_size + PTE_idx*PTE_size = 
 *     =      0 * 1GB     +      1 * 2MB     +      2 * 4KB     =
 *     =        0         +    0x20_0000     +      0x2000      =
 *     = 0x00_0020_2000
 */
void RGXMapBRN71422TargetPhysicalAddress(MMU_CONTEXT *psMMUContext)
{
	MMU_MEMORY_DESC  *psMemDesc  = &psMMUContext->sBaseLevelInfo.sMemDesc;
	IMG_DEV_PHYADDR  sPhysAddrPC = psMemDesc->sDevPAddr;
	IMG_UINT32       *pui32Px    = psMemDesc->pvCpuVAddr; 
	IMG_UINT64       *pui64Px    = psMemDesc->pvCpuVAddr; 
	IMG_UINT64       ui64Entry;

	/* PCE points to PC */
	ui64Entry = sPhysAddrPC.uiAddr;
	ui64Entry = ui64Entry >> RGX_MMUCTRL_PC_DATA_PD_BASE_ALIGNSHIFT;
	ui64Entry = ui64Entry << RGX_MMUCTRL_PC_DATA_PD_BASE_SHIFT;
	ui64Entry = ui64Entry & ~RGX_MMUCTRL_PC_DATA_PD_BASE_CLRMSK;
	ui64Entry = ui64Entry | RGX_MMUCTRL_PC_DATA_VALID_EN;
	pui32Px[0] = (IMG_UINT32) ui64Entry;

	/* PDE points to PC */
	ui64Entry = sPhysAddrPC.uiAddr;
	ui64Entry = ui64Entry & ~RGX_MMUCTRL_PD_DATA_PT_BASE_CLRMSK;
	ui64Entry = ui64Entry | RGX_MMUCTRL_PD_DATA_VALID_EN;
	pui64Px[1] = ui64Entry;

	/* PTE points to PAddr */
	ui64Entry = RGX_BRN71422_TARGET_HARDWARE_PHYSICAL_ADDR;
	ui64Entry = ui64Entry & ~RGX_MMUCTRL_PT_DATA_PAGE_CLRMSK;
	ui64Entry = ui64Entry | RGX_MMUCTRL_PT_DATA_VALID_EN;
	pui64Px[2] = ui64Entry;

	{
		PVRSRV_ERROR eError;
		PVRSRV_DEVICE_NODE *psDevNode = (PVRSRV_DEVICE_NODE *)psMMUContext->psPhysMemCtx->psDevNode;
		eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
														 &psMemDesc->psMapping->sMemHandle,
														 psMemDesc->uiOffset,
														 psMemDesc->uiSize);
		PVR_LOG_IF_ERROR(eError, "pfnDevPxClean");
	}

	PVR_DPF((PVR_DBG_MESSAGE, "%s: Mapping the BRN71422 workaround to target physical address 0x%" IMG_UINT64_FMTSPECx ".",
	         __func__, RGX_BRN71422_TARGET_HARDWARE_PHYSICAL_ADDR));
}
#endif 

/*****************************************************************************
 *         MMU memory allocation/management functions (mem desc)             *
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       _MMU_PhysMem_RAImportAlloc

@Description    Imports MMU Px memory into the RA. This is where the
                actual allocation of physical memory happens.

@Input          hArenaHandle    Handle that was passed in during the
                                creation of the RA

@Input          uiSize          Size of the memory to import

@Input          uiFlags         Flags that where passed in the allocation.

@Output         puiBase         The address of where to insert this import

@Output         puiActualSize   The actual size of the import

@Output         phPriv          Handle which will be passed back when
                                this import is freed

@Return         PVRSRV_OK if import alloc was successful
 */
/*****************************************************************************/
static PVRSRV_ERROR _MMU_PhysMem_RAImportAlloc(RA_PERARENA_HANDLE hArenaHandle,
                                               RA_LENGTH_T uiSize,
                                               RA_FLAGS_T uiFlags,
                                               const IMG_CHAR *pszAnnotation,
                                               RA_BASE_T *puiBase,
                                               RA_LENGTH_T *puiActualSize,
                                               RA_PERISPAN_HANDLE *phPriv)
{
	MMU_PHYSMEM_CONTEXT *psPhysMemCtx = (MMU_PHYSMEM_CONTEXT *)hArenaHandle;
	PVRSRV_DEVICE_NODE *psDevNode = (PVRSRV_DEVICE_NODE *)psPhysMemCtx->psDevNode;
	MMU_MEMORY_MAPPING *psMapping;
	PVRSRV_ERROR eError;
	IMG_UINT32 uiPid = 0;

	PVR_UNREFERENCED_PARAMETER(pszAnnotation);
	PVR_UNREFERENCED_PARAMETER(uiFlags);

	PVR_ASSERT(psDevNode != NULL);
	PVR_GOTO_IF_INVALID_PARAM(psDevNode, eError, e0);

	psMapping = OSAllocMem(sizeof(MMU_MEMORY_MAPPING));
	PVR_GOTO_IF_NOMEM(psMapping, eError, e0);

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
	uiPid = psDevNode->eDevState == PVRSRV_DEVICE_STATE_INIT ?
	        PVR_SYS_ALLOC_PID : OSGetCurrentClientProcessIDKM();
#endif

#if defined(SUPPORT_GPUVIRT_VALIDATION)
	/*
	 * Store the OSid in the PG_HANDLE.uiOSid field for use by the
	 * pfnDevPxFree() routine.
	 */
	psMapping->sMemHandle.uiOSid = psPhysMemCtx->ui32OSid;
	eError = psDevNode->sDevMMUPxSetup.pfnDevPxAllocGPV(psDevNode,
	                                                    TRUNCATE_64BITS_TO_SIZE_T(uiSize),
	                                                    &psMapping->sMemHandle,
	                                                    &psMapping->sDevPAddr,
	                                                    psPhysMemCtx->ui32OSid,
	                                                    uiPid);
#else
	eError = psDevNode->sDevMMUPxSetup.pfnDevPxAlloc(psDevNode,
	                                                 TRUNCATE_64BITS_TO_SIZE_T(uiSize),
	                                                 &psMapping->sMemHandle,
	                                                 &psMapping->sDevPAddr,
	                                                 uiPid);
#endif
	if (eError != PVRSRV_OK)
	{
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
		PVRSRVStatsUpdateOOMStats(PVRSRV_PROCESS_STAT_TYPE_OOM_PHYSMEM_COUNT,
					  OSGetCurrentClientProcessIDKM());
#endif
		goto e1;
	}

	psMapping->psContext = psPhysMemCtx;
	psMapping->uiSize = TRUNCATE_64BITS_TO_SIZE_T(uiSize);

	psMapping->uiCpuVAddrRefCount = 0;

	*phPriv = (RA_PERISPAN_HANDLE) psMapping;

	/* Note: This assumes this memory never gets paged out */
	*puiBase = (RA_BASE_T)psMapping->sDevPAddr.uiAddr;
	*puiActualSize = uiSize;

	return PVRSRV_OK;

e1:
	OSFreeMem(psMapping);
e0:
	return eError;
}

/*************************************************************************/ /*!
@Function       _MMU_PhysMem_RAImportFree

@Description    Imports MMU Px memory into the RA. This is where the
                actual free of physical memory happens.

@Input          hArenaHandle    Handle that was passed in during the
                                creation of the RA

@Input          puiBase         The address of where to insert this import

@Output         phPriv          Private data that the import alloc provided

@Return         None
 */
/*****************************************************************************/
static void _MMU_PhysMem_RAImportFree(RA_PERARENA_HANDLE hArenaHandle,
                                      RA_BASE_T uiBase,
                                      RA_PERISPAN_HANDLE hPriv)
{
	MMU_MEMORY_MAPPING *psMapping = (MMU_MEMORY_MAPPING *)hPriv;
	MMU_PHYSMEM_CONTEXT *psPhysMemCtx = (MMU_PHYSMEM_CONTEXT *)hArenaHandle;

	PVR_UNREFERENCED_PARAMETER(uiBase);

	/* Check we have dropped all CPU mappings */
	PVR_ASSERT(psMapping->uiCpuVAddrRefCount == 0);

	/* Add mapping to defer free list */
	psMapping->psContext = NULL;
	dllist_add_to_tail(&psPhysMemCtx->sTmpMMUMappingHead, &psMapping->sMMUMappingItem);
}

/*************************************************************************/ /*!
@Function       _MMU_PhysMemAlloc

@Description    Allocates physical memory for MMU objects

@Input          psPhysMemCtx    Physmem context to do the allocation from

@Output         psMemDesc       Allocation description

@Input          uiBytes         Size of the allocation in bytes

@Input          uiAlignment     Alignment requirement of this allocation

@Return         PVRSRV_OK if allocation was successful
 */
/*****************************************************************************/

static PVRSRV_ERROR _MMU_PhysMemAlloc(MMU_PHYSMEM_CONTEXT *psPhysMemCtx,
                                      MMU_MEMORY_DESC *psMemDesc,
                                      size_t uiBytes,
                                      size_t uiAlignment)
{
	PVRSRV_ERROR eError;
	RA_BASE_T uiPhysAddr;

	PVR_RETURN_IF_INVALID_PARAM(psMemDesc);
	PVR_RETURN_IF_INVALID_PARAM(!psMemDesc->bValid);

	eError = RA_Alloc(psPhysMemCtx->psPhysMemRA,
	                  uiBytes,
	                  RA_NO_IMPORT_MULTIPLIER,
	                  0, /* flags */
	                  uiAlignment,
	                  "",
	                  &uiPhysAddr,
	                  NULL,
	                  (RA_PERISPAN_HANDLE *)&psMemDesc->psMapping);

	PVR_LOG_RETURN_IF_ERROR(eError, "RA_Alloc");

	psMemDesc->bValid = IMG_TRUE;
	psMemDesc->pvCpuVAddr = NULL;
	psMemDesc->sDevPAddr.uiAddr = (IMG_UINT64) uiPhysAddr;

	if (psMemDesc->psMapping->uiCpuVAddrRefCount == 0)
	{
		eError = psPhysMemCtx->psDevNode->sDevMMUPxSetup.pfnDevPxMap(psPhysMemCtx->psDevNode,
		                                                             &psMemDesc->psMapping->sMemHandle,
		                                                             psMemDesc->psMapping->uiSize,
		                                                             &psMemDesc->psMapping->sDevPAddr,
		                                                             &psMemDesc->psMapping->pvCpuVAddr);
		if (eError != PVRSRV_OK)
		{
			RA_Free(psPhysMemCtx->psPhysMemRA, psMemDesc->sDevPAddr.uiAddr);
			return eError;
		}
	}

	psMemDesc->psMapping->uiCpuVAddrRefCount++;
	psMemDesc->uiOffset = (psMemDesc->sDevPAddr.uiAddr - psMemDesc->psMapping->sDevPAddr.uiAddr);
	psMemDesc->pvCpuVAddr = (IMG_UINT8 *)psMemDesc->psMapping->pvCpuVAddr + psMemDesc->uiOffset;
	psMemDesc->uiSize = uiBytes;
	PVR_ASSERT(psMemDesc->pvCpuVAddr != NULL);

	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       _MMU_PhysMemFree

@Description    Allocates physical memory for MMU objects

@Input          psPhysMemCtx    Physmem context to do the free on

@Input          psMemDesc       Allocation description

@Return         None
 */
/*****************************************************************************/
static void _MMU_PhysMemFree(MMU_PHYSMEM_CONTEXT *psPhysMemCtx,
                             MMU_MEMORY_DESC *psMemDesc)
{
	RA_BASE_T uiPhysAddr;

	PVR_ASSERT(psMemDesc->bValid);

	if (--psMemDesc->psMapping->uiCpuVAddrRefCount == 0)
	{
		psPhysMemCtx->psDevNode->sDevMMUPxSetup.pfnDevPxUnMap(psPhysMemCtx->psDevNode,
		                                                      &psMemDesc->psMapping->sMemHandle,
		                                                      psMemDesc->psMapping->pvCpuVAddr);
	}

	psMemDesc->pvCpuVAddr = NULL;

	uiPhysAddr = psMemDesc->sDevPAddr.uiAddr;
	RA_Free(psPhysMemCtx->psPhysMemRA, uiPhysAddr);

	psMemDesc->bValid = IMG_FALSE;
}


/*****************************************************************************
 *              MMU object allocation/management functions                   *
 *****************************************************************************/

static INLINE PVRSRV_ERROR _MMU_ConvertDevMemFlags(IMG_BOOL bInvalidate,
                                                   PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
                                                   MMU_PROTFLAGS_T *uiMMUProtFlags,
                                                   MMU_CONTEXT *psMMUContext)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_UINT32 uiGPUCacheMode;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	/* Do flag conversion between devmem flags and MMU generic flags */
	if (bInvalidate == IMG_FALSE)
	{
		*uiMMUProtFlags |= ((uiMappingFlags & PVRSRV_MEMALLOCFLAG_DEVICE_FLAGS_MASK)
				>> PVRSRV_MEMALLOCFLAG_DEVICE_FLAGS_OFFSET)
				<< MMU_PROTFLAGS_DEVICE_OFFSET;

		if (PVRSRV_CHECK_GPU_READABLE(uiMappingFlags))
		{
			*uiMMUProtFlags |= MMU_PROTFLAGS_READABLE;
		}
		if (PVRSRV_CHECK_GPU_WRITEABLE(uiMappingFlags))
		{
			*uiMMUProtFlags |= MMU_PROTFLAGS_WRITEABLE;
		}

		eError = DevmemDeviceCacheMode(psDevNode, uiMappingFlags, &uiGPUCacheMode);
		PVR_RETURN_IF_ERROR(eError);

		switch (uiGPUCacheMode)
		{
			case PVRSRV_MEMALLOCFLAG_GPU_UNCACHED:
			case PVRSRV_MEMALLOCFLAG_GPU_WRITE_COMBINE:
				break;
			case PVRSRV_MEMALLOCFLAG_GPU_CACHED:
				*uiMMUProtFlags |= MMU_PROTFLAGS_CACHED;
				break;
			default:
				PVR_DPF((PVR_DBG_ERROR,
						"%s: Wrong parameters",
						__func__));
				return PVRSRV_ERROR_INVALID_PARAMS;
		}

		if (DevmemDeviceCacheCoherency(psDevNode, uiMappingFlags))
		{
			*uiMMUProtFlags |= MMU_PROTFLAGS_CACHE_COHERENT;
		}
 /* Only compile if RGX_FEATURE_MIPS_BIT_MASK is defined to avoid compilation
  * errors on volcanic cores.
  */
 #if defined(SUPPORT_RGX) && defined(RGX_FEATURE_MIPS_BIT_MASK)
		if ((psDevNode->pfnCheckDeviceFeature) &&
			 PVRSRV_IS_FEATURE_SUPPORTED(psDevNode, MIPS))
		{
			/* If we are allocating on the MMU of the firmware processor, the
			 * cached/uncached attributes must depend on the FIRMWARE_CACHED
			 * allocation flag.
			 */
			if (psMMUContext->psDevAttrs == psDevNode->psFirmwareMMUDevAttrs)
			{
				if (uiMappingFlags & PVRSRV_MEMALLOCFLAG_DEVICE_FLAG(FIRMWARE_CACHED))
				{
					*uiMMUProtFlags |= MMU_PROTFLAGS_CACHED;
				}
				else
				{
					*uiMMUProtFlags &= ~MMU_PROTFLAGS_CACHED;

				}
				*uiMMUProtFlags &= ~MMU_PROTFLAGS_CACHE_COHERENT;
			}
		}
#endif
	}
	else
	{
		*uiMMUProtFlags |= MMU_PROTFLAGS_INVALID;
	}

	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       _PxMemAlloc

@Description    Allocates physical memory for MMU objects, initialises
                and PDumps it.

@Input          psMMUContext    MMU context

@Input          uiNumEntries    Number of entries to allocate

@Input          psConfig        MMU Px config

@Input          eMMULevel       MMU level that that allocation is for

@Output         psMemDesc       Description of allocation

@Return         PVRSRV_OK if allocation was successful
 */
/*****************************************************************************/
static PVRSRV_ERROR _PxMemAlloc(MMU_CONTEXT *psMMUContext,
                                IMG_UINT32 uiNumEntries,
                                const MMU_PxE_CONFIG *psConfig,
                                MMU_LEVEL eMMULevel,
                                MMU_MEMORY_DESC *psMemDesc,
                                IMG_UINT32 uiLog2Align)
{
	PVRSRV_ERROR eError;
	size_t uiBytes;
	size_t uiAlign;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	PVR_ASSERT(psConfig->uiBytesPerEntry != 0);

	uiBytes = uiNumEntries * psConfig->uiBytesPerEntry;
	/* We need here the alignment of the previous level because that is the entry for we generate here */
	uiAlign = 1 << uiLog2Align;

	/*
	 * If the hardware specifies an alignment requirement for a page table then
	 * it also requires that all memory up to the next aligned address is
	 * zeroed.
	 *
	 * Failing to do this can result in uninitialised data outside of the actual
	 * page table range being read by the MMU and treated as valid, e.g. the
	 * pending flag.
	 *
	 * Typically this will affect 1MiB, 2MiB PT pages which have a size of 16
	 * and 8 bytes respectively but an alignment requirement of 64 bytes each.
	 */
	uiBytes = PVR_ALIGN(uiBytes, uiAlign);

	/* allocate the object */
	eError = _MMU_PhysMemAlloc(psMMUContext->psPhysMemCtx,
	                           psMemDesc, uiBytes, uiAlign);
	if (eError != PVRSRV_OK)
	{
		PVR_LOG_GOTO_WITH_ERROR("_MMU_PhysMemAlloc", eError, PVRSRV_ERROR_OUT_OF_MEMORY, e0);
	}

	/*
		Clear the object
		Note: if any MMUs are cleared with non-zero values then will need a
		custom clear function
		Note: 'Cached' is wrong for the LMA + ARM64 combination, but this is
		unlikely
	 */
	OSCachedMemSet(psMemDesc->pvCpuVAddr, 0, uiBytes);

	eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
	                                                 &psMemDesc->psMapping->sMemHandle,
	                                                 psMemDesc->uiOffset,
	                                                 psMemDesc->uiSize);
	PVR_GOTO_IF_ERROR(eError, e1);

#if defined(PDUMP)
	PDUMPCOMMENT("Alloc MMU object");

	PDumpMMUMalloc(psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
	               eMMULevel,
	               &psMemDesc->sDevPAddr,
	               uiBytes,
	               uiAlign,
	               psMMUContext->psDevAttrs->eMMUType);

	PDumpMMUDumpPxEntries(eMMULevel,
	                      psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
	                      psMemDesc->pvCpuVAddr,
	                      psMemDesc->sDevPAddr,
	                      0,
	                      uiNumEntries,
	                      NULL, NULL, 0, /* pdump symbolic info is irrelevant here */
	                      psConfig->uiBytesPerEntry,
	                      uiLog2Align,
	                      psConfig->uiAddrShift,
	                      psConfig->uiAddrMask,
	                      psConfig->uiProtMask,
	                      psConfig->uiValidEnMask,
	                      0,
	                      psMMUContext->psDevAttrs->eMMUType);
#endif

	return PVRSRV_OK;
e1:
	_MMU_PhysMemFree(psMMUContext->psPhysMemCtx,
	                 psMemDesc);
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

/*************************************************************************/ /*!
@Function       _PxMemFree

@Description    Frees physical memory for MMU objects, de-initialises
                and PDumps it.

@Input          psMemDesc       Description of allocation

@Return         PVRSRV_OK if allocation was successful
 */
/*****************************************************************************/

static void _PxMemFree(MMU_CONTEXT *psMMUContext,
                       MMU_MEMORY_DESC *psMemDesc, MMU_LEVEL eMMULevel)
{
#if defined(MMU_CLEARMEM_ON_FREE)
	/*
		Clear the MMU object
		Note: if any MMUs are cleared with non-zero values then will need a
		custom clear function
		Note: 'Cached' is wrong for the LMA + ARM64 combination, but this is
		unlikely
	 */
	OSCachedMemSet(psMemDesc->pvCpuVAddr, 0, psMemDesc->ui32Bytes);

#if defined(PDUMP)
	PDUMPCOMMENT("Clear MMU object before freeing it");
#endif
#endif/* MMU_CLEARMEM_ON_FREE */

#if defined(PDUMP)
	PDUMPCOMMENT("Free MMU object");
	PDumpMMUFree(psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
	             eMMULevel,
	             &psMemDesc->sDevPAddr,
	             psMMUContext->psDevAttrs->eMMUType);
#else
	PVR_UNREFERENCED_PARAMETER(eMMULevel);
#endif
	/* free the PC */
	_MMU_PhysMemFree(psMMUContext->psPhysMemCtx, psMemDesc);
}

static INLINE PVRSRV_ERROR _SetupPTE(MMU_CONTEXT *psMMUContext,
                                     MMU_Levelx_INFO *psLevel,
                                     IMG_UINT32 uiIndex,
                                     const MMU_PxE_CONFIG *psConfig,
                                     const IMG_DEV_PHYADDR *psDevPAddr,
                                     IMG_BOOL bUnmap,
#if defined(PDUMP)
                                     const IMG_CHAR *pszMemspaceName,
                                     const IMG_CHAR *pszSymbolicAddr,
                                     IMG_DEVMEM_OFFSET_T uiSymbolicAddrOffset,
#endif
                                     IMG_UINT64 uiProtFlags)
{
	MMU_MEMORY_DESC *psMemDesc = &psLevel->sMemDesc;
	IMG_UINT64 ui64PxE64;
	IMG_UINT64 uiAddr = psDevPAddr->uiAddr;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	if (psDevNode->pfnValidateOrTweakPhysAddrs)
	{
		PVRSRV_ERROR eErr = psDevNode->pfnValidateOrTweakPhysAddrs(psDevNode,
		                                                           psMMUContext->psDevAttrs,
		                                                           &uiAddr);
		/* return if error */
		PVR_LOG_RETURN_IF_ERROR(eErr, "_SetupPTE");
	}

	/* Calculate Entry */
	ui64PxE64 =    uiAddr /* Calculate the offset to that base */
			>> psConfig->uiAddrLog2Align /* Shift away the useless bits, because the alignment is very coarse and we address by alignment */
			<< psConfig->uiAddrShift /* Shift back to fit address in the Px entry */
			& psConfig->uiAddrMask; /* Delete unused bits */
	ui64PxE64 |= uiProtFlags;

	/* Set the entry */
	if (psConfig->uiBytesPerEntry == 8)
	{
		IMG_UINT64 *pui64Px = psMemDesc->pvCpuVAddr; /* Give the virtual base address of Px */

		pui64Px[uiIndex] = ui64PxE64;
	}
	else if (psConfig->uiBytesPerEntry == 4)
	{
		IMG_UINT32 *pui32Px = psMemDesc->pvCpuVAddr; /* Give the virtual base address of Px */

		/* assert that the result fits into 32 bits before writing
		   it into the 32-bit array with a cast */
		PVR_ASSERT(ui64PxE64 == (ui64PxE64 & 0xffffffffU));

		pui32Px[uiIndex] = (IMG_UINT32) ui64PxE64;
	}
	else
	{
		return PVRSRV_ERROR_MMU_CONFIG_IS_WRONG;
	}


	/* Log modification */
	HTBLOGK(HTB_SF_MMU_PAGE_OP_TABLE,
	        HTBLOG_PTR_BITS_HIGH(psLevel), HTBLOG_PTR_BITS_LOW(psLevel),
	        uiIndex, MMU_LEVEL_1,
	        HTBLOG_U64_BITS_HIGH(ui64PxE64), HTBLOG_U64_BITS_LOW(ui64PxE64),
	        !bUnmap);

#if defined(PDUMP)
	PDumpMMUDumpPxEntries(MMU_LEVEL_1,
	                      psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
	                      psMemDesc->pvCpuVAddr,
	                      psMemDesc->sDevPAddr,
	                      uiIndex,
	                      1,
	                      pszMemspaceName,
	                      pszSymbolicAddr,
	                      uiSymbolicAddrOffset,
	                      psConfig->uiBytesPerEntry,
	                      psConfig->uiAddrLog2Align,
	                      psConfig->uiAddrShift,
	                      psConfig->uiAddrMask,
	                      psConfig->uiProtMask,
	                      psConfig->uiValidEnMask,
	                      0,
	                      psMMUContext->psDevAttrs->eMMUType);
#endif /*PDUMP*/

	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       _SetupPxE

@Description    Sets up an entry of an MMU object to point to the
                provided address

@Input          psMMUContext    MMU context to operate on

@Input          psLevel         Level info for MMU object

@Input          uiIndex         Index into the MMU object to setup

@Input          psConfig        MMU Px config

@Input          eMMULevel       Level of MMU object

@Input          psDevPAddr      Address to setup the MMU object to point to

@Input          pszMemspaceName Name of the PDump memory space that the entry
                                will point to

@Input          pszSymbolicAddr PDump symbolic address that the entry will
                                point to

@Input          uiProtFlags     MMU protection flags

@Return         PVRSRV_OK if the setup was successful
 */
/*****************************************************************************/
static PVRSRV_ERROR _SetupPxE(MMU_CONTEXT *psMMUContext,
                              MMU_Levelx_INFO *psLevel,
                              IMG_UINT32 uiIndex,
                              const MMU_PxE_CONFIG *psConfig,
                              MMU_LEVEL eMMULevel,
                              const IMG_DEV_PHYADDR *psDevPAddr,
#if defined(PDUMP)
                              const IMG_CHAR *pszMemspaceName,
                              const IMG_CHAR *pszSymbolicAddr,
                              IMG_DEVMEM_OFFSET_T uiSymbolicAddrOffset,
#endif
                              MMU_PROTFLAGS_T uiProtFlags,
                              IMG_UINT32 uiLog2DataPageSize)
{
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;
	MMU_MEMORY_DESC *psMemDesc = &psLevel->sMemDesc;

	IMG_UINT32 (*pfnDerivePxEProt4)(IMG_UINT32);
	IMG_UINT64 (*pfnDerivePxEProt8)(IMG_UINT32, IMG_UINT32);

	if (!psDevPAddr)
	{
		/* Invalidate entry */
		if (~uiProtFlags & MMU_PROTFLAGS_INVALID)
		{
			PVR_DPF((PVR_DBG_ERROR, "Error, no physical address specified, but not invalidating entry"));
			uiProtFlags |= MMU_PROTFLAGS_INVALID;
		}
		psDevPAddr = &gsBadDevPhyAddr;
	}
	else
	{
		if (uiProtFlags & MMU_PROTFLAGS_INVALID)
		{
			PVR_DPF((PVR_DBG_ERROR, "A physical address was specified when requesting invalidation of entry"));
			uiProtFlags |= MMU_PROTFLAGS_INVALID;
		}
	}

	switch (eMMULevel)
	{
		case MMU_LEVEL_3:
			pfnDerivePxEProt4 = psMMUContext->psDevAttrs->pfnDerivePCEProt4;
			pfnDerivePxEProt8 = psMMUContext->psDevAttrs->pfnDerivePCEProt8;
			break;

		case MMU_LEVEL_2:
			pfnDerivePxEProt4 = psMMUContext->psDevAttrs->pfnDerivePDEProt4;
			pfnDerivePxEProt8 = psMMUContext->psDevAttrs->pfnDerivePDEProt8;
			break;

		case MMU_LEVEL_1:
			pfnDerivePxEProt4 = psMMUContext->psDevAttrs->pfnDerivePTEProt4;
			pfnDerivePxEProt8 = psMMUContext->psDevAttrs->pfnDerivePTEProt8;
			break;

		default:
			PVR_DPF((PVR_DBG_ERROR, "%s: invalid MMU level", __func__));
			return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* How big is a PxE in bytes? */
	/* Filling the actual Px entry with an address */
	switch (psConfig->uiBytesPerEntry)
	{
		case 4:
		{
			IMG_UINT32 *pui32Px;
			IMG_UINT64 ui64PxE64;

			pui32Px = psMemDesc->pvCpuVAddr; /* Give the virtual base address of Px */

			ui64PxE64 = psDevPAddr->uiAddr               /* Calculate the offset to that base */
					>> psConfig->uiAddrLog2Align /* Shift away the unnecessary bits of the address */
					<< psConfig->uiAddrShift     /* Shift back to fit address in the Px entry */
					& psConfig->uiAddrMask;      /* Delete unused higher bits */

			ui64PxE64 |= (IMG_UINT64)pfnDerivePxEProt4(uiProtFlags);
			/* assert that the result fits into 32 bits before writing
			   it into the 32-bit array with a cast */
			PVR_ASSERT(ui64PxE64 == (ui64PxE64 & 0xffffffffU));

			/* We should never invalidate an invalid page */
			if (uiProtFlags & MMU_PROTFLAGS_INVALID)
			{
				PVR_ASSERT(pui32Px[uiIndex] != ui64PxE64);
			}
			pui32Px[uiIndex] = (IMG_UINT32) ui64PxE64;
			HTBLOGK(HTB_SF_MMU_PAGE_OP_TABLE,
			        HTBLOG_PTR_BITS_HIGH(psLevel), HTBLOG_PTR_BITS_LOW(psLevel),
			        uiIndex, eMMULevel,
			        HTBLOG_U64_BITS_HIGH(ui64PxE64), HTBLOG_U64_BITS_LOW(ui64PxE64),
			        (uiProtFlags & MMU_PROTFLAGS_INVALID)? 0: 1);
			break;
		}
		case 8:
		{
			IMG_UINT64 *pui64Px = psMemDesc->pvCpuVAddr; /* Give the virtual base address of Px */

			pui64Px[uiIndex] = psDevPAddr->uiAddr             /* Calculate the offset to that base */
					>> psConfig->uiAddrLog2Align  /* Shift away the unnecessary bits of the address */
					<< psConfig->uiAddrShift      /* Shift back to fit address in the Px entry */
					& psConfig->uiAddrMask;       /* Delete unused higher bits */
			pui64Px[uiIndex] |= pfnDerivePxEProt8(uiProtFlags, uiLog2DataPageSize);

			HTBLOGK(HTB_SF_MMU_PAGE_OP_TABLE,
			        HTBLOG_PTR_BITS_HIGH(psLevel), HTBLOG_PTR_BITS_LOW(psLevel),
			        uiIndex, eMMULevel,
			        HTBLOG_U64_BITS_HIGH(pui64Px[uiIndex]), HTBLOG_U64_BITS_LOW(pui64Px[uiIndex]),
			        (uiProtFlags & MMU_PROTFLAGS_INVALID)? 0: 1);
			break;
		}
		default:
			PVR_DPF((PVR_DBG_ERROR, "%s: PxE size not supported (%d) for level %d",
					__func__, psConfig->uiBytesPerEntry, eMMULevel));

			return PVRSRV_ERROR_MMU_CONFIG_IS_WRONG;
	}

#if defined(PDUMP)
	PDumpMMUDumpPxEntries(eMMULevel,
	                      psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
	                      psMemDesc->pvCpuVAddr,
	                      psMemDesc->sDevPAddr,
	                      uiIndex,
	                      1,
	                      pszMemspaceName,
	                      pszSymbolicAddr,
	                      uiSymbolicAddrOffset,
	                      psConfig->uiBytesPerEntry,
	                      psConfig->uiAddrLog2Align,
	                      psConfig->uiAddrShift,
	                      psConfig->uiAddrMask,
	                      psConfig->uiProtMask,
	                      psConfig->uiValidEnMask,
	                      0,
	                      psMMUContext->psDevAttrs->eMMUType);
#endif

	psDevNode->pfnMMUCacheInvalidate(psDevNode, psMMUContext,
	                                 eMMULevel,
	                                 uiProtFlags & MMU_PROTFLAGS_INVALID);

	return PVRSRV_OK;
}

/*****************************************************************************
 *                   MMU host control functions (Level Info)                 *
 *****************************************************************************/


/*************************************************************************/ /*!
@Function       _MMU_FreeLevel

@Description    Recursively frees the specified range of Px entries. If any
                level has its last reference dropped then the MMU object
                memory and the MMU_Levelx_Info will be freed.

				At each level we might be crossing a boundary from one Px to
				another. The values for auiStartArray should be by used for
				the first call into each level and the values in auiEndArray
				should only be used in the last call for each level.
				In order to determine if this is the first/last call we pass
				in bFirst and bLast.
				When one level calls down to the next only if bFirst/bLast is set
				and it's the first/last iteration of the loop at its level will
				bFirst/bLast set for the next recursion.
				This means that each iteration has the knowledge of the previous
				level which is required.

@Input          psMMUContext    MMU context to operate on

@Input          psLevel                 Level info on which to free the
                                        specified range

@Input          auiStartArray           Array of start indexes (one for each level)

@Input          auiEndArray             Array of end indexes (one for each level)

@Input          auiEntriesPerPxArray    Array of number of entries for the Px
                                        (one for each level)

@Input          apsConfig               Array of PxE configs (one for each level)

@Input          aeMMULevel              Array of MMU levels (one for each level)

@Input          pui32CurrentLevel       Pointer to a variable which is set to our
                                        current level

@Input          uiStartIndex            Start index of the range to free

@Input          uiEndIndex              End index of the range to free

@Input			bFirst                  This is the first call for this level

@Input			bLast                   This is the last call for this level

@Return         IMG_TRUE if the last reference to psLevel was dropped
 */
/*****************************************************************************/
static IMG_BOOL _MMU_FreeLevel(MMU_CONTEXT *psMMUContext,
                               MMU_Levelx_INFO *psLevel,
                               IMG_UINT32 auiStartArray[],
                               IMG_UINT32 auiEndArray[],
                               IMG_UINT32 auiEntriesPerPxArray[],
                               const MMU_PxE_CONFIG *apsConfig[],
                               MMU_LEVEL aeMMULevel[],
                               IMG_UINT32 *pui32CurrentLevel,
                               IMG_UINT32 uiStartIndex,
                               IMG_UINT32 uiEndIndex,
                               IMG_BOOL bFirst,
                               IMG_BOOL bLast,
                               IMG_UINT32 uiLog2DataPageSize)
{
	IMG_UINT32 uiThisLevel = *pui32CurrentLevel;
	const MMU_PxE_CONFIG *psConfig = apsConfig[uiThisLevel];
	IMG_UINT32 i;
	IMG_BOOL bFreed = IMG_FALSE;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	/* Call before parameter check for debugging purposes. */
	MMU_OBJ_DBG((PVR_DBG_ERROR, "_MMU_FreeLevel: level = %d, range %d - %d, refcount = %d",
			aeMMULevel[uiThisLevel], uiStartIndex,
			uiEndIndex, psLevel->ui32RefCount));

	/* Parameter checks */
	PVR_ASSERT(*pui32CurrentLevel < MMU_MAX_LEVEL);

	if (psLevel == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: invalid MMU level data", __func__));
		goto ErrReturn;
	}

	for (i = uiStartIndex; i < uiEndIndex; i++)
	{
		if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
		{
			MMU_Levelx_INFO *psNextLevel = psLevel->apsNextLevel[i];
			IMG_UINT32 uiNextStartIndex;
			IMG_UINT32 uiNextEndIndex;
			IMG_BOOL bNextFirst;
			IMG_BOOL bNextLast;

			/* If we're crossing a Px then the start index changes */
			if (bFirst && (i == uiStartIndex))
			{
				uiNextStartIndex = auiStartArray[uiThisLevel + 1];
				bNextFirst = IMG_TRUE;
			}
			else
			{
				uiNextStartIndex = 0;
				bNextFirst = IMG_FALSE;
			}

			/* If we're crossing a Px then the end index changes */
			if (bLast && (i == (uiEndIndex - 1)))
			{
				uiNextEndIndex = auiEndArray[uiThisLevel + 1];
				bNextLast = IMG_TRUE;
			}
			else
			{
				uiNextEndIndex = auiEntriesPerPxArray[uiThisLevel + 1];
				bNextLast = IMG_FALSE;
			}

			/* Recurse into the next level */
			(*pui32CurrentLevel)++;
			if (_MMU_FreeLevel(psMMUContext, psNextLevel, auiStartArray,
			                   auiEndArray, auiEntriesPerPxArray,
			                   apsConfig, aeMMULevel, pui32CurrentLevel,
			                   uiNextStartIndex, uiNextEndIndex,
			                   bNextFirst, bNextLast, uiLog2DataPageSize))
			{
				PVRSRV_ERROR eError;

				/* Un-wire the entry */
				eError = _SetupPxE(psMMUContext,
				                   psLevel,
				                   i,
				                   psConfig,
				                   aeMMULevel[uiThisLevel],
				                   NULL,
#if defined(PDUMP)
				                   NULL,	/* Only required for data page */
				                   NULL,	/* Only required for data page */
				                   0,      /* Only required for data page */
#endif
				                   MMU_PROTFLAGS_INVALID,
				                   uiLog2DataPageSize);

				PVR_ASSERT(eError == PVRSRV_OK);

				/* Free table of the level below, pointed to by this table entry.
				 * We don't destroy the table inside the above _MMU_FreeLevel call because we
				 * first have to set the table entry of the level above to invalid. */
				_PxMemFree(psMMUContext, &psNextLevel->sMemDesc, aeMMULevel[*pui32CurrentLevel]);
				OSFreeMem(psNextLevel);

				/* The level below us is empty, drop the refcount and clear the pointer */
				psLevel->ui32RefCount--;
				psLevel->apsNextLevel[i] = NULL;

				/* Check we haven't wrapped around */
				PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);
			}
			(*pui32CurrentLevel)--;
		}
		else
		{
			psLevel->ui32RefCount--;
		}

		/*
		   Free this level if it is no longer referenced, unless it's the base
		   level in which case it's part of the MMU context and should be freed
		   when the MMU context is freed
		 */
		if ((psLevel->ui32RefCount == 0) && (psLevel != &psMMUContext->sBaseLevelInfo))
		{
			bFreed = IMG_TRUE;
		}
	}

	/* Level one flushing is done when we actually write the table entries */
	if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
	{
		psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
		                                        &psLevel->sMemDesc.psMapping->sMemHandle,
		                                        uiStartIndex * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
		                                        (uiEndIndex - uiStartIndex) * psConfig->uiBytesPerEntry);
	}

ErrReturn:
	MMU_OBJ_DBG((PVR_DBG_ERROR, "_MMU_FreeLevel end: level = %u, refcount = %u",
	             aeMMULevel[uiThisLevel],
	             bFreed ? 0 : (psLevel != NULL ? psLevel->ui32RefCount : IMG_UINT32_MAX)));

	return bFreed;
}

/*************************************************************************/ /*!
@Function       _MMU_AllocLevel

@Description    Recursively allocates the specified range of Px entries. If any
                level has its last reference dropped then the MMU object
                memory and the MMU_Levelx_Info will be freed.

				At each level we might be crossing a boundary from one Px to
				another. The values for auiStartArray should be by used for
				the first call into each level and the values in auiEndArray
				should only be used in the last call for each level.
				In order to determine if this is the first/last call we pass
				in bFirst and bLast.
				When one level calls down to the next only if bFirst/bLast is set
				and it's the first/last iteration of the loop at its level will
				bFirst/bLast set for the next recursion.
				This means that each iteration has the knowledge of the previous
				level which is required.

@Input          psMMUContext    MMU context to operate on

@Input          psLevel                 Level info on which to free the
                                        specified range

@Input          auiStartArray           Array of start indexes (one for each level)

@Input          auiEndArray             Array of end indexes (one for each level)

@Input          auiEntriesPerPxArray    Array of number of entries for the Px
                                        (one for each level)

@Input          apsConfig               Array of PxE configs (one for each level)

@Input          aeMMULevel              Array of MMU levels (one for each level)

@Input          pui32CurrentLevel       Pointer to a variable which is set to our
                                        current level

@Input          uiStartIndex            Start index of the range to free

@Input          uiEndIndex              End index of the range to free

@Input			bFirst                  This is the first call for this level

@Input			bLast                   This is the last call for this level

@Return         IMG_TRUE if the last reference to psLevel was dropped
 */
/*****************************************************************************/
static PVRSRV_ERROR _MMU_AllocLevel(MMU_CONTEXT *psMMUContext,
                                    MMU_Levelx_INFO *psLevel,
                                    IMG_UINT32 auiStartArray[],
                                    IMG_UINT32 auiEndArray[],
                                    IMG_UINT32 auiEntriesPerPxArray[],
                                    const MMU_PxE_CONFIG *apsConfig[],
                                    MMU_LEVEL aeMMULevel[],
                                    IMG_UINT32 *pui32CurrentLevel,
                                    IMG_UINT32 uiStartIndex,
                                    IMG_UINT32 uiEndIndex,
                                    IMG_BOOL bFirst,
                                    IMG_BOOL bLast,
                                    IMG_UINT32 uiLog2DataPageSize)
{
	IMG_UINT32 uiThisLevel = *pui32CurrentLevel; /* Starting with 0 */
	const MMU_PxE_CONFIG *psConfig = apsConfig[uiThisLevel]; /* The table config for the current level */
	PVRSRV_ERROR eError = PVRSRV_ERROR_OUT_OF_MEMORY;
	IMG_UINT32 uiAllocState = 99; /* Debug info to check what progress was made in the function. Updated during this function. */
	IMG_UINT32 i;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	/* Sanity check */
	PVR_ASSERT(*pui32CurrentLevel < MMU_MAX_LEVEL);

	MMU_OBJ_DBG((PVR_DBG_ERROR, "_MMU_AllocLevel: level = %d, range %d - %d, refcount = %d",
			aeMMULevel[uiThisLevel], uiStartIndex,
			uiEndIndex, psLevel->ui32RefCount));

	/* Go from uiStartIndex to uiEndIndex through the Px */
	for (i = uiStartIndex;i < uiEndIndex;i++)
	{
		/* Only try an allocation if this is not the last level */
		/*Because a PT allocation is already done while setting the entry in PD */
		if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
		{
			IMG_UINT32 uiNextStartIndex;
			IMG_UINT32 uiNextEndIndex;
			IMG_BOOL bNextFirst;
			IMG_BOOL bNextLast;

			/* If there is already a next Px level existing, do not allocate it */
			if (!psLevel->apsNextLevel[i])
			{
				MMU_Levelx_INFO *psNextLevel;
				IMG_UINT32 ui32AllocSize;
				IMG_UINT32 uiNextEntries;

				/* Allocate and setup the next level */
				uiNextEntries = auiEntriesPerPxArray[uiThisLevel + 1];
				ui32AllocSize = sizeof(MMU_Levelx_INFO);
				if (aeMMULevel[uiThisLevel + 1] != MMU_LEVEL_1)
				{
					ui32AllocSize += sizeof(MMU_Levelx_INFO *) * (uiNextEntries - 1);
				}
				psNextLevel = OSAllocZMem(ui32AllocSize);
				if (psNextLevel == NULL)
				{
					uiAllocState = 0;
					goto e0;
				}

				/* Hook in this level for next time */
				psLevel->apsNextLevel[i] = psNextLevel;

				psNextLevel->ui32NumOfEntries = uiNextEntries;
				psNextLevel->ui32RefCount = 0;
				/* Allocate Px memory for a sub level*/
				eError = _PxMemAlloc(psMMUContext, uiNextEntries, apsConfig[uiThisLevel + 1],
				                     aeMMULevel[uiThisLevel + 1],
				                     &psNextLevel->sMemDesc,
				                     psConfig->uiAddrLog2Align);
				if (eError != PVRSRV_OK)
				{
					uiAllocState = 1;
					goto e0;
				}

				/* Wire up the entry */
				eError = _SetupPxE(psMMUContext,
				                   psLevel,
				                   i,
				                   psConfig,
				                   aeMMULevel[uiThisLevel],
				                   &psNextLevel->sMemDesc.sDevPAddr,
#if defined(PDUMP)
				                   NULL, /* Only required for data page */
				                   NULL, /* Only required for data page */
				                   0,    /* Only required for data page */
#endif
				                   0,
				                   uiLog2DataPageSize);

				if (eError != PVRSRV_OK)
				{
					uiAllocState = 2;
					goto e0;
				}

				psLevel->ui32RefCount++;
			}

			/* If we're crossing a Px then the start index changes */
			if (bFirst && (i == uiStartIndex))
			{
				uiNextStartIndex = auiStartArray[uiThisLevel + 1];
				bNextFirst = IMG_TRUE;
			}
			else
			{
				uiNextStartIndex = 0;
				bNextFirst = IMG_FALSE;
			}

			/* If we're crossing a Px then the end index changes */
			if (bLast && (i == (uiEndIndex - 1)))
			{
				uiNextEndIndex = auiEndArray[uiThisLevel + 1];
				bNextLast = IMG_TRUE;
			}
			else
			{
				uiNextEndIndex = auiEntriesPerPxArray[uiThisLevel + 1];
				bNextLast = IMG_FALSE;
			}

			/* Recurse into the next level */
			(*pui32CurrentLevel)++;
			eError = _MMU_AllocLevel(psMMUContext, psLevel->apsNextLevel[i],
			                         auiStartArray,
			                         auiEndArray,
			                         auiEntriesPerPxArray,
			                         apsConfig,
			                         aeMMULevel,
			                         pui32CurrentLevel,
			                         uiNextStartIndex,
			                         uiNextEndIndex,
			                         bNextFirst,
			                         bNextLast,
			                         uiLog2DataPageSize);
			(*pui32CurrentLevel)--;
			if (eError != PVRSRV_OK)
			{
				uiAllocState = 3;
				goto e0;
			}
		}
		else
		{
			/* All we need to do for level 1 is bump the refcount */
			psLevel->ui32RefCount++;
		}

		if (psLevel->ui32RefCount > psLevel->ui32NumOfEntries)
		{
			/* Given how the reference counting is implemented for MMU_LEVEL_2
			 * and MMU_LEVEL_3 this should never happen for those levels. Only
			 * in case of MMU_LEVEL_1 this should be possible (e.g. when someone
			 * takes multiple reservations of the same range). In such case
			 * return error to prevent reference count to rollover. */
			uiAllocState = 4;
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_BAD_MAPPING, e0);
		}
	}

	/* Level one flushing is done when we actually write the table entries */
	if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
	{
		eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
		                                                 &psLevel->sMemDesc.psMapping->sMemHandle,
		                                                 uiStartIndex * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
		                                                 (uiEndIndex - uiStartIndex) * psConfig->uiBytesPerEntry);
		PVR_GOTO_IF_ERROR(eError, e1);
	}

	MMU_OBJ_DBG((PVR_DBG_ERROR, "_MMU_AllocLevel end: level = %d, refcount = %d",
			aeMMULevel[uiThisLevel], psLevel->ui32RefCount));
	return PVRSRV_OK;

e0:
	/* Sanity check that we've not come down this route unexpectedly */
	PVR_ASSERT(uiAllocState!=99);
	PVR_DPF((PVR_DBG_ERROR, "_MMU_AllocLevel: Error %d allocating Px for level %d in stage %d"
			,eError, aeMMULevel[uiThisLevel], uiAllocState));

	/* In case of an error process current `i` in special way without
	 * recursively calling `_MMU_FreeLevel()`. */

	if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
	{
		/* If this is not a PT (so it's a PC or PD) it means that an error
		 * happened either during PT or PD allocation/setup.
		 *
		 * - If the error happened during PT allocation/setup it means that
		 *   we're at the PD level now and we already run below `else` clause
		 *   for the failing PT and the `for` loop below for the other PTs. This
		 *   means that all of the PTs that have been referenced during this
		 *   operation have been dereferenced. So we just need to free that
		 *   failing PT here and the rest of them in the loop below.
		 * - If the error happened during PD allocation/setup it means that
		 *   we're at the PC level now and we already run below `else` clause
		 *   for the failing PD and the `for` loop below for the other PDs. This
		 *   means that all of the PTs that have been allocated during this
		 *   operation for the failing PD have been freed. So we just need to
		 *   free that failing PD and the rest of them in a recursive manner
		 *   in the loop below. */
		if (uiAllocState >= 3)
		{
			if (psLevel->apsNextLevel[i] != NULL &&
			    psLevel->apsNextLevel[i]->ui32RefCount == 0)
			{
				psLevel->ui32RefCount--;
			}
		}
		if (uiAllocState >= 2)
		{
			if (psLevel->apsNextLevel[i] != NULL &&
			    psLevel->apsNextLevel[i]->ui32RefCount == 0)
			{
				_PxMemFree(psMMUContext, &psLevel->apsNextLevel[i]->sMemDesc,
				           aeMMULevel[uiThisLevel + 1]);
			}
		}
		if (psLevel->apsNextLevel[i] != NULL &&
		    psLevel->apsNextLevel[i]->ui32RefCount == 0)
		{
			OSFreeMem(psLevel->apsNextLevel[i]);
			psLevel->apsNextLevel[i] = NULL;
		}
	}
	else
	{
		/* This is a PT which means that we just need to dereference it. It's
		 * going to be freed on the PD level in this error path in the `if`
		 * clause above. */
		psLevel->ui32RefCount--;
	}

	/* Check we haven't wrapped around */
	PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);

e1:
	i--;

	/* The start value of index variable i is not initialised on purpose.
	 * This clean-up loop deinitialises what was already initialised in
	 * reverse order, so the i index already has the correct value.
	 */
	for (/* i already set */; i >= uiStartIndex && i < uiEndIndex; i--)
	{
		if (aeMMULevel[uiThisLevel] != MMU_LEVEL_1)
		{
			IMG_UINT32 uiNextStartIndex;
			IMG_UINT32 uiNextEndIndex;
			IMG_BOOL bNextFirst;
			IMG_BOOL bNextLast;

			/* If we're crossing a Px then the start index changes */
			if (bFirst && (i == uiStartIndex))
			{
				uiNextStartIndex = auiStartArray[uiThisLevel + 1];
				bNextFirst = IMG_TRUE;
			}
			else
			{
				uiNextStartIndex = 0;
				bNextFirst = IMG_FALSE;
			}

			/* If we're crossing a Px then the end index changes */
			if (bLast && (i == (uiEndIndex - 1)))
			{
				uiNextEndIndex = auiEndArray[uiThisLevel + 1];
				bNextLast = IMG_TRUE;
			}
			else
			{
				uiNextEndIndex = auiEntriesPerPxArray[uiThisLevel + 1];
				bNextLast = IMG_FALSE;
			}

			(*pui32CurrentLevel)++;
			if (_MMU_FreeLevel(psMMUContext, psLevel->apsNextLevel[i],
			                   auiStartArray, auiEndArray,
			                   auiEntriesPerPxArray, apsConfig,
			                   aeMMULevel, pui32CurrentLevel,
			                   uiNextStartIndex, uiNextEndIndex,
			                   bNextFirst, bNextLast, uiLog2DataPageSize))
			{
				_PxMemFree(psMMUContext, &psLevel->apsNextLevel[i]->sMemDesc,
				           aeMMULevel[uiThisLevel + 1]);
				OSFreeMem(psLevel->apsNextLevel[i]);
				psLevel->apsNextLevel[i] = NULL;

				psLevel->ui32RefCount--;

				/* Check we haven't wrapped around */
				PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);
			}
			(*pui32CurrentLevel)--;
		}
		else
		{
			/* We should never come down this path, but it's here
			   for completeness */
			psLevel->ui32RefCount--;

			/* Check we haven't wrapped around */
			PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);
		}
	}

	return eError;
}

/*****************************************************************************
 *                   MMU page table functions                                *
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       _MMU_GetLevelData

@Description    Get the all the level data and calculates the indexes for the
                specified address range

@Input          psMMUContext            MMU context to operate on

@Input          sDevVAddrStart          Start device virtual address

@Input          sDevVAddrEnd            End device virtual address

@Input          uiLog2DataPageSize      Log2 of the page size to use

@Input          auiStartArray           Array of start indexes (one for each level)

@Input          auiEndArray             Array of end indexes (one for each level)

@Input          uiEntriesPerPxArray     Array of number of entries for the Px
                                        (one for each level)

@Input          apsConfig               Array of PxE configs (one for each level)

@Input          aeMMULevel              Array of MMU levels (one for each level)

@Input          ppsMMUDevVAddrConfig    Device virtual address config

@Input			phPriv					Private data of page size config

@Return         IMG_TRUE if the last reference to psLevel was dropped
 */
/*****************************************************************************/
static void _MMU_GetLevelData(MMU_CONTEXT *psMMUContext,
                              IMG_DEV_VIRTADDR sDevVAddrStart,
                              IMG_DEV_VIRTADDR sDevVAddrEnd,
                              IMG_UINT32 uiLog2DataPageSize,
                              IMG_UINT32 auiStartArray[],
                              IMG_UINT32 auiEndArray[],
                              IMG_UINT32 auiEntriesPerPx[],
                              const MMU_PxE_CONFIG *apsConfig[],
                              MMU_LEVEL aeMMULevel[],
                              const MMU_DEVVADDR_CONFIG **ppsMMUDevVAddrConfig,
                              IMG_HANDLE *phPriv)
{
	const MMU_PxE_CONFIG *psMMUPDEConfig;
	const MMU_PxE_CONFIG *psMMUPTEConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	MMU_DEVICEATTRIBS *psDevAttrs = psMMUContext->psDevAttrs;
	PVRSRV_ERROR eError;
	IMG_UINT32 i = 0;

	eError = psDevAttrs->pfnGetPageSizeConfiguration(uiLog2DataPageSize,
	                                                 &psMMUPDEConfig,
	                                                 &psMMUPTEConfig,
	                                                 ppsMMUDevVAddrConfig,
	                                                 phPriv);
	PVR_ASSERT(eError == PVRSRV_OK);

	psDevVAddrConfig = *ppsMMUDevVAddrConfig;

	if (psDevVAddrConfig->uiPCIndexMask != 0)
	{
		auiStartArray[i] = _CalcPCEIdx(sDevVAddrStart, psDevVAddrConfig, IMG_FALSE);
		auiEndArray[i] = _CalcPCEIdx(sDevVAddrEnd, psDevVAddrConfig, IMG_TRUE);
		auiEntriesPerPx[i] = psDevVAddrConfig->uiNumEntriesPC;
		apsConfig[i] = psDevAttrs->psBaseConfig;
		aeMMULevel[i] = MMU_LEVEL_3;
		i++;
	}

	if (psDevVAddrConfig->uiPDIndexMask != 0)
	{
		auiStartArray[i] = _CalcPDEIdx(sDevVAddrStart, psDevVAddrConfig, IMG_FALSE);
		auiEndArray[i] = _CalcPDEIdx(sDevVAddrEnd, psDevVAddrConfig, IMG_TRUE);
		auiEntriesPerPx[i] = psDevVAddrConfig->uiNumEntriesPD;
		if (i == 0)
		{
			apsConfig[i] = psDevAttrs->psBaseConfig;
		}
		else
		{
			apsConfig[i] = psMMUPDEConfig;
		}
		aeMMULevel[i] = MMU_LEVEL_2;
		i++;
	}

	/*
		There is always a PTE entry so we have a slightly different behaviour than above.
		E.g. for 2 MB RGX pages the uiPTIndexMask is 0x0000000000 but still there
		is a PT with one entry.

	 */
	auiStartArray[i] = _CalcPTEIdx(sDevVAddrStart, psDevVAddrConfig, IMG_FALSE);
	if (psDevVAddrConfig->uiPTIndexMask !=0)
	{
		auiEndArray[i] = _CalcPTEIdx(sDevVAddrEnd, psDevVAddrConfig, IMG_TRUE);
	}
	else
	{
		/*
			If the PTE mask is zero it means there is only 1 PTE and thus, as an
			an exclusive bound, the end array index is equal to the start index + 1.
		 */

		auiEndArray[i] = auiStartArray[i] + 1;
	}

	auiEntriesPerPx[i] = psDevVAddrConfig->uiNumEntriesPT;

	if (i == 0)
	{
		apsConfig[i] = psDevAttrs->psBaseConfig;
	}
	else
	{
		apsConfig[i] = psMMUPTEConfig;
	}
	aeMMULevel[i] = MMU_LEVEL_1;
}

static void _MMU_PutLevelData(MMU_CONTEXT *psMMUContext, IMG_HANDLE hPriv)
{
	MMU_DEVICEATTRIBS *psDevAttrs = psMMUContext->psDevAttrs;

	psDevAttrs->pfnPutPageSizeConfiguration(hPriv);
}

/*************************************************************************/ /*!
@Function       _AllocPageTables

@Description    Allocate page tables and any higher level MMU objects required
                for the specified virtual range

@Input          psMMUContext            MMU context to operate on

@Input          sDevVAddrStart          Start device virtual address

@Input          sDevVAddrEnd            End device virtual address

@Input          uiLog2DataPageSize      Page size of the data pages

@Return         PVRSRV_OK if the allocation was successful
 */
/*****************************************************************************/
static PVRSRV_ERROR
_AllocPageTables(MMU_CONTEXT *psMMUContext,
                 IMG_DEV_VIRTADDR sDevVAddrStart,
                 IMG_DEV_VIRTADDR sDevVAddrEnd,
                 IMG_UINT32 uiLog2DataPageSize)
{
	PVRSRV_ERROR eError;
	IMG_UINT32 auiStartArray[MMU_MAX_LEVEL];
	IMG_UINT32 auiEndArray[MMU_MAX_LEVEL];
	IMG_UINT32 auiEntriesPerPx[MMU_MAX_LEVEL];
	MMU_LEVEL aeMMULevel[MMU_MAX_LEVEL];
	const MMU_PxE_CONFIG *apsConfig[MMU_MAX_LEVEL];
	const MMU_DEVVADDR_CONFIG	*psDevVAddrConfig;
	IMG_HANDLE hPriv;
	IMG_UINT32 ui32CurrentLevel = 0;

	PVR_DPF((PVR_DBG_ALLOC,
			"_AllocPageTables: vaddr range: "IMG_DEV_VIRTADDR_FMTSPEC":"IMG_DEV_VIRTADDR_FMTSPEC,
			sDevVAddrStart.uiAddr,
			sDevVAddrEnd.uiAddr
	));

#if defined(PDUMP)
	PDUMPCOMMENT("Allocating page tables for %"IMG_UINT64_FMTSPEC" bytes virtual range: "
	             IMG_DEV_VIRTADDR_FMTSPEC":"IMG_DEV_VIRTADDR_FMTSPEC,
	             (IMG_UINT64)sDevVAddrEnd.uiAddr - (IMG_UINT64)sDevVAddrStart.uiAddr,
	             (IMG_UINT64)sDevVAddrStart.uiAddr,
	             (IMG_UINT64)sDevVAddrEnd.uiAddr);
#endif

	_MMU_GetLevelData(psMMUContext, sDevVAddrStart, sDevVAddrEnd,
	                  (IMG_UINT32) uiLog2DataPageSize, auiStartArray, auiEndArray,
	                  auiEntriesPerPx, apsConfig, aeMMULevel,
	                  &psDevVAddrConfig, &hPriv);

	HTBLOGK(HTB_SF_MMU_PAGE_OP_ALLOC,
	        HTBLOG_U64_BITS_HIGH(sDevVAddrStart.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddrStart.uiAddr),
	        HTBLOG_U64_BITS_HIGH(sDevVAddrEnd.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddrEnd.uiAddr));

	eError = _MMU_AllocLevel(psMMUContext, &psMMUContext->sBaseLevelInfo,
	                         auiStartArray, auiEndArray, auiEntriesPerPx,
	                         apsConfig, aeMMULevel, &ui32CurrentLevel,
	                         auiStartArray[0], auiEndArray[0],
	                         IMG_TRUE, IMG_TRUE, uiLog2DataPageSize);

	_MMU_PutLevelData(psMMUContext, hPriv);

	return eError;
}

/*************************************************************************/ /*!
@Function       _FreePageTables

@Description    Free page tables and any higher level MMU objects at are no
                longer referenced for the specified virtual range.
                This will fill the temporary free list of the MMU context which
                needs cleanup after the call.

@Input          psMMUContext            MMU context to operate on

@Input          sDevVAddrStart          Start device virtual address

@Input          sDevVAddrEnd            End device virtual address

@Input          uiLog2DataPageSize      Page size of the data pages

@Return         None
 */
/*****************************************************************************/
static void _FreePageTables(MMU_CONTEXT *psMMUContext,
                            IMG_DEV_VIRTADDR sDevVAddrStart,
                            IMG_DEV_VIRTADDR sDevVAddrEnd,
                            IMG_UINT32 uiLog2DataPageSize)
{
	IMG_UINT32 auiStartArray[MMU_MAX_LEVEL];
	IMG_UINT32 auiEndArray[MMU_MAX_LEVEL];
	IMG_UINT32 auiEntriesPerPx[MMU_MAX_LEVEL];
	MMU_LEVEL aeMMULevel[MMU_MAX_LEVEL];
	const MMU_PxE_CONFIG *apsConfig[MMU_MAX_LEVEL];
	const MMU_DEVVADDR_CONFIG	*psDevVAddrConfig;
	IMG_UINT32 ui32CurrentLevel = 0;
	IMG_HANDLE hPriv;

	PVR_DPF((PVR_DBG_ALLOC,
			"_FreePageTables: vaddr range: "IMG_DEV_VIRTADDR_FMTSPEC":"IMG_DEV_VIRTADDR_FMTSPEC,
			sDevVAddrStart.uiAddr,
			sDevVAddrEnd.uiAddr
	));

	_MMU_GetLevelData(psMMUContext, sDevVAddrStart, sDevVAddrEnd,
	                  uiLog2DataPageSize, auiStartArray, auiEndArray,
	                  auiEntriesPerPx, apsConfig, aeMMULevel,
	                  &psDevVAddrConfig, &hPriv);

	HTBLOGK(HTB_SF_MMU_PAGE_OP_FREE,
	        HTBLOG_U64_BITS_HIGH(sDevVAddrStart.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddrStart.uiAddr),
	        HTBLOG_U64_BITS_HIGH(sDevVAddrEnd.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddrEnd.uiAddr));

	/* ignoring return code, in this case there should be no references
	 * to the level anymore, and at this stage there is nothing to do with
	 * the return status */
	(void) _MMU_FreeLevel(psMMUContext, &psMMUContext->sBaseLevelInfo,
	                      auiStartArray, auiEndArray, auiEntriesPerPx,
	                      apsConfig, aeMMULevel, &ui32CurrentLevel,
	                      auiStartArray[0], auiEndArray[0],
	                      IMG_TRUE, IMG_TRUE, uiLog2DataPageSize);

	_MMU_PutLevelData(psMMUContext, hPriv);
}


/*************************************************************************/ /*!
@Function       _MMU_GetPTInfo

@Description    Get the PT level information and PT entry index for the specified
                virtual address

@Input          psMMUContext            MMU context to operate on

@Input          psDevVAddr              Device virtual address to get the PTE info
                                        from.

@Input          psDevVAddrConfig        The current virtual address config obtained
                                        by another function call before.

@Output         psLevel                 Level info of the PT

@Output         pui32PTEIndex           Index into the PT the address corresponds to

@Return         IMG_TRUE if the operation was successful and IMG_FALSE otherwise
 */
/*****************************************************************************/
static INLINE IMG_BOOL _MMU_GetPTInfo(MMU_CONTEXT                *psMMUContext,
                                      IMG_DEV_VIRTADDR            sDevVAddr,
                                      const MMU_DEVVADDR_CONFIG  *psDevVAddrConfig,
                                      MMU_Levelx_INFO           **psLevel,
                                      IMG_UINT32                 *pui32PTEIndex)
{
	MMU_Levelx_INFO *psLocalLevel = NULL;
	MMU_LEVEL eMMULevel = psMMUContext->psDevAttrs->eTopLevel;
	const MMU_LEVEL eMMUBaseLevel = eMMULevel;
	IMG_UINT32 uiPCEIndex;
	IMG_UINT32 uiPDEIndex;

	if ((eMMULevel <= MMU_LEVEL_0) || (eMMULevel >= MMU_LEVEL_LAST))
	{
		PVR_DPF((PVR_DBG_ERROR, "_MMU_GetPTEInfo: Invalid MMU level"));
		psLevel = NULL;
		return IMG_FALSE;
	}

	for (; eMMULevel > MMU_LEVEL_0; eMMULevel--)
	{
		if (eMMULevel == MMU_LEVEL_3)
		{
			/* find the page directory pointed by the PCE */
			uiPCEIndex = _CalcPCEIdx (sDevVAddr, psDevVAddrConfig,
			                          IMG_FALSE);
			psLocalLevel = psMMUContext->sBaseLevelInfo.apsNextLevel[uiPCEIndex];
		}

		if (eMMULevel == MMU_LEVEL_2)
		{
			/* find the page table pointed by the PDE */
			uiPDEIndex = _CalcPDEIdx(sDevVAddr, psDevVAddrConfig, IMG_FALSE);
			if (psLocalLevel == NULL)
			{
				return IMG_FALSE;
			}

			psLocalLevel = psLocalLevel->apsNextLevel[uiPDEIndex];
		}

		if (eMMULevel == MMU_LEVEL_1)
		{
			/* find PTE index into page table */
			*pui32PTEIndex = _CalcPTEIdx(sDevVAddr, psDevVAddrConfig, IMG_FALSE);

			if (psLocalLevel == NULL)
			{
				if (eMMUBaseLevel == eMMULevel)
				{
					/* if the MMU only supports one level return the base level */
					psLocalLevel = &psMMUContext->sBaseLevelInfo;
				}
				else
				{
					return IMG_FALSE;
				}
			}
		}
	}

	*psLevel = psLocalLevel;

	return IMG_TRUE;
}

/*************************************************************************/ /*!
@Function       _MMU_GetPTConfig

@Description    Get the level config. Call _MMU_PutPTConfig after use!

@Input          psMMUContext            MMU context to operate on

@Input          uiLog2DataPageSize      Log 2 of the page size

@Output         ppsConfig               Config of the PTE

@Output         phPriv                  Private data handle to be passed back
                                        when the info is put

@Output         ppsDevVAddrConfig       Config of the device virtual addresses

@Return         None
 */
/*****************************************************************************/
static INLINE void _MMU_GetPTConfig(MMU_CONTEXT               *psMMUContext,
                                    IMG_UINT32                  uiLog2DataPageSize,
                                    const MMU_PxE_CONFIG      **ppsConfig,
                                    IMG_HANDLE                 *phPriv,
                                    const MMU_DEVVADDR_CONFIG **ppsDevVAddrConfig)
{
	MMU_DEVICEATTRIBS *psDevAttrs = psMMUContext->psDevAttrs;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	const MMU_PxE_CONFIG *psPDEConfig;
	const MMU_PxE_CONFIG *psPTEConfig;

	if (psDevAttrs->pfnGetPageSizeConfiguration(uiLog2DataPageSize,
	                                            &psPDEConfig,
	                                            &psPTEConfig,
	                                            &psDevVAddrConfig,
	                                            phPriv) != PVRSRV_OK)
	{
		/*
		   There should be no way we got here unless uiLog2DataPageSize
		   has changed after the MMU_Alloc call (in which case it's a bug in
		   the MM code)
		 */
		PVR_DPF((PVR_DBG_ERROR, "_MMU_GetPTConfig: Could not get valid page size config"));
		PVR_ASSERT(0);
	}

	*ppsConfig = psPTEConfig;
	*ppsDevVAddrConfig = psDevVAddrConfig;
}

/*************************************************************************/ /*!
@Function       _MMU_PutPTConfig

@Description    Put the level info. Has to be called after _MMU_GetPTConfig to
                ensure correct refcounting.

@Input          psMMUContext            MMU context to operate on

@Input          phPriv                  Private data handle created by
                                        _MMU_GetPTConfig.

@Return         None
 */
/*****************************************************************************/
static INLINE void _MMU_PutPTConfig(MMU_CONTEXT *psMMUContext,
                                    IMG_HANDLE hPriv)
{
	MMU_DEVICEATTRIBS *psDevAttrs = psMMUContext->psDevAttrs;

	if (psDevAttrs->pfnPutPageSizeConfiguration(hPriv) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: Could not put page size config",
				__func__));
		PVR_ASSERT(0);
	}
}

/* dummy pages */

static PVRSRV_ERROR _MMU_GetBackingPage(PVRSRV_DEVICE_NODE *psDevNode,
                                        PVRSRV_DEF_PAGE *psDefPage,
                                        IMG_INT uiInitValue,
                                        IMG_CHAR *pcDefPageName,
                                        IMG_BOOL bInitPage)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_DEV_PHYADDR	sDevPAddr = {0};

	OSLockAcquire(psDefPage->psPgLock);

	if (psDefPage->ui64PgPhysAddr != MMU_BAD_PHYS_ADDR)
	{
		goto UnlockAndReturn;
	}

#if defined(PDUMP)
	PDUMPCOMMENT("Alloc %s page object", pcDefPageName);
#endif

	/* Allocate the dummy page required for sparse backing */
	eError = DevPhysMemAlloc(psDevNode,
	                         (1 << psDefPage->ui32Log2PgSize),
	                         0,
	                         uiInitValue,
	                         bInitPage,
#if defined(PDUMP)
	                         psDevNode->psMMUDevAttrs->pszMMUPxPDumpMemSpaceName,
	                         pcDefPageName,
	                         &psDefPage->hPdumpPg,
#endif
	                         PVR_SYS_ALLOC_PID,
	                         &psDefPage->sPageHandle,
	                         &sDevPAddr);
	PVR_GOTO_IF_ERROR(eError, UnlockAndReturn);

	psDefPage->ui64PgPhysAddr = sDevPAddr.uiAddr;

UnlockAndReturn:
	OSLockRelease(psDefPage->psPgLock);

	return eError;
}

static void _MMU_FreeBackingPage(PVRSRV_DEVICE_NODE *psDevNode,
                                 PVRSRV_DEF_PAGE *psDefPage,
                                 IMG_CHAR *pcDefPageName)
{
	OSLockAcquire(psDefPage->psPgLock);

	if (psDefPage->ui64PgPhysAddr == MMU_BAD_PHYS_ADDR)
	{
		goto UnlockAndReturn;
	}

#if defined(PDUMP)
	PDUMPCOMMENT("Free %s page object", pcDefPageName);
#endif

	DevPhysMemFree(psDevNode,
#if defined(PDUMP)
	               psDefPage->hPdumpPg,
#endif
	               &psDefPage->sPageHandle);

#if defined(PDUMP)
	psDefPage->hPdumpPg = NULL;
#endif
	psDefPage->ui64PgPhysAddr = MMU_BAD_PHYS_ADDR;

UnlockAndReturn:
	OSLockRelease(psDefPage->psPgLock);
}


/*****************************************************************************
 *                     Public interface functions                            *
 *****************************************************************************/

/*
	MMU_InitDevice
*/
PVRSRV_ERROR MMU_InitDevice(struct _PVRSRV_DEVICE_NODE_ *psDevNode)
{
	PVRSRV_ERROR eError;

	/* Set the order to 0 */
	psDevNode->sDummyPage.sPageHandle.uiOrder = 0;
	psDevNode->sDevZeroPage.sPageHandle.uiOrder = 0;

	/* Set the size of the Dummy and Zero pages to largest page size */
	if (psDevNode->ui32RGXLog2Non4KPgSize != 0)
	{
		psDevNode->sDummyPage.ui32Log2PgSize = psDevNode->ui32RGXLog2Non4KPgSize;
		psDevNode->sDevZeroPage.ui32Log2PgSize = psDevNode->ui32RGXLog2Non4KPgSize;
	}
	else
	{
		psDevNode->sDummyPage.ui32Log2PgSize = OSGetPageSize();
		psDevNode->sDevZeroPage.ui32Log2PgSize = OSGetPageSize();
	}

	/* Set the Dummy page phys addr */
	psDevNode->sDummyPage.ui64PgPhysAddr = MMU_BAD_PHYS_ADDR;

	/* Set the Zero page phys addr */
	psDevNode->sDevZeroPage.ui64PgPhysAddr = MMU_BAD_PHYS_ADDR;

	/* The lock can be acquired from MISR (Z-buffer) path */
	eError = OSLockCreate(&psDevNode->sDummyPage.psPgLock);
	PVR_LOG_GOTO_IF_ERROR(eError, "OSLockCreate.Dummy", ErrReturnError);

	/* Create the lock for zero page */
	eError = OSLockCreate(&psDevNode->sDevZeroPage.psPgLock);
	PVR_LOG_GOTO_IF_ERROR(eError, "OSLockCreate.Zero", ErrFreeDummyPageLock);

#ifdef PDUMP
	psDevNode->sDummyPage.hPdumpPg = NULL;
	psDevNode->sDevZeroPage.hPdumpPg = NULL;
#endif /* PDUMP */

	eError = _MMU_GetBackingPage(psDevNode,
	                             &psDevNode->sDummyPage,
	                             PVR_DUMMY_PAGE_INIT_VALUE,
	                             DUMMY_PAGE,
	                             IMG_TRUE);
	PVR_LOG_GOTO_IF_ERROR(eError, "_MMU_GetBackingPage.Dummy", ErrFreeZeroPageLock);

	eError = _MMU_GetBackingPage(psDevNode,
	                             &psDevNode->sDevZeroPage,
	                             PVR_ZERO_PAGE_INIT_VALUE,
	                             DEV_ZERO_PAGE,
	                             IMG_TRUE);
	PVR_LOG_GOTO_IF_ERROR(eError, "_MMU_GetBackingPage.Zero", ErrFreeDummyPage);

	return PVRSRV_OK;

ErrFreeDummyPage:
	_MMU_FreeBackingPage(psDevNode, &psDevNode->sDummyPage, DUMMY_PAGE);
ErrFreeZeroPageLock:
	OSLockDestroy(psDevNode->sDevZeroPage.psPgLock);
	psDevNode->sDevZeroPage.psPgLock = NULL;
ErrFreeDummyPageLock:
	OSLockDestroy(psDevNode->sDummyPage.psPgLock);
	psDevNode->sDummyPage.psPgLock = NULL;
ErrReturnError:
	return eError;
}

/*
	MMU_DeInitDevice
*/
void MMU_DeInitDevice(struct _PVRSRV_DEVICE_NODE_ *psDevNode)
{
	if (psDevNode->sDummyPage.psPgLock != NULL)
	{
		_MMU_FreeBackingPage(psDevNode, &psDevNode->sDummyPage, DUMMY_PAGE);

		OSLockDestroy(psDevNode->sDummyPage.psPgLock);
		psDevNode->sDummyPage.psPgLock = NULL;
	}

	if (psDevNode->sDevZeroPage.psPgLock)
	{
		_MMU_FreeBackingPage(psDevNode, &psDevNode->sDevZeroPage, DEV_ZERO_PAGE);


		OSLockDestroy(psDevNode->sDevZeroPage.psPgLock);
		psDevNode->sDevZeroPage.psPgLock = NULL;
	}
}

/*
	MMU_ContextCreate
 */
PVRSRV_ERROR
MMU_ContextCreate(CONNECTION_DATA *psConnection,
                  PVRSRV_DEVICE_NODE *psDevNode,
                  MMU_CONTEXT **ppsMMUContext,
                  MMU_DEVICEATTRIBS *psDevAttrs)
{
	MMU_CONTEXT *psMMUContext;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	const MMU_PxE_CONFIG *psConfig;
	MMU_PHYSMEM_CONTEXT *psPhysMemCtx;
	IMG_UINT32 ui32BaseObjects;
	IMG_UINT32 ui32Size;
	IMG_CHAR sBuf[40];
	PVRSRV_ERROR eError = PVRSRV_OK;

#if defined(PDUMP)
	PDUMPCOMMENT("MMU context create");
#endif

	psConfig = psDevAttrs->psBaseConfig;
	psDevVAddrConfig = psDevAttrs->psTopLevelDevVAddrConfig;

	switch (psDevAttrs->eTopLevel)
	{
		case MMU_LEVEL_3:
			ui32BaseObjects = psDevVAddrConfig->uiNumEntriesPC;
			break;

		case MMU_LEVEL_2:
			ui32BaseObjects = psDevVAddrConfig->uiNumEntriesPD;
			break;

		case MMU_LEVEL_1:
			ui32BaseObjects = psDevVAddrConfig->uiNumEntriesPT;
			break;

		default:
			PVR_LOG_GOTO_WITH_ERROR("psDevAttrs->eTopLevel", eError, PVRSRV_ERROR_INVALID_PARAMS, e0);
	}

	/* Allocate the MMU context with the Level 1 Px info's */
	ui32Size = sizeof(MMU_CONTEXT) +
			((ui32BaseObjects - 1) * sizeof(MMU_Levelx_INFO *));

	psMMUContext = OSAllocZMem(ui32Size);
	PVR_LOG_GOTO_IF_NOMEM(psMMUContext, eError, e0);

#if defined(PDUMP)
	/* Clear the refcount */
	psMMUContext->ui32PDumpContextIDRefCount = 0;
#endif
	/* Record Device specific attributes in the context for subsequent use */
	psMMUContext->psDevAttrs = psDevAttrs;

	/*
	  Allocate physmem context and set it up
	 */
	psPhysMemCtx = OSAllocZMem(sizeof(MMU_PHYSMEM_CONTEXT));
	PVR_LOG_GOTO_IF_NOMEM(psPhysMemCtx, eError, e1);

	psMMUContext->psPhysMemCtx = psPhysMemCtx;
	psMMUContext->psConnection = psConnection;

	psPhysMemCtx->psDevNode = psDevNode;		/* Needed for Direct Bridge case */
	psPhysMemCtx->psMMUContext = psMMUContext;	/* Back-link to self */

#if defined(SUPPORT_GPUVIRT_VALIDATION)
	/* Save the app-specific values for external reference via MMU_GetOSids. */
	if (psConnection != NULL)
	{
		psPhysMemCtx->ui32OSid     = psConnection->ui32OSid;
		psPhysMemCtx->ui32OSidReg  = psConnection->ui32OSidReg;
		psPhysMemCtx->bOSidAxiProt = psConnection->bOSidAxiProtReg;
	}
	else
	{
		/* Direct Bridge calling sequence e.g. Firmware */
		psPhysMemCtx->ui32OSid     = 0;
		psPhysMemCtx->ui32OSidReg  = 0;
		psPhysMemCtx->bOSidAxiProt = IMG_FALSE;
	}
#endif

	OSSNPrintf(sBuf, sizeof(sBuf), "pgtables %p", psPhysMemCtx);
	psPhysMemCtx->uiPhysMemRANameAllocSize = OSStringLength(sBuf)+1;
	psPhysMemCtx->pszPhysMemRAName = OSAllocMem(psPhysMemCtx->uiPhysMemRANameAllocSize);
	PVR_LOG_GOTO_IF_NOMEM(psPhysMemCtx->pszPhysMemRAName, eError, e2);

	OSStringLCopy(psPhysMemCtx->pszPhysMemRAName, sBuf, psPhysMemCtx->uiPhysMemRANameAllocSize);

	psPhysMemCtx->psPhysMemRA = RA_Create(psPhysMemCtx->pszPhysMemRAName,
	                                      /* subsequent import */
	                                      psDevNode->sDevMMUPxSetup.uiMMUPxLog2AllocGran,
	                                      RA_LOCKCLASS_1,
	                                      _MMU_PhysMem_RAImportAlloc,
	                                      _MMU_PhysMem_RAImportFree,
	                                      psPhysMemCtx, /* priv */
	                                      IMG_FALSE);
	if (psPhysMemCtx->psPhysMemRA == NULL)
	{
		OSFreeMem(psPhysMemCtx->pszPhysMemRAName);
		psPhysMemCtx->pszPhysMemRAName = NULL;
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_OUT_OF_MEMORY, e3);
	}

	/* Setup cleanup meta data to check if a MMU context
	 * has been destroyed and should not be accessed anymore */
	psPhysMemCtx->psCleanupData = OSAllocMem(sizeof(*(psPhysMemCtx->psCleanupData)));
	PVR_LOG_GOTO_IF_NOMEM(psPhysMemCtx->psCleanupData, eError, e4);

#if defined(SUPPORT_GPUVIRT_VALIDATION)
	/* Record the originating OSid for all allocation / free for this context */
	psPhysMemCtx->psCleanupData->ui32OSid = psPhysMemCtx->ui32OSid;
#endif	/* defined(SUPPORT_GPUVIRT_VALIDATION) */
	OSLockCreate(&psPhysMemCtx->psCleanupData->hCleanupLock);
	psPhysMemCtx->psCleanupData->bMMUContextExists = IMG_TRUE;
	dllist_init(&psPhysMemCtx->psCleanupData->sMMUCtxCleanupItemsHead);
	OSAtomicWrite(&psPhysMemCtx->psCleanupData->iRef, 1);

	/* allocate the base level object */
	/*
	   Note: Although this is not required by the this file until
	         the 1st allocation is made, a device specific callback
	         might request the base object address so we allocate
	         it up front.
	 */
	if (_PxMemAlloc(psMMUContext,
	                ui32BaseObjects,
	                psConfig,
	                psDevAttrs->eTopLevel,
	                &psMMUContext->sBaseLevelInfo.sMemDesc,
	                psDevAttrs->ui32BaseAlign))
	{
		PVR_LOG_GOTO_WITH_ERROR("_PxMemAlloc", eError, PVRSRV_ERROR_OUT_OF_MEMORY, e5);
	}

	dllist_init(&psMMUContext->psPhysMemCtx->sTmpMMUMappingHead);

	psMMUContext->sBaseLevelInfo.ui32NumOfEntries = ui32BaseObjects;
	psMMUContext->sBaseLevelInfo.ui32RefCount = 0;

	eError = OSLockCreate(&psMMUContext->hLock);
	PVR_LOG_GOTO_IF_ERROR(eError, "OSLockCreate", e6);

	/* return context */
	*ppsMMUContext = psMMUContext;

	return PVRSRV_OK;

e6:
	_PxMemFree(psMMUContext, &psMMUContext->sBaseLevelInfo.sMemDesc, psDevAttrs->eTopLevel);
e5:
	OSFreeMem(psPhysMemCtx->psCleanupData);
e4:
	RA_Delete(psPhysMemCtx->psPhysMemRA);
e3:
	OSFreeMem(psPhysMemCtx->pszPhysMemRAName);
e2:
	OSFreeMem(psPhysMemCtx);
e1:
	OSFreeMem(psMMUContext);
e0:
	return eError;
}

/*
	MMU_ContextDestroy
 */
void
MMU_ContextDestroy (MMU_CONTEXT *psMMUContext)
{
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
	PDLLIST_NODE psNode, psNextNode;

	PVRSRV_DEVICE_NODE *psDevNode = (PVRSRV_DEVICE_NODE *)psMMUContext->psPhysMemCtx->psDevNode;
	MMU_CTX_CLEANUP_DATA *psCleanupData = psMMUContext->psPhysMemCtx->psCleanupData;

	PVR_DPF((PVR_DBG_MESSAGE, "%s: Enter", __func__));

	if (psPVRSRVData->eServicesState == PVRSRV_SERVICES_STATE_OK)
	{
		/* There should be no way to get here with live pages unless
		   there is a bug in this module or the MM code */
		PVR_ASSERT(psMMUContext->sBaseLevelInfo.ui32RefCount == 0);
	}

	/* Cleanup lock must be acquired before MMUContext lock. Reverse order
	 * may lead to a deadlock and is reported by lockdep. */
	OSLockAcquire(psCleanupData->hCleanupLock);
	OSLockAcquire(psMMUContext->hLock);

	/* Free the top level MMU object - will be put on defer free list.
	 * This has to be done before the step below that will empty the
	 * defer-free list. */
	_PxMemFree(psMMUContext,
	           &psMMUContext->sBaseLevelInfo.sMemDesc,
	           psMMUContext->psDevAttrs->eTopLevel);

	/* Empty the temporary defer-free list of Px */
	_FreeMMUMapping(psDevNode, &psMMUContext->psPhysMemCtx->sTmpMMUMappingHead);
	PVR_ASSERT(dllist_is_empty(&psMMUContext->psPhysMemCtx->sTmpMMUMappingHead));

	/* Empty the defer free list so the cleanup thread will
	 * not have to access any MMU context related structures anymore */
	dllist_foreach_node(&psCleanupData->sMMUCtxCleanupItemsHead,
	                    psNode,
	                    psNextNode)
	{
		MMU_CLEANUP_ITEM *psCleanup = IMG_CONTAINER_OF(psNode,
		                                               MMU_CLEANUP_ITEM,
		                                               sMMUCtxCleanupItem);

		_FreeMMUMapping(psDevNode, &psCleanup->sMMUMappingHead);

		dllist_remove_node(psNode);
	}
	PVR_ASSERT(dllist_is_empty(&psCleanupData->sMMUCtxCleanupItemsHead));

	psCleanupData->bMMUContextExists = IMG_FALSE;

	/* Free physmem context */
	RA_Delete(psMMUContext->psPhysMemCtx->psPhysMemRA);
	psMMUContext->psPhysMemCtx->psPhysMemRA = NULL;
	OSFreeMem(psMMUContext->psPhysMemCtx->pszPhysMemRAName);
	psMMUContext->psPhysMemCtx->pszPhysMemRAName = NULL;

	OSFreeMem(psMMUContext->psPhysMemCtx);

	OSLockRelease(psMMUContext->hLock);

	OSLockRelease(psCleanupData->hCleanupLock);

	if (OSAtomicDecrement(&psCleanupData->iRef) == 0)
	{
		OSLockDestroy(psCleanupData->hCleanupLock);
		OSFreeMem(psCleanupData);
	}

	OSLockDestroy(psMMUContext->hLock);

	/* free the context itself. */
	OSFreeMem(psMMUContext);
	/*not nulling pointer, copy on stack*/

	PVR_DPF((PVR_DBG_MESSAGE, "%s: Exit", __func__));
}

/*
	MMU_Alloc
 */
PVRSRV_ERROR
MMU_Alloc (MMU_CONTEXT *psMMUContext,
           IMG_DEVMEM_SIZE_T uSize,
           IMG_DEVMEM_SIZE_T *puActualSize,
           IMG_UINT32 uiProtFlags,
           IMG_DEVMEM_SIZE_T uDevVAddrAlignment,
           IMG_DEV_VIRTADDR *psDevVAddr,
           IMG_UINT32 uiLog2PageSize)
{
	PVRSRV_ERROR eError;
	IMG_DEV_VIRTADDR sDevVAddrEnd;

	const MMU_PxE_CONFIG *psPDEConfig;
	const MMU_PxE_CONFIG *psPTEConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;

	MMU_DEVICEATTRIBS *psDevAttrs;
	IMG_HANDLE hPriv;

#if !defined(DEBUG)
	PVR_UNREFERENCED_PARAMETER(uDevVAddrAlignment);
#endif

	PVR_DPF((PVR_DBG_MESSAGE,
			"%s: uSize=" IMG_DEVMEM_SIZE_FMTSPEC
			", uiProtFlags=0x%x, align="IMG_DEVMEM_ALIGN_FMTSPEC,
			__func__, uSize, uiProtFlags, uDevVAddrAlignment));

	/* check params */
	PVR_LOG_RETURN_IF_INVALID_PARAM(psMMUContext, "psMMUContext");
	PVR_LOG_RETURN_IF_INVALID_PARAM(psDevVAddr, "psDevVAddr");
	PVR_LOG_RETURN_IF_INVALID_PARAM(puActualSize, "puActualSize");

	psDevAttrs = psMMUContext->psDevAttrs;

	eError = psDevAttrs->pfnGetPageSizeConfiguration(uiLog2PageSize,
	                                                 &psPDEConfig,
	                                                 &psPTEConfig,
	                                                 &psDevVAddrConfig,
	                                                 &hPriv);
	PVR_LOG_RETURN_IF_ERROR(eError, "pfnGetPageSizeConfiguration");

	/* size and alignment must be datapage granular */
	if (((psDevVAddr->uiAddr & psDevVAddrConfig->uiPageOffsetMask) != 0)
			|| ((uSize & psDevVAddrConfig->uiPageOffsetMask) != 0))
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: invalid address or size granularity",
				__func__));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	sDevVAddrEnd = *psDevVAddr;
	sDevVAddrEnd.uiAddr += uSize;

	OSLockAcquire(psMMUContext->hLock);
	eError = _AllocPageTables(psMMUContext, *psDevVAddr, sDevVAddrEnd, uiLog2PageSize);
	OSLockRelease(psMMUContext->hLock);

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: _AllocPageTables failed",
				__func__));
		return PVRSRV_ERROR_MMU_FAILED_TO_ALLOCATE_PAGETABLES;
	}

	psDevAttrs->pfnPutPageSizeConfiguration(hPriv);

	return PVRSRV_OK;
}

/*
	MMU_Free
 */
void
MMU_Free (MMU_CONTEXT *psMMUContext,
          IMG_DEV_VIRTADDR sDevVAddr,
          IMG_DEVMEM_SIZE_T uiSize,
          IMG_UINT32 uiLog2DataPageSize)
{
	IMG_DEV_VIRTADDR sDevVAddrEnd;
	MMU_PHYSMEM_CONTEXT *psPhysMemCtx;
	PVRSRV_DEV_POWER_STATE ePowerState;
	PVRSRV_ERROR eError;

	PVR_ASSERT(psMMUContext != NULL);
	PVR_LOG_RETURN_VOID_IF_FALSE(psMMUContext != NULL, "psMMUContext");

	psPhysMemCtx = psMMUContext->psPhysMemCtx;

	PVR_DPF((PVR_DBG_MESSAGE, "%s: Freeing DevVAddr " IMG_DEV_VIRTADDR_FMTSPEC,
			__func__, sDevVAddr.uiAddr));

	/* ensure the address range to free is inside the heap */
	sDevVAddrEnd = sDevVAddr;
	sDevVAddrEnd.uiAddr += uiSize;

	/* The Cleanup lock has to be taken before the MMUContext hLock to
	 * prevent deadlock scenarios. It is necessary only for parts of
	 * _SetupCleanup_FreeMMUMapping though.*/
	OSLockAcquire(psPhysMemCtx->psCleanupData->hCleanupLock);

	OSLockAcquire(psMMUContext->hLock);

	_FreePageTables(psMMUContext,
	                sDevVAddr,
	                sDevVAddrEnd,
	                uiLog2DataPageSize);

	eError = PVRSRVGetDevicePowerState(psPhysMemCtx->psDevNode, &ePowerState);
	if (eError != PVRSRV_OK)
	{
		/* Treat unknown power state as ON. */
		ePowerState = PVRSRV_DEV_POWER_STATE_ON;
	}

	if (ePowerState == PVRSRV_DEV_POWER_STATE_OFF)
	{
		_FreeMMUMapping(psPhysMemCtx->psDevNode, &psPhysMemCtx->sTmpMMUMappingHead);
	}
	else
	{
		_SetupCleanup_FreeMMUMapping(psMMUContext->psPhysMemCtx);
	}

	OSLockRelease(psMMUContext->hLock);

	OSLockRelease(psMMUContext->psPhysMemCtx->psCleanupData->hCleanupLock);

	return;

}

PVRSRV_ERROR
MMU_MapPages(MMU_CONTEXT *psMMUContext,
             PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
             IMG_DEV_VIRTADDR sDevVAddrBase,
             PMR *psPMR,
             IMG_UINT32 ui32PhysPgOffset,
             IMG_UINT32 ui32MapPageCount,
             IMG_UINT32 *paui32MapIndices,
             IMG_UINT32 uiLog2HeapPageSize)
{
	PVRSRV_ERROR eError;
	IMG_HANDLE hPriv;

	MMU_Levelx_INFO *psLevel = NULL;

	MMU_Levelx_INFO *psPrevLevel = NULL;

	IMG_UINT32 uiPTEIndex = 0;
	IMG_UINT32 uiPageSize = (1 << uiLog2HeapPageSize);
	IMG_UINT32 uiLoop = 0;
	IMG_UINT32 ui32MappedCount = 0;
	IMG_DEVMEM_OFFSET_T uiPgOffset = 0;
	IMG_UINT32 uiFlushEnd = 0, uiFlushStart = 0;

	IMG_UINT64 uiProtFlags = 0, uiProtFlagsReadOnly = 0, uiDefProtFlags=0;
	IMG_UINT64 uiDummyProtFlags = 0;
	MMU_PROTFLAGS_T uiMMUProtFlags = 0;

	const MMU_PxE_CONFIG *psConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;

	IMG_DEV_VIRTADDR sDevVAddr = sDevVAddrBase;

	IMG_DEV_PHYADDR asDevPAddr[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_BOOL abValid[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEV_PHYADDR *psDevPAddr;
	IMG_DEV_PHYADDR sDevPAddr;
	IMG_BOOL *pbValid;
	IMG_BOOL bValid;
	IMG_BOOL bDummyBacking = IMG_FALSE, bZeroBacking = IMG_FALSE;
	PVRSRV_DEVICE_NODE *psDevNode;

#if defined(PDUMP)
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicAddress[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiSymbolicAddrOffset;

	PDUMPCOMMENT("Wire up Page Table entries to point to the Data Pages (%"IMG_INT64_FMTSPECd" bytes)",
	             (IMG_UINT64)(ui32MapPageCount * uiPageSize));
#endif /*PDUMP*/

#if defined(TC_MEMORY_CONFIG) || defined(PLATO_MEMORY_CONFIG)
	/* We're aware that on TC based platforms, accesses from GPU to CPU_LOCAL
	 * allocated DevMem fail, so we forbid mapping such a PMR into device mmu */
	if (PMR_Flags(psPMR) & PVRSRV_MEMALLOCFLAG_CPU_LOCAL)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: Mapping a CPU_LOCAL PMR to device is forbidden on this platform", __func__));
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}
#endif

	/* Validate the most essential parameters */
	PVR_LOG_RETURN_IF_INVALID_PARAM(psMMUContext != NULL, "psMMUContext");
	PVR_LOG_RETURN_IF_INVALID_PARAM(psPMR != NULL, "psPMR");

	psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

	/* Allocate memory for page-frame-numbers and validity states,
	   N.B. assert could be triggered by an illegal uiSizeBytes */
	if (ui32MapPageCount > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		psDevPAddr = OSAllocMem(ui32MapPageCount * sizeof(IMG_DEV_PHYADDR));
		PVR_LOG_GOTO_IF_NOMEM(psDevPAddr, eError, ErrReturnError);

		pbValid = OSAllocMem(ui32MapPageCount * sizeof(IMG_BOOL));
		PVR_LOG_GOTO_IF_NOMEM(pbValid, eError, ErrFreePAddrMappingArray);
	}
	else
	{
		psDevPAddr = asDevPAddr;
		pbValid	= abValid;
	}

	/* Get the Device physical addresses of the pages we are trying to map
	 * In the case of non indexed mapping we can get all addresses at once */
	if (NULL == paui32MapIndices)
	{
		eError = PMR_DevPhysAddr(psPMR,
		                         uiLog2HeapPageSize,
		                         ui32MapPageCount,
		                         (ui32PhysPgOffset << uiLog2HeapPageSize),
		                         psDevPAddr,
		                         pbValid);
		PVR_GOTO_IF_ERROR(eError, ErrFreeValidArray);
	}

	/*Get the Page table level configuration */
	_MMU_GetPTConfig(psMMUContext,
	                 (IMG_UINT32) uiLog2HeapPageSize,
	                 &psConfig,
	                 &hPriv,
	                 &psDevVAddrConfig);

	eError = _MMU_ConvertDevMemFlags(IMG_FALSE,
	                                 uiMappingFlags,
	                                 &uiMMUProtFlags,
	                                 psMMUContext);
	PVR_GOTO_IF_ERROR(eError, ErrPutPTConfig);

	/* Callback to get device specific protection flags */
	if (psConfig->uiBytesPerEntry == 8)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUProtFlags , uiLog2HeapPageSize);
		uiMMUProtFlags |= MMU_PROTFLAGS_READABLE;
		uiProtFlagsReadOnly = psMMUContext->psDevAttrs->pfnDerivePTEProt8((uiMMUProtFlags & ~MMU_PROTFLAGS_WRITEABLE),
		                                                                  uiLog2HeapPageSize);
	}
	else if (psConfig->uiBytesPerEntry == 4)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUProtFlags);
		uiMMUProtFlags |= MMU_PROTFLAGS_READABLE;
		uiProtFlagsReadOnly = psMMUContext->psDevAttrs->pfnDerivePTEProt4((uiMMUProtFlags & ~MMU_PROTFLAGS_WRITEABLE));
	}
	else
	{
		PVR_LOG_GOTO_WITH_ERROR("psConfig->uiBytesPerEntry", eError, PVRSRV_ERROR_INVALID_PARAMS, ErrPutPTConfig);
	}
	uiDummyProtFlags = uiProtFlags;

	if (PMR_IsSparse(psPMR))
	{
		/* We know there will not be 4G number of PMR's */
		bDummyBacking = PVRSRV_IS_SPARSE_DUMMY_BACKING_REQUIRED(uiMappingFlags);
		if (bDummyBacking)
		{
			bZeroBacking = PVRSRV_IS_SPARSE_ZERO_BACKING_REQUIRED(uiMappingFlags);
		}

		if (PVRSRV_CHECK_GPU_CACHE_COHERENT(uiMappingFlags))
		{
			/* Obtain non-coherent protection flags as we cannot have multiple coherent
			   virtual pages pointing to the same physical page so all dummy page
			   mappings have to be non-coherent even in a coherent allocation */
			eError = _MMU_ConvertDevMemFlags(IMG_FALSE,
									uiMappingFlags & ~PVRSRV_MEMALLOCFLAG_GPU_CACHE_COHERENT,
									&uiMMUProtFlags,
									psMMUContext);
			PVR_GOTO_IF_ERROR(eError, ErrPutPTConfig);

			/* Callback to get device specific protection flags */
			if (psConfig->uiBytesPerEntry == 8)
			{
				uiDummyProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUProtFlags , uiLog2HeapPageSize);
			}
			else if (psConfig->uiBytesPerEntry == 4)
			{
				uiDummyProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUProtFlags);
			}
			else
			{
				PVR_DPF((PVR_DBG_ERROR,
				         "%s: The page table entry byte length is not supported",
				         __func__));
				eError = PVRSRV_ERROR_INVALID_PARAMS;
				goto ErrPutPTConfig;
			}
		}
	}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
	PMRMarkForDeferFree(psPMR);
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

	OSLockAcquire(psMMUContext->hLock);

	for (uiLoop = 0; uiLoop < ui32MapPageCount; uiLoop++)
	{

#if defined(PDUMP)
		IMG_DEVMEM_OFFSET_T uiNextSymName;
#endif /*PDUMP*/

		if (NULL != paui32MapIndices)
		{
			uiPgOffset = paui32MapIndices[uiLoop];

			/*Calculate the Device Virtual Address of the page */
			sDevVAddr.uiAddr = sDevVAddrBase.uiAddr + (uiPgOffset * uiPageSize);

			/* Get the physical address to map */
			eError = PMR_DevPhysAddr(psPMR,
			                         uiLog2HeapPageSize,
			                         1,
			                         uiPgOffset * uiPageSize,
			                         &sDevPAddr,
			                         &bValid);
			PVR_GOTO_IF_ERROR(eError, ErrUnlockAndUnmapPages);
		}
		else
		{
			uiPgOffset = uiLoop + ui32PhysPgOffset;
			sDevPAddr = psDevPAddr[uiLoop];
			bValid = pbValid[uiLoop];
		}

		uiDefProtFlags = uiProtFlags;
		/*
			The default value of the entry is invalid so we don't need to mark
			it as such if the page wasn't valid, we just advance pass that address
		 */
		if (bValid || bDummyBacking)
		{
			if (!bValid)
			{
				if (bZeroBacking)
				{
					eError = _MMU_GetBackingPage(psDevNode,
					                             &psDevNode->sDevZeroPage,
					                             PVR_ZERO_PAGE_INIT_VALUE,
					                             DEV_ZERO_PAGE,
					                             IMG_TRUE);
					PVR_LOG_GOTO_IF_ERROR(eError, "_MMU_GetBackingPage",
					                      ErrUnlockAndUnmapPages);

					sDevPAddr.uiAddr = psDevNode->sDevZeroPage.ui64PgPhysAddr;
					/* Ensure the zero back page PTE is read only */
					uiDefProtFlags = uiProtFlagsReadOnly;
				}
				else
				{
					eError = _MMU_GetBackingPage(psDevNode,
					                             &psDevNode->sDummyPage,
					                             PVR_DUMMY_PAGE_INIT_VALUE,
					                             DUMMY_PAGE,
					                             IMG_TRUE);
					PVR_LOG_GOTO_IF_ERROR(eError, "_MMU_GetBackingPage",
					                      ErrUnlockAndUnmapPages);

					sDevPAddr.uiAddr = psDevNode->sDummyPage.ui64PgPhysAddr;
				}
			}
			else
			{
				/* check the physical alignment of the memory to map */
				PVR_ASSERT((sDevPAddr.uiAddr & (uiPageSize-1)) == 0);
			}

#if defined(DEBUG)
			{
				IMG_INT32	i32FeatureVal = 0;
				IMG_UINT32 ui32BitLength = FloorLog2(sDevPAddr.uiAddr);

				i32FeatureVal = PVRSRV_GET_DEVICE_FEATURE_VALUE(psDevNode, PHYS_BUS_WIDTH);
				do {
					/* i32FeatureVal can be negative for cases where this feature is undefined
					 * In that situation we need to bail out than go ahead with debug comparison */
					if (0 > i32FeatureVal)
						break;

					if (ui32BitLength > i32FeatureVal)
					{
						PVR_DPF((PVR_DBG_ERROR,
								"%s Failed. The physical address bitlength (%d)"
								" is greater than the chip can handle (%d).",
								__func__, ui32BitLength, i32FeatureVal));

						PVR_ASSERT(ui32BitLength <= i32FeatureVal);
						eError = PVRSRV_ERROR_INVALID_PARAMS;
						goto ErrUnlockAndUnmapPages;
					}
				} while (0);
			}
#endif /*DEBUG*/

#if defined(PDUMP)
			if (bValid)
			{
				eError = PMR_PDumpSymbolicAddr(psPMR, uiPgOffset * uiPageSize,
				                               sizeof(aszMemspaceName), &aszMemspaceName[0],
				                               sizeof(aszSymbolicAddress), &aszSymbolicAddress[0],
				                               &uiSymbolicAddrOffset,
				                               &uiNextSymName);
				PVR_ASSERT(eError == PVRSRV_OK);
			}
#endif /*PDUMP*/

			psPrevLevel = psLevel;
			/* Calculate PT index and get new table descriptor */
			if (!_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
			                    &psLevel, &uiPTEIndex))
			{
				PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND,
				                    ErrUnlockAndUnmapPages);
			}

			if (psPrevLevel == psLevel)
			{
				/*
				 * Sparse allocations may have page offsets which
				 * decrement as well as increment, so make sure we
				 * update the range we will flush correctly.
				 */
				if (uiPTEIndex > uiFlushEnd)
					uiFlushEnd = uiPTEIndex;
				else if (uiPTEIndex < uiFlushStart)
					uiFlushStart = uiPTEIndex;
			}
			else
			{
				/* Flush if we moved to another psLevel, i.e. page table */
				if (psPrevLevel != NULL)
				{
					eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
					                                                 &psPrevLevel->sMemDesc.psMapping->sMemHandle,
					                                                 uiFlushStart * psConfig->uiBytesPerEntry + psPrevLevel->sMemDesc.uiOffset,
					                                                 (uiFlushEnd+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
					PVR_GOTO_IF_ERROR(eError, ErrUnlockAndUnmapPages);
				}

				uiFlushStart = uiPTEIndex;
				uiFlushEnd = uiFlushStart;
			}

			HTBLOGK(HTB_SF_MMU_PAGE_OP_MAP,
			        HTBLOG_U64_BITS_HIGH(sDevVAddr.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddr.uiAddr),
			        HTBLOG_U64_BITS_HIGH(sDevPAddr.uiAddr), HTBLOG_U64_BITS_LOW(sDevPAddr.uiAddr));

			/* Set the PT entry with the specified address and protection flags */
			eError = _SetupPTE(psMMUContext,
			                   psLevel,
			                   uiPTEIndex,
			                   psConfig,
			                   &sDevPAddr,
			                   IMG_FALSE,
#if defined(PDUMP)
			                   (bValid)?aszMemspaceName:(psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName),
			                          ((bValid)?aszSymbolicAddress:((bZeroBacking)?DEV_ZERO_PAGE:DUMMY_PAGE)),
			                          (bValid)?uiSymbolicAddrOffset:0,
#endif /*PDUMP*/
			                    uiDefProtFlags);
			PVR_LOG_GOTO_IF_ERROR(eError, "_SetupPTE", ErrUnlockAndUnmapPages);

			if (bValid)
			{
				PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);
				PVR_DPF ((PVR_DBG_MESSAGE,
						"%s: devVAddr=" IMG_DEV_VIRTADDR_FMTSPEC ", "
						"size=" IMG_DEVMEM_OFFSET_FMTSPEC,
						__func__,
						sDevVAddr.uiAddr,
						uiPgOffset * uiPageSize));

				ui32MappedCount++;
			}
		}

		sDevVAddr.uiAddr += uiPageSize;
	}

	/* Flush the last level we touched */
	if (psLevel != NULL)
	{
		eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
		                                                 &psLevel->sMemDesc.psMapping->sMemHandle,
		                                                 uiFlushStart * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
		                                                 (uiFlushEnd+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
		PVR_GOTO_IF_ERROR(eError, ErrUnlockAndUnmapPages);
	}

	OSLockRelease(psMMUContext->hLock);

	_MMU_PutPTConfig(psMMUContext, hPriv);

	if (psDevPAddr != asDevPAddr)
	{
		OSFreeMem(pbValid);
		OSFreeMem(psDevPAddr);
	}

	/* Flush TLB for PTs*/
	psDevNode->pfnMMUCacheInvalidate(psDevNode,
	                                 psMMUContext,
	                                 MMU_LEVEL_1,
	                                 IMG_FALSE);

#if defined(PDUMP)
	PDUMPCOMMENT("Wired up %d Page Table entries (out of %d)", ui32MappedCount, ui32MapPageCount);
#endif /*PDUMP*/

	return PVRSRV_OK;

ErrUnlockAndUnmapPages:
	(void) MMU_UnmapPagesUnlocked(psMMUContext,
	                              /* In the case of change sparse, it may want a scratch backing. */
	                              PMR_IsSparse(psPMR) ? uiMappingFlags : 0,
	                              sDevVAddrBase,
	                              uiLoop,
	                              paui32MapIndices,
	                              uiLog2HeapPageSize);

	OSLockRelease(psMMUContext->hLock);
ErrPutPTConfig:
	_MMU_PutPTConfig(psMMUContext, hPriv);
ErrFreeValidArray:
	if (pbValid != abValid)
	{
		OSFreeMem(pbValid);
	}
ErrFreePAddrMappingArray:
	if (psDevPAddr != asDevPAddr)
	{
		OSFreeMem(psDevPAddr);
	}
ErrReturnError:
	return eError;
}

/* Unmap the pages or map them to the backing pages
 * if the `uiMappingFlags` request it. */
static PVRSRV_ERROR
MMU_UnmapPagesUnlocked(MMU_CONTEXT *psMMUContext,
                       PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
                       IMG_DEV_VIRTADDR sDevVAddrBase,
                       IMG_UINT32 ui32PageCount,
                       IMG_UINT32 *pai32FreeIndices,
                       IMG_UINT32 uiLog2PageSize)
{
	PVRSRV_ERROR eError;
	IMG_UINT32 uiPTEIndex = 0, ui32Loop=0;
	IMG_UINT32 uiPageSize = 1 << uiLog2PageSize;
	IMG_UINT32 uiFlushEnd = 0, uiFlushStart = 0;
	MMU_Levelx_INFO *psLevel = NULL;
	MMU_Levelx_INFO *psPrevLevel = NULL;
	IMG_HANDLE hPriv;
	const MMU_PxE_CONFIG *psConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	IMG_UINT64 uiProtFlags = 0, uiProtFlagsReadOnly = 0;
	MMU_PROTFLAGS_T uiMMUProtFlags = 0, uiMMUReadOnlyProtFlags = 0;
	IMG_DEV_VIRTADDR sDevVAddr = sDevVAddrBase;
	IMG_DEV_PHYADDR sBackingPgDevPhysAddr;
	IMG_BOOL bUnmap = IMG_TRUE, bDummyBacking = IMG_FALSE, bZeroBacking = IMG_FALSE;
	IMG_CHAR *pcBackingPageName = NULL;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

#if defined(PDUMP)
	PDUMPCOMMENT("Invalidate %d entries in page tables for virtual range: 0x%010"IMG_UINT64_FMTSPECX" to 0x%010"IMG_UINT64_FMTSPECX,
	             ui32PageCount,
	             (IMG_UINT64)sDevVAddr.uiAddr,
	             ((IMG_UINT64)sDevVAddr.uiAddr) + (uiPageSize*ui32PageCount)-1);
#endif

	PVR_ASSERT(OSLockIsLocked(psMMUContext->hLock));

	bDummyBacking = PVRSRV_IS_SPARSE_DUMMY_BACKING_REQUIRED(uiMappingFlags);
	bZeroBacking = PVRSRV_IS_SPARSE_ZERO_BACKING_REQUIRED(uiMappingFlags);

	if (bZeroBacking)
	{
		sBackingPgDevPhysAddr.uiAddr = psDevNode->sDevZeroPage.ui64PgPhysAddr;
		pcBackingPageName = DEV_ZERO_PAGE;
	}
	else
	{
		sBackingPgDevPhysAddr.uiAddr = psDevNode->sDummyPage.ui64PgPhysAddr;
		pcBackingPageName = DUMMY_PAGE;
	}

	bUnmap = (uiMappingFlags)? !bDummyBacking : IMG_TRUE;
	/* Get PT and address configs */
	_MMU_GetPTConfig(psMMUContext, (IMG_UINT32) uiLog2PageSize,
	                 &psConfig, &hPriv, &psDevVAddrConfig);

	eError = _MMU_ConvertDevMemFlags(bUnmap,
	                                 uiMappingFlags,
	                                 &uiMMUProtFlags,
	                                 psMMUContext);
	PVR_RETURN_IF_ERROR(eError);

	uiMMUReadOnlyProtFlags = (uiMMUProtFlags & ~MMU_PROTFLAGS_WRITEABLE) | MMU_PROTFLAGS_READABLE;

	/* Callback to get device specific protection flags */
	if (psConfig->uiBytesPerEntry == 4)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUProtFlags);
		uiProtFlagsReadOnly = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUReadOnlyProtFlags);
	}
	else if (psConfig->uiBytesPerEntry == 8)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUProtFlags , uiLog2PageSize);
		uiProtFlagsReadOnly = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUReadOnlyProtFlags, uiLog2PageSize);
	}

	/* Unmap page by page */
	while (ui32Loop < ui32PageCount)
	{
		if (NULL != pai32FreeIndices)
		{
			/*Calculate the Device Virtual Address of the page */
			sDevVAddr.uiAddr = sDevVAddrBase.uiAddr +
					pai32FreeIndices[ui32Loop] * (IMG_UINT64) uiPageSize;
		}

		psPrevLevel = psLevel;
		/* Calculate PT index and get new table descriptor */
		if (!_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
		                    &psLevel, &uiPTEIndex))
		{
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND, e0);
		}

		if (psPrevLevel == psLevel)
		{
			/*
			 * Sparse allocations may have page offsets which
			 * decrement as well as increment, so make sure we
			 * update the range we will flush correctly.
			 */
			if (uiPTEIndex > uiFlushEnd)
				uiFlushEnd = uiPTEIndex;
			else if (uiPTEIndex < uiFlushStart)
				uiFlushStart = uiPTEIndex;
		}
		else
		{
			/* Flush if we moved to another psLevel, i.e. page table */
			if (psPrevLevel != NULL)
			{
				psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
				                                        &psPrevLevel->sMemDesc.psMapping->sMemHandle,
				                                        uiFlushStart * psConfig->uiBytesPerEntry + psPrevLevel->sMemDesc.uiOffset,
				                                        (uiFlushEnd+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
			}

			uiFlushStart = uiPTEIndex;
			uiFlushEnd = uiFlushStart;
		}

		HTBLOGK(HTB_SF_MMU_PAGE_OP_UNMAP,
		        HTBLOG_U64_BITS_HIGH(sDevVAddr.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddr.uiAddr));

		/* Set the PT entry to invalid and poison it with a bad address */
		eError = _SetupPTE(psMMUContext,
		                   psLevel,
		                   uiPTEIndex,
		                   psConfig,
		                   (bDummyBacking)? &sBackingPgDevPhysAddr : &gsBadDevPhyAddr,
		                   bUnmap,
#if defined(PDUMP)
		                   (bDummyBacking)? (psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName): NULL,
		                   (bDummyBacking)? pcBackingPageName: NULL,
		                   0U,
#endif
		                   (bZeroBacking)? uiProtFlagsReadOnly: uiProtFlags);
		PVR_GOTO_IF_ERROR(eError, e0);

		/* Check we haven't wrapped around */
		PVR_ASSERT(psLevel->ui32RefCount <= psLevel->ui32NumOfEntries);
		ui32Loop++;
		sDevVAddr.uiAddr += uiPageSize;
	}

	/* Flush the last level we touched */
	if (psLevel != NULL)
	{
		psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
		                                        &psLevel->sMemDesc.psMapping->sMemHandle,
		                                        uiFlushStart * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
		                                        (uiFlushEnd+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
	}

	_MMU_PutPTConfig(psMMUContext, hPriv);

	/* Flush TLB for PTs*/
	psDevNode->pfnMMUCacheInvalidate(psDevNode,
	                                 psMMUContext,
	                                 MMU_LEVEL_1,
	                                 IMG_TRUE);

	return PVRSRV_OK;

e0:
	_MMU_PutPTConfig(psMMUContext, hPriv);
	PVR_DPF((PVR_DBG_ERROR, "%s: Failed to map/unmap page table "
	         "with error %u", __func__, eError));

	return eError;
}

/*
	MMU_UnmapPages
 */
PVRSRV_ERROR
MMU_UnmapPages(MMU_CONTEXT *psMMUContext,
               PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
               IMG_DEV_VIRTADDR sDevVAddrBase,
               IMG_UINT32 ui32PageCount,
               IMG_UINT32 *pai32FreeIndices,
               IMG_UINT32 uiLog2PageSize)
{
	PVRSRV_ERROR eError;

	OSLockAcquire(psMMUContext->hLock);

	eError = MMU_UnmapPagesUnlocked(psMMUContext,
	                                uiMappingFlags,
	                                sDevVAddrBase,
	                                ui32PageCount,
	                                pai32FreeIndices,
	                                uiLog2PageSize);

	OSLockRelease(psMMUContext->hLock);

	return eError;
}

PVRSRV_ERROR
MMU_MapPMRFast (MMU_CONTEXT *psMMUContext,
                IMG_DEV_VIRTADDR sDevVAddrBase,
                PMR *psPMR,
                IMG_DEVMEM_SIZE_T uiSizeBytes,
                PVRSRV_MEMALLOCFLAGS_T uiMappingFlags,
                IMG_UINT32 uiLog2HeapPageSize)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_UINT32 uiCount, i;
	IMG_UINT32 uiPageSize = 1 << uiLog2HeapPageSize;
	IMG_UINT32 uiPTEIndex = 0;
	IMG_UINT64 uiProtFlags;
	MMU_PROTFLAGS_T uiMMUProtFlags = 0;
	MMU_Levelx_INFO *psLevel = NULL;
	IMG_HANDLE hPriv;
	const MMU_PxE_CONFIG *psConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	IMG_DEV_VIRTADDR sDevVAddr = sDevVAddrBase;
	IMG_DEV_PHYADDR asDevPAddr[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_BOOL abValid[PMR_MAX_TRANSLATION_STACK_ALLOC];
	IMG_DEV_PHYADDR *psDevPAddr;
	IMG_BOOL *pbValid;
	IMG_UINT32 uiFlushStart = 0;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

#if defined(PDUMP)
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicAddress[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiSymbolicAddrOffset;
	IMG_UINT32 ui32MappedCount = 0;
	PDUMPCOMMENT("Wire up Page Table entries to point to the Data Pages (%"IMG_INT64_FMTSPECd" bytes)", uiSizeBytes);
#endif /*PDUMP*/

	/* We should verify the size and contiguity when supporting variable page size */

	PVR_LOG_RETURN_IF_INVALID_PARAM(psMMUContext != NULL, "psMMUContext");
	PVR_LOG_RETURN_IF_INVALID_PARAM(psPMR != NULL, "psPMR");

#if defined(TC_MEMORY_CONFIG) || defined(PLATO_MEMORY_CONFIG)
	/* We're aware that on TC based platforms, accesses from GPU to CPU_LOCAL
	 * allocated DevMem fail, so we forbid mapping such a PMR into device mmu */
	if (PMR_Flags(psPMR) & PVRSRV_MEMALLOCFLAG_CPU_LOCAL)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: Mapping a CPU_LOCAL PMR to device is forbidden on this platform", __func__));
		return PVRSRV_ERROR_PMR_NOT_PERMITTED;
	}
#endif

	/* Allocate memory for page-frame-numbers and validity states,
	   N.B. assert could be triggered by an illegal uiSizeBytes */
	uiCount = uiSizeBytes >> uiLog2HeapPageSize;
	PVR_ASSERT((IMG_DEVMEM_OFFSET_T)uiCount << uiLog2HeapPageSize == uiSizeBytes);
	if (uiCount > PMR_MAX_TRANSLATION_STACK_ALLOC)
	{
		psDevPAddr = OSAllocMem(uiCount * sizeof(IMG_DEV_PHYADDR));
		PVR_LOG_GOTO_IF_NOMEM(psDevPAddr, eError, return_error);

		pbValid = OSAllocMem(uiCount * sizeof(IMG_BOOL));
		PVR_LOG_GOTO_IF_NOMEM(pbValid, eError, free_paddr_array);
	}
	else
	{
		psDevPAddr = asDevPAddr;
		pbValid	= abValid;
	}

	/* Get general PT and address configs */
	_MMU_GetPTConfig(psMMUContext, (IMG_UINT32) uiLog2HeapPageSize,
	                 &psConfig, &hPriv, &psDevVAddrConfig);

	eError = _MMU_ConvertDevMemFlags(IMG_FALSE,
	                                 uiMappingFlags,
	                                 &uiMMUProtFlags,
	                                 psMMUContext);
	PVR_GOTO_IF_ERROR(eError, put_mmu_context);

	/* Callback to get device specific protection flags */

	if (psConfig->uiBytesPerEntry == 8)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUProtFlags , uiLog2HeapPageSize);
	}
	else if (psConfig->uiBytesPerEntry == 4)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUProtFlags);
	}
	else
	{
		PVR_LOG_GOTO_WITH_ERROR("psConfig->uiBytesPerEntry", eError, PVRSRV_ERROR_MMU_CONFIG_IS_WRONG, put_mmu_context);
	}


	/* "uiSize" is the amount of contiguity in the underlying
	   page.  Normally this would be constant for the system, but,
	   that constant needs to be communicated, in case it's ever
	   different; caller guarantees that PMRLockSysPhysAddr() has
	   already been called */
	eError = PMR_DevPhysAddr(psPMR,
	                         uiLog2HeapPageSize,
	                         uiCount,
	                         0,
	                         psDevPAddr,
	                         pbValid);
	PVR_GOTO_IF_ERROR(eError, put_mmu_context);

#if defined(SUPPORT_PMR_DEFERRED_FREE)
	PMRMarkForDeferFree(psPMR);
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

	OSLockAcquire(psMMUContext->hLock);

	if (!_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
	                    &psLevel, &uiPTEIndex))
	{
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND,
		                    unlock_mmu_context);
	}
	uiFlushStart = uiPTEIndex;

	/* Map in all pages of that PMR page by page*/
	for (i=0, uiCount=0; uiCount < uiSizeBytes; i++)
	{
#if defined(DEBUG)
		{
			IMG_INT32	i32FeatureVal = 0;
			IMG_UINT32 ui32BitLength = FloorLog2(psDevPAddr[i].uiAddr);
			i32FeatureVal = PVRSRV_GET_DEVICE_FEATURE_VALUE(psDevNode, PHYS_BUS_WIDTH);
			do {
				if (0 > i32FeatureVal)
					break;

				if (ui32BitLength > i32FeatureVal)
				{
					PVR_DPF((PVR_DBG_ERROR,
							"%s Failed. The physical address bitlength (%d)"
							" is greater than the chip can handle (%d).",
							__func__, ui32BitLength, i32FeatureVal));

					PVR_ASSERT(ui32BitLength <= i32FeatureVal);
					OSLockRelease(psMMUContext->hLock);
					PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_INVALID_PARAMS, put_mmu_context);
				}
			} while (0);
		}
#endif /*DEBUG*/
#if defined(PDUMP)
		{
			IMG_DEVMEM_OFFSET_T uiNextSymName;

			eError = PMR_PDumpSymbolicAddr(psPMR, uiCount,
			                               sizeof(aszMemspaceName), &aszMemspaceName[0],
			                               sizeof(aszSymbolicAddress), &aszSymbolicAddress[0],
			                               &uiSymbolicAddrOffset,
			                               &uiNextSymName);
			PVR_ASSERT(eError == PVRSRV_OK);
			ui32MappedCount++;
		}
#endif /*PDUMP*/

		HTBLOGK(HTB_SF_MMU_PAGE_OP_PMRMAP,
		        HTBLOG_U64_BITS_HIGH(sDevVAddr.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddr.uiAddr),
		        HTBLOG_U64_BITS_HIGH(psDevPAddr[i].uiAddr), HTBLOG_U64_BITS_LOW(psDevPAddr[i].uiAddr));

		/* Set the PT entry with the specified address and protection flags */
		eError = _SetupPTE(psMMUContext, psLevel, uiPTEIndex,
		                   psConfig, &psDevPAddr[i], IMG_FALSE,
#if defined(PDUMP)
		                   aszMemspaceName,
		                   aszSymbolicAddress,
		                   uiSymbolicAddrOffset,
#endif /*PDUMP*/
		                   uiProtFlags);
		PVR_GOTO_IF_ERROR(eError, unlock_mmu_context);

		sDevVAddr.uiAddr += uiPageSize;
		uiCount += uiPageSize;

		/* Calculate PT index and get new table descriptor */
		if (uiPTEIndex < (psDevVAddrConfig->uiNumEntriesPT - 1) && (uiCount != uiSizeBytes))
		{
			uiPTEIndex++;
		}
		else
		{
			eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
			                                                 &psLevel->sMemDesc.psMapping->sMemHandle,
			                                                 uiFlushStart * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
			                                                 (uiPTEIndex+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
			PVR_GOTO_IF_ERROR(eError, unlock_mmu_context);

			/* If this is not the last PTE in the PT and _MMU_GetPTInfo() fails
			 * return an error, otherwise ignore lookup result as it is not used
			 * on the last loop iteration */
			if ((uiCount != uiSizeBytes) &&
			    !_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
			                    &psLevel, &uiPTEIndex))
			{
				PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND,
				                    unlock_mmu_context);
			}
			uiFlushStart = uiPTEIndex;
		}
	}

	OSLockRelease(psMMUContext->hLock);


	_MMU_PutPTConfig(psMMUContext, hPriv);

	if (psDevPAddr != asDevPAddr)
	{
		OSFreeMem(pbValid);
		OSFreeMem(psDevPAddr);
	}

	/* Flush TLB for PTs*/
	psDevNode->pfnMMUCacheInvalidate(psDevNode,
	                                 psMMUContext,
	                                 MMU_LEVEL_1,
	                                 IMG_FALSE);

#if defined(PDUMP)
	PDUMPCOMMENT("Wired up %d Page Table entries (out of %d)", ui32MappedCount, i);
#endif /*PDUMP*/

	return PVRSRV_OK;

unlock_mmu_context:
	/* Unmap starting from the address passed as an argument. */
	(void) MMU_UnmapPMRFastUnlocked(psMMUContext,
	                                sDevVAddrBase,
	                                uiSizeBytes >> uiLog2HeapPageSize,
	                                uiLog2HeapPageSize);
	OSLockRelease(psMMUContext->hLock);

put_mmu_context:
	_MMU_PutPTConfig(psMMUContext, hPriv);

	if (pbValid != abValid)
	{
		OSFreeMem(pbValid);
	}

free_paddr_array:
	if (psDevPAddr != asDevPAddr)
	{
		OSFreeMem(psDevPAddr);
	}

return_error:
	return eError;
}

static PVRSRV_ERROR
MMU_UnmapPMRFastUnlocked(MMU_CONTEXT *psMMUContext,
                         IMG_DEV_VIRTADDR sDevVAddrBase,
                         IMG_UINT32 ui32PageCount,
                         IMG_UINT32 uiLog2PageSize)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_UINT32 uiPTEIndex = 0, ui32Loop=0;
	IMG_UINT32 uiPageSize = 1 << uiLog2PageSize;
	MMU_Levelx_INFO *psLevel = NULL;
	IMG_HANDLE hPriv;
	const MMU_PxE_CONFIG *psConfig;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	IMG_DEV_VIRTADDR sDevVAddr = sDevVAddrBase;
	IMG_UINT64 uiProtFlags = 0;
	MMU_PROTFLAGS_T uiMMUProtFlags = 0;
	IMG_UINT64 uiEntry = 0;
	IMG_UINT32 uiFlushStart = 0;
	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

#if defined(PDUMP)
	PDUMPCOMMENT("Invalidate %d entries in page tables for virtual range: 0x%010"IMG_UINT64_FMTSPECX" to 0x%010"IMG_UINT64_FMTSPECX,
	             ui32PageCount,
	             (IMG_UINT64)sDevVAddr.uiAddr,
	             ((IMG_UINT64)sDevVAddr.uiAddr) + (uiPageSize*ui32PageCount)-1);
#endif

	PVR_ASSERT(OSLockIsLocked(psMMUContext->hLock));

	/* Get PT and address configs */
	_MMU_GetPTConfig(psMMUContext, (IMG_UINT32) uiLog2PageSize,
	                 &psConfig, &hPriv, &psDevVAddrConfig);

	eError = _MMU_ConvertDevMemFlags(IMG_TRUE,
	                                 0,
	                                 &uiMMUProtFlags,
	                                 psMMUContext);
	PVR_RETURN_IF_ERROR(eError);

	/* Callback to get device specific protection flags */

	if (psConfig->uiBytesPerEntry == 8)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt8(uiMMUProtFlags , uiLog2PageSize);

		/* Fill the entry with a bad address but leave space for protection flags */
		uiEntry = (gsBadDevPhyAddr.uiAddr & ~psConfig->uiProtMask) | uiProtFlags;
	}
	else if (psConfig->uiBytesPerEntry == 4)
	{
		uiProtFlags = psMMUContext->psDevAttrs->pfnDerivePTEProt4(uiMMUProtFlags);

		/* Fill the entry with a bad address but leave space for protection flags */
		uiEntry = (((IMG_UINT32) gsBadDevPhyAddr.uiAddr) & ~psConfig->uiProtMask) | (IMG_UINT32) uiProtFlags;
	}
	else
	{
		PVR_DPF((PVR_DBG_ERROR,
				"%s: The page table entry byte length is not supported",
				__func__));
		goto e0;
	}

	if (!_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
	                    &psLevel, &uiPTEIndex))
	{
		PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND, e0);
	}
	uiFlushStart = uiPTEIndex;

	/* Unmap page by page and keep the loop as quick as possible.
	 * Only use parts of _SetupPTE that need to be executed. */
	while (ui32Loop < ui32PageCount)
	{

		/* Set the PT entry to invalid and poison it with a bad address */
		if (psConfig->uiBytesPerEntry == 8)
		{
			((IMG_UINT64*)psLevel->sMemDesc.pvCpuVAddr)[uiPTEIndex] = uiEntry;
		}
		else if (psConfig->uiBytesPerEntry == 4)
		{
			((IMG_UINT32*)psLevel->sMemDesc.pvCpuVAddr)[uiPTEIndex] = (IMG_UINT32) uiEntry;
		}
		else
		{
			PVR_DPF((PVR_DBG_ERROR,
					"%s: The page table entry byte length is not supported",
					__func__));
			goto e1;
		}

		/* Log modifications */
		HTBLOGK(HTB_SF_MMU_PAGE_OP_UNMAP,
		        HTBLOG_U64_BITS_HIGH(sDevVAddr.uiAddr), HTBLOG_U64_BITS_LOW(sDevVAddr.uiAddr));

		HTBLOGK(HTB_SF_MMU_PAGE_OP_TABLE,
		        HTBLOG_PTR_BITS_HIGH(psLevel), HTBLOG_PTR_BITS_LOW(psLevel),
		        uiPTEIndex, MMU_LEVEL_1,
		        HTBLOG_U64_BITS_HIGH(uiEntry), HTBLOG_U64_BITS_LOW(uiEntry),
		        IMG_FALSE);

#if defined(PDUMP)
		PDumpMMUDumpPxEntries(MMU_LEVEL_1,
		                      psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
		                      psLevel->sMemDesc.pvCpuVAddr,
		                      psLevel->sMemDesc.sDevPAddr,
		                      uiPTEIndex,
		                      1,
		                      NULL,
		                      NULL,
		                      0,
		                      psConfig->uiBytesPerEntry,
		                      psConfig->uiAddrLog2Align,
		                      psConfig->uiAddrShift,
		                      psConfig->uiAddrMask,
		                      psConfig->uiProtMask,
		                      psConfig->uiValidEnMask,
		                      0,
		                      psMMUContext->psDevAttrs->eMMUType);
#endif /*PDUMP*/

		sDevVAddr.uiAddr += uiPageSize;
		ui32Loop++;

		/* Calculate PT index and get new table descriptor */
		if (uiPTEIndex < (psDevVAddrConfig->uiNumEntriesPT - 1) && (ui32Loop != ui32PageCount))
		{
			uiPTEIndex++;
		}
		else
		{
			psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
			                                        &psLevel->sMemDesc.psMapping->sMemHandle,
			                                        uiFlushStart * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
			                                        (uiPTEIndex+1 - uiFlushStart) * psConfig->uiBytesPerEntry);

			/* If this is not the last PTE in the PT and _MMU_GetPTInfo() fails
			 * return an error, otherwise ignore lookup result as it is not used
			 * on the last loop iteration */
			if ((ui32Loop != ui32PageCount) &&
			    !_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
			                    &psLevel, &uiPTEIndex))
			{
				PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_MAPPING_NOT_FOUND, e0);
			}
			uiFlushStart = uiPTEIndex;
		}
	}

	_MMU_PutPTConfig(psMMUContext, hPriv);

	/* Flush TLB for PTs*/
	psDevNode->pfnMMUCacheInvalidate(psDevNode,
	                                 psMMUContext,
	                                 MMU_LEVEL_1,
	                                 IMG_TRUE);

	return PVRSRV_OK;

e1:
	OSLockRelease(psMMUContext->hLock);
	_MMU_PutPTConfig(psMMUContext, hPriv);
e0:
	PVR_DPF((PVR_DBG_ERROR, "%s: Failed to map/unmap page table with error %u",
	         __func__, eError));

	return eError;
}

/*
    MMU_UnmapPMRFast
 */
PVRSRV_ERROR
MMU_UnmapPMRFast(MMU_CONTEXT *psMMUContext,
                 IMG_DEV_VIRTADDR sDevVAddrBase,
                 IMG_UINT32 ui32PageCount,
                 IMG_UINT32 uiLog2PageSize)
{
	PVRSRV_ERROR eError;

	OSLockAcquire(psMMUContext->hLock);

	eError = MMU_UnmapPMRFastUnlocked(psMMUContext,
                                      sDevVAddrBase,
                                      ui32PageCount,
                                      uiLog2PageSize);

	OSLockRelease(psMMUContext->hLock);

	return eError;
}

/*
	MMU_ChangeValidity
 */
PVRSRV_ERROR
MMU_ChangeValidity(MMU_CONTEXT *psMMUContext,
                   IMG_DEV_VIRTADDR sDevVAddr,
                   IMG_DEVMEM_SIZE_T uiNumPages,
                   IMG_UINT32 uiLog2PageSize,
                   IMG_BOOL bMakeValid,
                   PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	IMG_HANDLE hPriv;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;
	const MMU_PxE_CONFIG *psConfig;
	MMU_Levelx_INFO *psLevel = NULL;
	IMG_UINT32 uiFlushStart = 0;
	IMG_UINT32 uiPTIndex = 0;
	IMG_UINT32 i;
	IMG_UINT32 uiPageSize = 1 << uiLog2PageSize;
	IMG_BOOL bValid;

	PVRSRV_DEVICE_NODE *psDevNode = psMMUContext->psPhysMemCtx->psDevNode;

#if defined(PDUMP)
	IMG_CHAR aszMemspaceName[PHYSMEM_PDUMP_MEMSPACE_MAX_LENGTH];
	IMG_CHAR aszSymbolicAddress[PHYSMEM_PDUMP_SYMNAME_MAX_LENGTH];
	IMG_DEVMEM_OFFSET_T uiSymbolicAddrOffset;
	IMG_DEVMEM_OFFSET_T uiNextSymName;

	PDUMPCOMMENT("Change valid bit of the data pages to %d (0x%"IMG_UINT64_FMTSPECX" - 0x%"IMG_UINT64_FMTSPECX")",
	             bMakeValid,
	             sDevVAddr.uiAddr,
	             sDevVAddr.uiAddr + (uiNumPages<<uiLog2PageSize) - 1);
#endif /*PDUMP*/

	/* We should verify the size and contiguity when supporting variable page size */
	PVR_ASSERT (psMMUContext != NULL);
	PVR_ASSERT (psPMR != NULL);

	/* Get general PT and address configs */
	_MMU_GetPTConfig(psMMUContext, (IMG_UINT32) uiLog2PageSize,
	                 &psConfig, &hPriv, &psDevVAddrConfig);

	_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
	               &psLevel, &uiPTIndex);
	uiFlushStart = uiPTIndex;

	/* Do a page table walk and change attribute for every page in range. */
	for (i=0; i < uiNumPages;)
	{
		/* Set the entry */
		if (bMakeValid)
		{
			/* Only set valid if physical address exists (sparse allocs might have none)*/
			eError = PMR_IsOffsetValid(psPMR, uiLog2PageSize, 1, (IMG_DEVMEM_OFFSET_T) i << uiLog2PageSize, &bValid);
			PVR_LOG_GOTO_IF_ERROR(eError, "PMR_IsOffsetValid", e_exit);

			if (bValid)
			{
				if (psConfig->uiBytesPerEntry == 8)
				{
					((IMG_UINT64 *)psLevel->sMemDesc.pvCpuVAddr)[uiPTIndex] |= (psConfig->uiValidEnMask);
				}
				else if (psConfig->uiBytesPerEntry == 4)
				{
					((IMG_UINT32 *)psLevel->sMemDesc.pvCpuVAddr)[uiPTIndex] |= (psConfig->uiValidEnMask);
				}
				else
				{
					PVR_LOG_GOTO_WITH_ERROR("psConfig->uiBytesPerEntry", eError, PVRSRV_ERROR_MMU_CONFIG_IS_WRONG, e_exit);
				}
			}
		}
		else
		{
			if (psConfig->uiBytesPerEntry == 8)
			{
				((IMG_UINT64 *)psLevel->sMemDesc.pvCpuVAddr)[uiPTIndex] &= ~(psConfig->uiValidEnMask);
			}
			else if (psConfig->uiBytesPerEntry == 4)
			{
				((IMG_UINT32 *)psLevel->sMemDesc.pvCpuVAddr)[uiPTIndex] &= ~(psConfig->uiValidEnMask);
			}
			else
			{
				PVR_LOG_GOTO_WITH_ERROR("psConfig->uiBytesPerEntry", eError, PVRSRV_ERROR_MMU_CONFIG_IS_WRONG, e_exit);
			}
		}

#if defined(PDUMP)

		PMR_PDumpSymbolicAddr(psPMR, i<<uiLog2PageSize,
		                      sizeof(aszMemspaceName), &aszMemspaceName[0],
		                      sizeof(aszSymbolicAddress), &aszSymbolicAddress[0],
		                      &uiSymbolicAddrOffset,
		                      &uiNextSymName);

		PDumpMMUDumpPxEntries(MMU_LEVEL_1,
		                      psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName,
		                      psLevel->sMemDesc.pvCpuVAddr,
		                      psLevel->sMemDesc.sDevPAddr,
		                      uiPTIndex,
		                      1,
		                      aszMemspaceName,
		                      aszSymbolicAddress,
		                      uiSymbolicAddrOffset,
		                      psConfig->uiBytesPerEntry,
		                      psConfig->uiAddrLog2Align,
		                      psConfig->uiAddrShift,
		                      psConfig->uiAddrMask,
		                      psConfig->uiProtMask,
		                      psConfig->uiValidEnMask,
		                      0,
		                      psMMUContext->psDevAttrs->eMMUType);
#endif /*PDUMP*/

		sDevVAddr.uiAddr += uiPageSize;
		i++;

		/* Calculate PT index and get new table descriptor */
		if (uiPTIndex < (psDevVAddrConfig->uiNumEntriesPT - 1) && (i != uiNumPages))
		{
			uiPTIndex++;
		}
		else
		{

			eError = psDevNode->sDevMMUPxSetup.pfnDevPxClean(psDevNode,
			                                                 &psLevel->sMemDesc.psMapping->sMemHandle,
			                                                 uiFlushStart * psConfig->uiBytesPerEntry + psLevel->sMemDesc.uiOffset,
			                                                 (uiPTIndex+1 - uiFlushStart) * psConfig->uiBytesPerEntry);
			PVR_GOTO_IF_ERROR(eError, e_exit);

			_MMU_GetPTInfo(psMMUContext, sDevVAddr, psDevVAddrConfig,
			               &psLevel, &uiPTIndex);
			uiFlushStart = uiPTIndex;
		}
	}

e_exit:

	_MMU_PutPTConfig(psMMUContext, hPriv);

	/* Flush TLB for PTs*/
	psDevNode->pfnMMUCacheInvalidate(psDevNode,
	                                 psMMUContext,
	                                 MMU_LEVEL_1,
	                                 !bMakeValid);

	PVR_ASSERT(eError == PVRSRV_OK);
	return eError;
}


/*
	MMU_AcquireBaseAddr
 */
PVRSRV_ERROR
MMU_AcquireBaseAddr(MMU_CONTEXT *psMMUContext, IMG_DEV_PHYADDR *psPhysAddr)
{
	if (!psMMUContext)
	{
		psPhysAddr->uiAddr = 0;
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	*psPhysAddr = psMMUContext->sBaseLevelInfo.sMemDesc.sDevPAddr;

	return PVRSRV_OK;
}

/*
	MMU_ReleaseBaseAddr
 */
void
MMU_ReleaseBaseAddr(MMU_CONTEXT *psMMUContext)
{
	PVR_UNREFERENCED_PARAMETER(psMMUContext);
}

/*
	MMU_AppendCacheFlags
*/

void MMU_AppendCacheFlags(MMU_CONTEXT *psMMUContext, IMG_UINT32 ui32AppendFlags)
{
	PVR_ASSERT(psMMUContext != NULL);

	if (psMMUContext == NULL)
	{
		return;
	}

	OSAtomicOr(&psMMUContext->sCacheFlags, (IMG_INT)ui32AppendFlags);
}

/*
	MMU_GetAndResetCacheFlags
*/
IMG_UINT32 MMU_GetAndResetCacheFlags(MMU_CONTEXT *psMMUContext)
{
	IMG_UINT32 uiFlags;

	PVR_ASSERT(psMMUContext != NULL);
	if (psMMUContext == NULL)
	{
		return 0;
	}

	uiFlags = (IMG_UINT32) OSAtomicExchange(&psMMUContext->sCacheFlags, 0);

#if defined(SUPPORT_PMR_DEFERRED_FREE)
	/* kick cleanup thread to free all zombie PMRs residing in the device's
	 * zombie list */
	if (PMRQueueZombiesForCleanup(psMMUContext->psPhysMemCtx->psDevNode))
	{
		BITMASK_SET(uiFlags, RGXFWIF_MMUCACHEDATA_FLAGS_CTX_ALL | RGXFWIF_MMUCACHEDATA_FLAGS_PT);
	}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

	return uiFlags;
}

#if defined(SUPPORT_GPUVIRT_VALIDATION)
/*
    MMU_GetOSids
 */

void MMU_GetOSids(MMU_CONTEXT *psMMUContext, IMG_UINT32 *pui32OSid, IMG_UINT32 *pui32OSidReg, IMG_BOOL *pbOSidAxiProt)
{
	*pui32OSid     = psMMUContext->psPhysMemCtx->ui32OSid;
	*pui32OSidReg  = psMMUContext->psPhysMemCtx->ui32OSidReg;
	*pbOSidAxiProt = psMMUContext->psPhysMemCtx->bOSidAxiProt;

	return;
}

#endif

/*
	MMU_CheckFaultAddress
 */
void MMU_CheckFaultAddress(MMU_CONTEXT *psMMUContext,
                           IMG_DEV_VIRTADDR *psDevVAddr,
                           MMU_FAULT_DATA *psOutFaultData)
{
	/* Ideally the RGX defs should be via callbacks, but the function is only called from RGX. */
#if defined(SUPPORT_RGX)
# define MMU_MASK_VALID_FOR_32BITS(level) \
		((RGX_MMUCTRL_##level##_DATA_ENTRY_PENDING_EN | \
		  RGX_MMUCTRL_##level##_DATA_VALID_EN) <= 0xFFFFFFFF)
#define MMU_VALID_STR(entry,level) \
		(apszMMUValidStr[((((entry)&(RGX_MMUCTRL_##level##_DATA_ENTRY_PENDING_EN))!=0) << 1)| \
		                 ((((entry)&(RGX_MMUCTRL_##level##_DATA_VALID_EN))!=0) << 0)])
	static const IMG_PCHAR apszMMUValidStr[1<<2] =  {/*--*/ "not valid",
	                                                 /*-V*/ "valid",
	                                                 /*P-*/ "pending",
	                                                 /*PV*/ "inconsistent (pending and valid)"};
#else
# define MMU_MASK_VALID_FOR_32BITS 0
# define MMU_VALID_STR(entry,level) ("??")
#endif
	MMU_DEVICEATTRIBS *psDevAttrs = psMMUContext->psDevAttrs;
	MMU_LEVEL	eMMULevel = psDevAttrs->eTopLevel;
	const MMU_PxE_CONFIG *psConfig;
	const MMU_PxE_CONFIG *psMMUPDEConfig;
	const MMU_PxE_CONFIG *psMMUPTEConfig;
	const MMU_DEVVADDR_CONFIG *psMMUDevVAddrConfig;
	IMG_HANDLE hPriv;
	MMU_Levelx_INFO *psLevel = NULL;
	PVRSRV_ERROR eError;
	IMG_UINT64 uiIndex;
	IMG_UINT32 ui32PCIndex = 0xFFFFFFFF;
	IMG_UINT32 ui32PDIndex = 0xFFFFFFFF;
	IMG_UINT32 ui32PTIndex = 0xFFFFFFFF;
	IMG_UINT32 ui32Log2PageSize;
	MMU_FAULT_DATA sMMUFaultData = {0};
	MMU_LEVEL_DATA *psMMULevelData;

	OSLockAcquire(psMMUContext->hLock);

	/*
		At this point we don't know the page size so assume it's 4K.
		When we get the PD level (MMU_LEVEL_2) we can check to see
		if this assumption is correct.
	 */
	eError = psDevAttrs->pfnGetPageSizeConfiguration(12,
	                                                 &psMMUPDEConfig,
	                                                 &psMMUPTEConfig,
	                                                 &psMMUDevVAddrConfig,
	                                                 &hPriv);
	if (eError != PVRSRV_OK)
	{
		PVR_LOG(("Failed to get the page size info for log2 page sizeof 12"));
	}

	psLevel = &psMMUContext->sBaseLevelInfo;
	psConfig = psDevAttrs->psBaseConfig;

	sMMUFaultData.eTopLevel = psDevAttrs->eTopLevel;
	sMMUFaultData.eType = MMU_FAULT_TYPE_NON_PM;


	for (; eMMULevel > MMU_LEVEL_0; eMMULevel--)
	{
		if (eMMULevel == MMU_LEVEL_3)
		{
			/* Determine the PC index */
			uiIndex = psDevVAddr->uiAddr & psDevAttrs->psTopLevelDevVAddrConfig->uiPCIndexMask;
			uiIndex = uiIndex >> psDevAttrs->psTopLevelDevVAddrConfig->uiPCIndexShift;
			ui32PCIndex = (IMG_UINT32) uiIndex;
			PVR_ASSERT(uiIndex == ((IMG_UINT64) ui32PCIndex));

			psMMULevelData = &sMMUFaultData.sLevelData[MMU_LEVEL_3];
			psMMULevelData->uiBytesPerEntry = psConfig->uiBytesPerEntry;
			psMMULevelData->ui32Index = ui32PCIndex;

			if (ui32PCIndex >= psLevel->ui32NumOfEntries)
			{
				psMMULevelData->ui32NumOfEntries = psLevel->ui32NumOfEntries;
				break;
			}

			if (psConfig->uiBytesPerEntry == 4)
			{
				IMG_UINT32 *pui32Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui32Ptr[ui32PCIndex];
				if (MMU_MASK_VALID_FOR_32BITS(PC))
				{
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui32Ptr[ui32PCIndex] & psConfig->uiProtMask, PC);
				}
				else
				{
					psMMULevelData->psDebugStr = "";
					PVR_LOG(("Invalid RGX_MMUCTRL_PC_DATA_ENTRY mask for 32-bit entry"));
				}
			}
			else
			{
				IMG_UINT64 *pui64Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui64Ptr[ui32PCIndex];
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui64Ptr[ui32PCIndex] & psConfig->uiProtMask, PC);

			}

			psLevel = psLevel->apsNextLevel[ui32PCIndex];
			if (!psLevel)
			{
				break;
			}
			psConfig = psMMUPDEConfig;
			continue; /* continue to the next level */
		}


		if (eMMULevel == MMU_LEVEL_2)
		{
			/* Determine the PD index */
			uiIndex = psDevVAddr->uiAddr & psDevAttrs->psTopLevelDevVAddrConfig->uiPDIndexMask;
			uiIndex = uiIndex >> psDevAttrs->psTopLevelDevVAddrConfig->uiPDIndexShift;
			ui32PDIndex = (IMG_UINT32) uiIndex;
			PVR_ASSERT(uiIndex == ((IMG_UINT64) ui32PDIndex));

			psMMULevelData = &sMMUFaultData.sLevelData[MMU_LEVEL_2];
			psMMULevelData->uiBytesPerEntry = psConfig->uiBytesPerEntry;
			psMMULevelData->ui32Index = ui32PDIndex;

			if (ui32PDIndex >= psLevel->ui32NumOfEntries)
			{
				psMMULevelData->ui32NumOfEntries = psLevel->ui32NumOfEntries;
				break;
			}

			if (psConfig->uiBytesPerEntry == 4)
			{
				IMG_UINT32 *pui32Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui32Ptr[ui32PDIndex];
				if (MMU_MASK_VALID_FOR_32BITS(PD))
				{
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui32Ptr[ui32PDIndex] & psMMUPDEConfig->uiProtMask, PD);
				}
				else
				{
					psMMULevelData->psDebugStr = "";
					PVR_LOG(("Invalid RGX_MMUCTRL_PD_DATA_ENTRY mask for 32-bit entry"));
				}

				if (psDevAttrs->pfnGetPageSizeFromPDE4(pui32Ptr[ui32PDIndex], &ui32Log2PageSize) != PVRSRV_OK)
				{
					PVR_LOG(("Failed to get the page size from the PDE"));
				}
			}
			else
			{
				IMG_UINT64 *pui64Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui64Ptr[ui32PDIndex];
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui64Ptr[ui32PDIndex]  & psMMUPDEConfig->uiProtMask, PD);

				if (psDevAttrs->pfnGetPageSizeFromVirtAddr != NULL)
				{
					/* MMU_VERSION >= 4 */
					if (psDevAttrs->pfnGetPageSizeFromVirtAddr(psMMUContext->psPhysMemCtx->psDevNode, *psDevVAddr, &ui32Log2PageSize) != PVRSRV_OK)
					{
						PVR_LOG(("Failed to get the page size from the virtual address"));
					}
				}
				else if (psDevAttrs->pfnGetPageSizeFromPDE8(pui64Ptr[ui32PDIndex], &ui32Log2PageSize) != PVRSRV_OK)
				{
					PVR_LOG(("Failed to get the page size from the PDE"));
				}
			}

			/*
					We assumed the page size was 4K, now we have the actual size
					from the PDE we can confirm if our assumption was correct.
					Until now it hasn't mattered as the PC and PD are the same
					regardless of the page size
			 */
			if (ui32Log2PageSize != 12)
			{
				/* Put the 4K page size data */
				psDevAttrs->pfnPutPageSizeConfiguration(hPriv);

				/* Get the correct size data */
				eError = psDevAttrs->pfnGetPageSizeConfiguration(ui32Log2PageSize,
				                                                 &psMMUPDEConfig,
				                                                 &psMMUPTEConfig,
				                                                 &psMMUDevVAddrConfig,
				                                                 &hPriv);
				if (eError != PVRSRV_OK)
				{
					PVR_LOG(("Failed to get the page size info for log2 page sizeof %d", ui32Log2PageSize));
					break;
				}
			}
			psLevel = psLevel->apsNextLevel[ui32PDIndex];
			if (!psLevel)
			{
				break;
			}
			psConfig = psMMUPTEConfig;
			continue; /* continue to the next level */
		}


		if (eMMULevel == MMU_LEVEL_1)
		{
			/* Determine the PT index */
			uiIndex = psDevVAddr->uiAddr & psMMUDevVAddrConfig->uiPTIndexMask;
			uiIndex = uiIndex >> psMMUDevVAddrConfig->uiPTIndexShift;
			ui32PTIndex = (IMG_UINT32) uiIndex;
			PVR_ASSERT(uiIndex == ((IMG_UINT64) ui32PTIndex));

			psMMULevelData = &sMMUFaultData.sLevelData[MMU_LEVEL_1];
			psMMULevelData->uiBytesPerEntry = psConfig->uiBytesPerEntry;
			psMMULevelData->ui32Index = ui32PTIndex;

			if (ui32PTIndex >= psLevel->ui32NumOfEntries)
			{
				psMMULevelData->ui32NumOfEntries = psLevel->ui32NumOfEntries;
				break;
			}

			if (psConfig->uiBytesPerEntry == 4)
			{
				IMG_UINT32 *pui32Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui32Ptr[ui32PTIndex];
				if (MMU_MASK_VALID_FOR_32BITS(PT))
				{
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui32Ptr[ui32PTIndex] & psMMUPTEConfig->uiProtMask, PT);
				}
				else
				{
					psMMULevelData->psDebugStr = "";
					PVR_LOG(("Invalid RGX_MMUCTRL_PT_DATA_ENTRY mask for 32-bit entry"));
				}
			}
			else
			{
				IMG_UINT64 *pui64Ptr = psLevel->sMemDesc.pvCpuVAddr;

				psMMULevelData->ui64Address = pui64Ptr[ui32PTIndex];
				psMMULevelData->psDebugStr  = MMU_VALID_STR(pui64Ptr[ui32PTIndex] & psMMUPTEConfig->uiProtMask, PT);

			}
			goto e1;
		}

		PVR_LOG(("Unsupported MMU setup: %d", eMMULevel));
		break;
	}

e1:
	/* Put the page size data back */
	psDevAttrs->pfnPutPageSizeConfiguration(hPriv);
	OSLockRelease(psMMUContext->hLock);

	*psOutFaultData = sMMUFaultData;
}

static IMG_UINT64 MMU_GetVDevAddrPTE(MMU_CONTEXT *psMMUContext,
                                     const MMU_PxE_CONFIG *psConfig,
                                     const MMU_DEVVADDR_CONFIG *psDevVAddrConfig,
                                     IMG_UINT32 uiLog2PageSize,
                                     IMG_DEV_VIRTADDR sDevVAddr,
                                     IMG_BOOL *pbStatusOut)
{
	MMU_Levelx_INFO *psLevel = NULL;
	IMG_UINT32 uiIndex = 0;
	IMG_BOOL bStatus = IMG_FALSE;
	IMG_UINT64 ui64Entry = 0;

	OSLockAcquire(psMMUContext->hLock);

	switch (psMMUContext->psDevAttrs->eTopLevel)
	{
		case MMU_LEVEL_3:
			uiIndex = _CalcPCEIdx(sDevVAddr, psDevVAddrConfig, IMG_FALSE);
			psLevel = psMMUContext->sBaseLevelInfo.apsNextLevel[uiIndex];
			if (psLevel == NULL)
				break;

			__fallthrough;
		case MMU_LEVEL_2:
			uiIndex = _CalcPDEIdx(sDevVAddr, psDevVAddrConfig, IMG_FALSE);

			if (psLevel != NULL)
				psLevel = psLevel->apsNextLevel[uiIndex];
			else
				psLevel = psMMUContext->sBaseLevelInfo.apsNextLevel[uiIndex];

			if (psLevel == NULL)
				break;

			__fallthrough;
		case MMU_LEVEL_1:
			uiIndex = _CalcPTEIdx(sDevVAddr, psDevVAddrConfig, IMG_FALSE);

			if (psLevel == NULL)
				psLevel = &psMMUContext->sBaseLevelInfo;

			ui64Entry = ((IMG_UINT64 *)psLevel->sMemDesc.pvCpuVAddr)[uiIndex];
			bStatus = ui64Entry & psConfig->uiValidEnMask;

			break;
		default:
			PVR_LOG(("MMU_IsVDevAddrValid: Unsupported MMU setup"));
			break;
	}

	OSLockRelease(psMMUContext->hLock);

	*pbStatusOut = bStatus;

	return ui64Entry;
}

IMG_BOOL MMU_IsVDevAddrValid(MMU_CONTEXT *psMMUContext,
                             IMG_UINT32 uiLog2PageSize,
                             IMG_DEV_VIRTADDR sDevVAddr)
{
	IMG_BOOL bStatus;
	const MMU_PxE_CONFIG *psConfig;
	IMG_HANDLE hPriv;
	const MMU_DEVVADDR_CONFIG *psDevVAddrConfig;

	_MMU_GetPTConfig(psMMUContext, uiLog2PageSize, &psConfig, &hPriv, &psDevVAddrConfig);

	MMU_GetVDevAddrPTE(psMMUContext,
	                   psConfig,
	                   psDevVAddrConfig,
	                   uiLog2PageSize,
	                   sDevVAddr,
	                   &bStatus);

	_MMU_PutPTConfig(psMMUContext, hPriv);

	return bStatus;
}

#if defined(PDUMP)
/*
	MMU_ContextDerivePCPDumpSymAddr
 */
PVRSRV_ERROR MMU_ContextDerivePCPDumpSymAddr(MMU_CONTEXT *psMMUContext,
                                             IMG_CHAR *pszPDumpSymbolicNameBuffer,
                                             size_t uiPDumpSymbolicNameBufferSize)
{
	size_t uiCount;
	IMG_UINT64 ui64PhysAddr;
	PVRSRV_DEVICE_IDENTIFIER *psDevId = &psMMUContext->psPhysMemCtx->psDevNode->sDevId;

	if (!psMMUContext->sBaseLevelInfo.sMemDesc.bValid)
	{
		/* We don't have any allocations.  You're not allowed to ask
		 * for the page catalogue base address until you've made at
		 * least one allocation.
		 */
		return PVRSRV_ERROR_MMU_API_PROTOCOL_ERROR;
	}

	ui64PhysAddr = (IMG_UINT64)psMMUContext->sBaseLevelInfo.sMemDesc.sDevPAddr.uiAddr;

	PVR_ASSERT(uiPDumpSymbolicNameBufferSize >= (IMG_UINT32)(21 + OSStringLength(psDevId->pszPDumpDevName)));

	/* Page table Symbolic Name is formed from page table phys addr
	   prefixed with MMUPT_. */
	uiCount = OSSNPrintf(pszPDumpSymbolicNameBuffer,
	                     uiPDumpSymbolicNameBufferSize,
	                     ":%s:%s%016"IMG_UINT64_FMTSPECX,
	                     psDevId->pszPDumpDevName,
	                     psMMUContext->sBaseLevelInfo.sMemDesc.bValid?"MMUPC_":"XXX",
	                     ui64PhysAddr);

	if (uiCount + 1 > uiPDumpSymbolicNameBufferSize)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	return PVRSRV_OK;
}

/*
	MMU_PDumpWritePageCatBase
 */
PVRSRV_ERROR
MMU_PDumpWritePageCatBase(MMU_CONTEXT *psMMUContext,
                          const IMG_CHAR *pszSpaceName,
                          IMG_DEVMEM_OFFSET_T uiOffset,
                          IMG_UINT32 ui32WordSize,
                          IMG_UINT32 ui32AlignShift,
                          IMG_UINT32 ui32Shift,
                          PDUMP_FLAGS_T uiPdumpFlags)
{
	PVRSRV_ERROR eError;
	IMG_CHAR aszPageCatBaseSymbolicAddr[100];
	const IMG_CHAR *pszPDumpDevName = psMMUContext->psDevAttrs->pszMMUPxPDumpMemSpaceName;

	eError = MMU_ContextDerivePCPDumpSymAddr(psMMUContext,
	                                         &aszPageCatBaseSymbolicAddr[0],
	                                         sizeof(aszPageCatBaseSymbolicAddr));
	if (eError == PVRSRV_OK)
	{
		eError = PDumpWriteSymbAddress(pszSpaceName,
		                               uiOffset,
		                               aszPageCatBaseSymbolicAddr,
		                               0, /* offset -- Could be non-zero for var. pgsz */
		                               pszPDumpDevName,
		                               ui32WordSize,
		                               ui32AlignShift,
		                               ui32Shift,
		                               uiPdumpFlags | PDUMP_FLAGS_CONTINUOUS);
	}

	return eError;
}

/*
	MMU_AcquirePDumpMMUContext
 */
PVRSRV_ERROR MMU_AcquirePDumpMMUContext(MMU_CONTEXT *psMMUContext,
                                        IMG_UINT32 *pui32PDumpMMUContextID,
                                        IMG_UINT32 ui32PDumpFlags)
{
	PVRSRV_DEVICE_IDENTIFIER *psDevId = &psMMUContext->psPhysMemCtx->psDevNode->sDevId;

	if (!psMMUContext->ui32PDumpContextIDRefCount)
	{
		PDUMP_MMU_ALLOC_MMUCONTEXT(psDevId->pszPDumpDevName,
		                           psMMUContext->sBaseLevelInfo.sMemDesc.sDevPAddr,
		                           psMMUContext->psDevAttrs->eMMUType,
		                           &psMMUContext->uiPDumpContextID,
		                           ui32PDumpFlags);
	}

	psMMUContext->ui32PDumpContextIDRefCount++;
	*pui32PDumpMMUContextID = psMMUContext->uiPDumpContextID;

	return PVRSRV_OK;
}

/*
	MMU_ReleasePDumpMMUContext
 */
PVRSRV_ERROR MMU_ReleasePDumpMMUContext(MMU_CONTEXT *psMMUContext,
	                                        IMG_UINT32 ui32PDumpFlags)
{
	PVRSRV_DEVICE_IDENTIFIER *psDevId = &psMMUContext->psPhysMemCtx->psDevNode->sDevId;

	PVR_ASSERT(psMMUContext->ui32PDumpContextIDRefCount != 0);
	psMMUContext->ui32PDumpContextIDRefCount--;

	if (psMMUContext->ui32PDumpContextIDRefCount == 0)
	{
		PDUMP_MMU_FREE_MMUCONTEXT(psDevId->pszPDumpDevName,
		                          psMMUContext->uiPDumpContextID,
		                          ui32PDumpFlags);
	}

	return PVRSRV_OK;
}
#endif

#if defined(SUPPORT_PMR_DEFERRED_FREE)
PVRSRV_ERROR MMU_CacheInvalidateKick(PPVRSRV_DEVICE_NODE psDeviceNode,
                                     IMG_UINT32 *puiRequiredSyncValue)
{
	IMG_UINT32 uiRequiredSyncValue;
	PVRSRV_ERROR eError;

	eError = psDeviceNode->pfnMMUCacheInvalidateKick(psDeviceNode, &uiRequiredSyncValue, IMG_TRUE);

	if (puiRequiredSyncValue != NULL)
	{
		*puiRequiredSyncValue = uiRequiredSyncValue;
	}

	return eError;
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

/******************************************************************************
 End of file (mmu_common.c)
 ******************************************************************************/
