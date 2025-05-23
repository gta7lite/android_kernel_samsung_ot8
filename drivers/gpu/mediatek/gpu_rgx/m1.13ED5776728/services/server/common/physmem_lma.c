/*************************************************************************/ /*!
@File           physmem_lma.c
@Title          Local card memory allocator
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Part of the memory management. This module is responsible for
                implementing the function callbacks for local card memory.
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
#include "pvrsrv_memallocflags.h"
#include "rgx_pdump_panics.h"
#include "allocmem.h"
#include "osfunc.h"
#include "pvrsrv.h"
#include "devicemem_server_utils.h"
#include "physmem_lma.h"
#include "pdump_km.h"
#include "pmr.h"
#include "pmr_impl.h"
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#include "process_stats.h"
#endif

#if defined(SUPPORT_GPUVIRT_VALIDATION)
#include "rgxutils.h"
#endif

/* Since 0x0 is a valid DevPAddr, we rely on max 64-bit value to be an invalid
 * page address */
#define INVALID_PAGE_ADDR ~((IMG_UINT64)0x0)

typedef struct _PMR_LMALLOCARRAY_DATA_ {
	PVRSRV_DEVICE_NODE *psDevNode;
	IMG_PID uiPid;
	IMG_INT32 iNumPagesAllocated;
	/*
	 * uiTotalNumPages:
	 * Total number of pages supported by this PMR.
	 * (Fixed as of now due the fixed Page table array size)
	 */
	IMG_UINT32 uiTotalNumPages;
	IMG_UINT32 uiPagesToAlloc;

	IMG_UINT32 uiLog2AllocSize;
	IMG_UINT32 uiContigAllocSize;
	IMG_DEV_PHYADDR *pasDevPAddr;

	IMG_BOOL bZeroOnAlloc;
	IMG_BOOL bPoisonOnAlloc;
	IMG_BOOL bFwLocalAlloc;
	FIRMWARE_ALLOC_TYPE eFwAllocType;
	IMG_UINT32 ui32OSid;

	IMG_BOOL bOnDemand;

	/*
	  Record at alloc time whether poisoning will be required when the
	  PMR is freed.
	*/
	IMG_BOOL bPoisonOnFree;

	IMG_BOOL bIsZombie;
	IMG_BOOL bIsSparse;

	/* Physical heap and arena pointers for this allocation */
	PHYS_HEAP* psPhysHeap;
	RA_ARENA* psArena;
	PVRSRV_MEMALLOCFLAGS_T uiAllocFlags;

	/*
	   Connection data for this requests' originating process. NULL for
	   direct-bridge originating calls
	 */
	CONNECTION_DATA *psConnection;
} PMR_LMALLOCARRAY_DATA;

static PVRSRV_ERROR _MapAlloc(PVRSRV_DEVICE_NODE *psDevNode,
							  IMG_DEV_PHYADDR *psDevPAddr,
							  size_t uiSize,
							  IMG_BOOL bFwLocalAlloc,
							  PMR_FLAGS_T ulFlags,
							  void **pvPtr)
{
	IMG_UINT32 ui32CPUCacheFlags;
	IMG_CPU_PHYADDR sCpuPAddr;
	PHYS_HEAP *psPhysHeap;
	PVRSRV_ERROR eError;

	eError = DevmemCPUCacheMode(psDevNode, ulFlags, &ui32CPUCacheFlags);
	PVR_RETURN_IF_ERROR(eError);

	if (bFwLocalAlloc)
	{
		psPhysHeap = psDevNode->apsPhysHeap[PVRSRV_DEVICE_PHYS_HEAP_FW_LOCAL];
	}
	else
	{
		psPhysHeap = psDevNode->apsPhysHeap[PVRSRV_DEVICE_PHYS_HEAP_GPU_LOCAL];
	}

	PhysHeapDevPAddrToCpuPAddr(psPhysHeap, 1, &sCpuPAddr, psDevPAddr);

	*pvPtr = OSMapPhysToLin(sCpuPAddr, uiSize, ui32CPUCacheFlags);
	PVR_RETURN_IF_NOMEM(*pvPtr);

	return PVRSRV_OK;
}

static void _UnMapAlloc(PVRSRV_DEVICE_NODE *psDevNode,
						size_t uiSize,
						IMG_BOOL bFwLocalAlloc,
						PMR_FLAGS_T ulFlags,
						void *pvPtr)
{
	OSUnMapPhysToLin(pvPtr, uiSize, PVRSRV_CPU_CACHE_MODE(ulFlags));
}

static PVRSRV_ERROR
_PoisonAlloc(PVRSRV_DEVICE_NODE *psDevNode,
			 IMG_DEV_PHYADDR *psDevPAddr,
			 IMG_BOOL bFwLocalAlloc,
			 IMG_UINT32 uiContigAllocSize,
			 IMG_BYTE ui8PoisonValue)
{
	PVRSRV_ERROR eError;
	void *pvKernLin = NULL;

	eError = _MapAlloc(psDevNode,
					   psDevPAddr,
					   uiContigAllocSize,
					   bFwLocalAlloc,
					   PVRSRV_MEMALLOCFLAG_CPU_UNCACHED,
					   &pvKernLin);
	PVR_GOTO_IF_ERROR(eError, map_failed);

	OSDeviceMemSet(pvKernLin, ui8PoisonValue, uiContigAllocSize);

	_UnMapAlloc(psDevNode, uiContigAllocSize, bFwLocalAlloc, 0, pvKernLin);

	return PVRSRV_OK;

map_failed:
	PVR_DPF((PVR_DBG_ERROR, "Failed to poison allocation"));
	return eError;
}

static PVRSRV_ERROR
_ZeroAlloc(PVRSRV_DEVICE_NODE *psDevNode,
		   IMG_DEV_PHYADDR *psDevPAddr,
		   IMG_BOOL bFwLocalAlloc,
		   IMG_UINT32 uiContigAllocSize)
{
	void *pvKernLin = NULL;
	PVRSRV_ERROR eError;

	eError = _MapAlloc(psDevNode,
					   psDevPAddr,
					   uiContigAllocSize,
					   bFwLocalAlloc,
					   PVRSRV_MEMALLOCFLAG_CPU_UNCACHED,
					   &pvKernLin);
	PVR_GOTO_IF_ERROR(eError, map_failed);

	OSDeviceMemSet(pvKernLin, 0, uiContigAllocSize);

	_UnMapAlloc(psDevNode, uiContigAllocSize, bFwLocalAlloc, 0, pvKernLin);

	return PVRSRV_OK;

map_failed:
	PVR_DPF((PVR_DBG_ERROR, "Failed to zero allocation"));
	return eError;
}

static PVRSRV_ERROR
_AllocLMPageArray(PVRSRV_DEVICE_NODE *psDevNode,
                  PMR_SIZE_T uiSize,
                  PMR_SIZE_T uiChunkSize,
                  IMG_UINT32 ui32NumPhysChunks,
                  IMG_UINT32 ui32NumVirtChunks,
                  IMG_UINT32 *pabMappingTable,
                  IMG_UINT32 uiLog2AllocPageSize,
                  IMG_BOOL bZero,
                  IMG_BOOL bPoisonOnAlloc,
                  IMG_BOOL bPoisonOnFree,
                  IMG_BOOL bContig,
                  IMG_BOOL bOnDemand,
                  IMG_BOOL bFwLocalAlloc,
                  FIRMWARE_ALLOC_TYPE eFwAllocType,
                  IMG_UINT32 ui32OSid,
                  PHYS_HEAP* psPhysHeap,
                  PVRSRV_MEMALLOCFLAGS_T uiAllocFlags,
                  IMG_PID uiPid,
                  PMR_LMALLOCARRAY_DATA **ppsPageArrayDataPtr,
                  CONNECTION_DATA *psConnection
                  )
{
	PMR_LMALLOCARRAY_DATA *psPageArrayData = NULL;
	IMG_UINT32 ui32Index;
	PVRSRV_ERROR eError;

	PVR_ASSERT(!bZero || !bPoisonOnAlloc);
	PVR_ASSERT(OSGetPageShift() <= uiLog2AllocPageSize);

#if defined(SUPPORT_SECURITY_VALIDATION)
	/* The following check is done before any attempt to use either security flag */
	if ((PVRSRV_CHECK_SECURE_FW_CODE(uiAllocFlags) && PVRSRV_CHECK_SECURE_BUFFER(uiAllocFlags)) ||
	    (PVRSRV_CHECK_SECURE_FW_DATA(uiAllocFlags) && PVRSRV_CHECK_SECURE_BUFFER(uiAllocFlags)) ||
	    (PVRSRV_CHECK_SECURE_FW_CODE(uiAllocFlags) && PVRSRV_CHECK_SECURE_FW_DATA(uiAllocFlags)))
	{
		PVR_DPF((PVR_DBG_ERROR, "Multiple secure allocation flags are set!"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
#endif

	psPageArrayData = OSAllocZMem(sizeof(PMR_LMALLOCARRAY_DATA));
	PVR_GOTO_IF_NOMEM(psPageArrayData, eError, errorOnAllocArray);

	if (bContig)
	{
		/*
			Some allocations require kernel mappings in which case in order
			to be virtually contiguous we also have to be physically contiguous.
		*/
		psPageArrayData->uiTotalNumPages = 1;
		psPageArrayData->uiPagesToAlloc = psPageArrayData->uiTotalNumPages;
		psPageArrayData->uiContigAllocSize = TRUNCATE_64BITS_TO_32BITS(uiSize);
		psPageArrayData->uiLog2AllocSize = uiLog2AllocPageSize;
		psPageArrayData->bIsSparse = IMG_FALSE;
	}
	else
	{
		IMG_UINT32 uiNumPages;

		/* Use of cast below is justified by the assertion that follows to
		prove that no significant bits have been truncated */
		uiNumPages = (IMG_UINT32)(((uiSize - 1) >> uiLog2AllocPageSize) + 1);
		PVR_ASSERT(((PMR_SIZE_T)uiNumPages << uiLog2AllocPageSize) == uiSize);

		psPageArrayData->uiTotalNumPages = uiNumPages;

		if ((ui32NumVirtChunks != ui32NumPhysChunks) || (1 < ui32NumVirtChunks))
		{
			psPageArrayData->uiPagesToAlloc = ui32NumPhysChunks;
		}
		else
		{
			psPageArrayData->uiPagesToAlloc = uiNumPages;
		}
		psPageArrayData->uiContigAllocSize = 1 << uiLog2AllocPageSize;
		psPageArrayData->uiLog2AllocSize = uiLog2AllocPageSize;
		psPageArrayData->bIsSparse = IMG_TRUE;
	}
	psPageArrayData->psDevNode = psDevNode;
	psPageArrayData->psConnection = psConnection;
	psPageArrayData->uiPid = uiPid;
	psPageArrayData->pasDevPAddr = OSAllocMem(sizeof(IMG_DEV_PHYADDR) *
												psPageArrayData->uiTotalNumPages);
	PVR_GOTO_IF_NOMEM(psPageArrayData->pasDevPAddr, eError, errorOnAllocAddr);

	/* Since no pages are allocated yet, initialise page addresses to INVALID_PAGE_ADDR */
	for (ui32Index = 0; ui32Index < psPageArrayData->uiTotalNumPages; ui32Index++)
	{
		psPageArrayData->pasDevPAddr[ui32Index].uiAddr = INVALID_PAGE_ADDR;
	}

	psPageArrayData->iNumPagesAllocated = 0;
	psPageArrayData->bZeroOnAlloc = bZero;
	psPageArrayData->bPoisonOnAlloc = bPoisonOnAlloc;
	psPageArrayData->bPoisonOnFree = bPoisonOnFree;
	psPageArrayData->bOnDemand = bOnDemand;
	psPageArrayData->bFwLocalAlloc = bFwLocalAlloc;
	psPageArrayData->psPhysHeap = psPhysHeap;
	psPageArrayData->uiAllocFlags = uiAllocFlags;
	psPageArrayData->eFwAllocType = eFwAllocType;
	psPageArrayData->ui32OSid = ui32OSid;

	*ppsPageArrayDataPtr = psPageArrayData;

	return PVRSRV_OK;

	/*
	  error exit paths follow:
	*/
errorOnAllocAddr:
	OSFreeMem(psPageArrayData);

errorOnAllocArray:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}


static PVRSRV_ERROR
_AllocLMPages(PMR_LMALLOCARRAY_DATA *psPageArrayData, IMG_UINT32 *pui32MapTable)
{
	PVRSRV_ERROR eError;
	RA_BASE_T uiCardAddr;
	RA_LENGTH_T uiActualSize;
	IMG_UINT32 i, ui32Index = 0;
	IMG_UINT32 uiContigAllocSize;
	IMG_UINT32 uiLog2AllocSize;
	PVRSRV_DEVICE_NODE *psDevNode;
	IMG_BOOL bPoisonOnAlloc;
	IMG_BOOL bZeroOnAlloc;
	RA_ARENA *pArena;

	PVR_ASSERT(NULL != psPageArrayData);
	PVR_ASSERT(0 <= psPageArrayData->iNumPagesAllocated);

	uiContigAllocSize = psPageArrayData->uiContigAllocSize;
	uiLog2AllocSize = psPageArrayData->uiLog2AllocSize;
	psDevNode = psPageArrayData->psDevNode;
	bPoisonOnAlloc = psPageArrayData->bPoisonOnAlloc;
	bZeroOnAlloc = psPageArrayData->bZeroOnAlloc;

	if (!PVRSRV_VZ_MODE_IS(NATIVE) && psPageArrayData->bFwLocalAlloc)
	{
		/* Allocate from FW_LOCAL or FW_GUEST physheap via the RAs */
		switch (psPageArrayData->eFwAllocType)
		{
			case FW_ALLOC_CONFIG:
			{
				pArena = psDevNode->psKernelFwConfigMemArena;
				break;
			}
			case FW_ALLOC_RAW:
			{
				pArena = psDevNode->psKernelFwRawMemArena[psPageArrayData->ui32OSid];
				break;
			}
			default:
			{
				PVR_ASSERT(psPageArrayData->eFwAllocType != FW_ALLOC_NO_FW_ACCESS);
				pArena = psDevNode->psKernelFwMainMemArena;
				break;
			}
		}
	}
	else
	{
		IMG_UINT32 uiRegionId;

		/* Get suitable local memory region for this GPU physheap allocation */
		uiRegionId = PhysHeapGetRegionId(psPageArrayData->psPhysHeap,
		                                 psPageArrayData->uiAllocFlags);
		PVR_ASSERT(uiRegionId < psDevNode->ui32NumOfLocalMemArenas);
		pArena = psDevNode->apsLocalDevMemArenas[uiRegionId];
	}

	if (psPageArrayData->uiTotalNumPages <
			(psPageArrayData->iNumPagesAllocated + psPageArrayData->uiPagesToAlloc))
	{
		PVR_DPF((PVR_DBG_ERROR, "Pages requested to allocate don't fit PMR alloc Size. "
				"Allocated: %u + Requested: %u > Total Allowed: %u",
				psPageArrayData->iNumPagesAllocated,
				psPageArrayData->uiPagesToAlloc,
				psPageArrayData->uiTotalNumPages));
		return PVRSRV_ERROR_PMR_BAD_MAPPINGTABLE_SIZE;
	}


#if defined(SUPPORT_GPUVIRT_VALIDATION)
	{
		IMG_UINT32 ui32OSid=0;

		/* Obtain the OSid specific data from our connection handle */
		if (psPageArrayData->psConnection != NULL)
		{
			ui32OSid = psPageArrayData->psConnection->ui32OSid;
		}

		pArena=psDevNode->psOSidSubArena[ui32OSid];
		PVR_DPF((PVR_DBG_MESSAGE,
		         "(GPU Virtualization Validation): Giving from OS slot %d",
		         ui32OSid));
	}
#endif

	psPageArrayData->psArena = pArena;

	for (i = 0; i < psPageArrayData->uiPagesToAlloc; i++)
	{
		/* This part of index finding should happen before allocating the page.
		 * Just avoiding intricate paths */
		if (psPageArrayData->uiTotalNumPages == psPageArrayData->uiPagesToAlloc)
		{
			ui32Index = i;
		}
		else
		{
			if (NULL == pui32MapTable)
			{
				PVR_LOG_GOTO_WITH_ERROR("pui32MapTable", eError, PVRSRV_ERROR_PMR_INVALID_MAP_INDEX_ARRAY, errorOnRAAlloc);
			}

			ui32Index = pui32MapTable[i];
			if (ui32Index >= psPageArrayData->uiTotalNumPages)
			{
				PVR_DPF((PVR_DBG_ERROR,
						"%s: Page alloc request Index out of bounds for PMR @0x%p",
						__func__,
						psPageArrayData));
				PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_DEVICEMEM_OUT_OF_RANGE, errorOnRAAlloc);
			}

			if (INVALID_PAGE_ADDR != psPageArrayData->pasDevPAddr[ui32Index].uiAddr)
			{
				PVR_LOG_GOTO_WITH_ERROR("Mapping already exists", eError, PVRSRV_ERROR_PMR_MAPPING_ALREADY_EXISTS, errorOnRAAlloc);
			}
		}

		eError = RA_Alloc(pArena,
		                  uiContigAllocSize,
		                  RA_NO_IMPORT_MULTIPLIER,
		                  0,                       /* No flags */
		                  1ULL << uiLog2AllocSize,
		                  "LMA_Page_Alloc",
		                  &uiCardAddr,
		                  &uiActualSize,
		                  NULL);                   /* No private handle */
		if (PVRSRV_OK != eError)
		{
			PVR_DPF((PVR_DBG_ERROR,
					"Failed to Allocate the page @index:%d, size = 0x%llx",
					ui32Index, 1ULL << uiLog2AllocSize));
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_FAILED_TO_ALLOC_PAGES, errorOnRAAlloc);
		}

#if defined(SUPPORT_GPUVIRT_VALIDATION)
{
		PVR_DPF((PVR_DBG_MESSAGE,
				"(GPU Virtualization Validation): Address: 0x%"IMG_UINT64_FMTSPECX,
				uiCardAddr));
}
#endif

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#if !defined(PVRSRV_ENABLE_MEMORY_STATS)
		/* Allocation is done a page at a time */
		PVRSRVStatsIncrMemAllocStat(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES, uiActualSize, psPageArrayData->uiPid);
#else
		{
			IMG_CPU_PHYADDR sLocalCpuPAddr;

			sLocalCpuPAddr.uiAddr = (IMG_UINT64)uiCardAddr;
			PVRSRVStatsAddMemAllocRecord(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES,
									 NULL,
									 sLocalCpuPAddr,
									 uiActualSize,
									 NULL,
									 psPageArrayData->uiPid
									 DEBUG_MEMSTATS_VALUES);
		}
#endif
#endif

		psPageArrayData->pasDevPAddr[ui32Index].uiAddr = uiCardAddr;
		if (bPoisonOnAlloc)
		{
			eError = _PoisonAlloc(psDevNode,
								  &psPageArrayData->pasDevPAddr[ui32Index],
								  psPageArrayData->bFwLocalAlloc,
								  uiContigAllocSize,
								  PVRSRV_POISON_ON_ALLOC_VALUE);
			PVR_LOG_GOTO_IF_ERROR(eError, "_PoisonAlloc", errorOnPoison);
		}

		if (bZeroOnAlloc)
		{
			eError = _ZeroAlloc(psDevNode,
								&psPageArrayData->pasDevPAddr[ui32Index],
								psPageArrayData->bFwLocalAlloc,
								uiContigAllocSize);
			PVR_LOG_GOTO_IF_ERROR(eError, "_ZeroAlloc", errorOnZero);
		}
	}
	psPageArrayData->iNumPagesAllocated += psPageArrayData->uiPagesToAlloc;

	return PVRSRV_OK;

	/*
	  error exit paths follow:
	*/
errorOnZero:
errorOnPoison:
	eError = PVRSRV_ERROR_PMR_FAILED_TO_ALLOC_PAGES;
errorOnRAAlloc:
	PVR_DPF((PVR_DBG_ERROR,
			"%s: alloc_pages failed to honour request %d @index: %d of %d pages: (%s)",
			__func__,
			ui32Index,
			i,
			psPageArrayData->uiPagesToAlloc,
			PVRSRVGetErrorString(eError)));
	while (--i < psPageArrayData->uiPagesToAlloc)
	{
		if (psPageArrayData->uiTotalNumPages == psPageArrayData->uiPagesToAlloc)
		{
			ui32Index = i;
		}
		else
		{
			if (NULL == pui32MapTable)
			{
				break;
			}

			ui32Index = pui32MapTable[i];
		}

		if (ui32Index < psPageArrayData->uiTotalNumPages)
		{
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#if !defined(PVRSRV_ENABLE_MEMORY_STATS)
			/* Allocation is done a page at a time */
			PVRSRVStatsDecrMemAllocStat(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES,
			                            uiContigAllocSize,
			                            psPageArrayData->uiPid);
#else
			{
				PVRSRVStatsRemoveMemAllocRecord(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES,
				                                psPageArrayData->pasDevPAddr[ui32Index].uiAddr,
				                                psPageArrayData->uiPid);
			}
#endif
#endif
			RA_Free(pArena, psPageArrayData->pasDevPAddr[ui32Index].uiAddr);
			psPageArrayData->pasDevPAddr[ui32Index].uiAddr = INVALID_PAGE_ADDR;
		}
	}
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

static PVRSRV_ERROR
_FreeLMPageArray(PMR_LMALLOCARRAY_DATA *psPageArrayData)
{
	OSFreeMem(psPageArrayData->pasDevPAddr);

	PVR_DPF((PVR_DBG_MESSAGE,
			"physmem_lma.c: freed local memory array structure for PMR @0x%p",
			psPageArrayData));

	OSFreeMem(psPageArrayData);

	return PVRSRV_OK;
}

static PVRSRV_ERROR
_FreeLMPages(PMR_LMALLOCARRAY_DATA *psPageArrayData,
             IMG_UINT32 *pui32FreeIndices,
             IMG_UINT32 ui32FreePageCount)
{
	IMG_UINT32 uiContigAllocSize;
	IMG_UINT32 i, ui32PagesToFree=0, ui32PagesFreed=0, ui32Index=0;
	RA_ARENA *pArena = psPageArrayData->psArena;
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
	IMG_UINT32 uiStat = PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES;
#if defined(SUPPORT_PMR_DEFERRED_FREE)
	if (psPageArrayData->bIsZombie)
	{
		uiStat = PVRSRV_MEM_ALLOC_TYPE_ZOMBIE_LMA_PAGES;
	}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */
#endif /* defined(PVRSRV_ENABLE_PROCESS_STATS) */

	PVR_ASSERT(psPageArrayData->iNumPagesAllocated != 0);

	uiContigAllocSize = psPageArrayData->uiContigAllocSize;

	ui32PagesToFree = (NULL == pui32FreeIndices) ?
			psPageArrayData->uiTotalNumPages : ui32FreePageCount;

	for (i = 0; i < ui32PagesToFree; i++)
	{
		if (NULL == pui32FreeIndices)
		{
			ui32Index = i;
		}
		else
		{
			ui32Index = pui32FreeIndices[i];
		}

		if (INVALID_PAGE_ADDR != psPageArrayData->pasDevPAddr[ui32Index].uiAddr)
		{
			ui32PagesFreed++;
			if (psPageArrayData->bPoisonOnFree)
			{
				_PoisonAlloc(psPageArrayData->psDevNode,
							 &psPageArrayData->pasDevPAddr[ui32Index],
							 psPageArrayData->bFwLocalAlloc,
							 uiContigAllocSize,
							 PVRSRV_POISON_ON_FREE_VALUE);
			}

			RA_Free(pArena,	psPageArrayData->pasDevPAddr[ui32Index].uiAddr);

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#if !defined(PVRSRV_ENABLE_MEMORY_STATS)
			/* Allocation is done a page at a time */
			PVRSRVStatsDecrMemAllocStat(uiStat,
			                          uiContigAllocSize,
			                          psPageArrayData->uiPid);
#else
			{
				PVRSRVStatsRemoveMemAllocRecord(uiStat,
				                                psPageArrayData->pasDevPAddr[ui32Index].uiAddr,
				                                psPageArrayData->uiPid);
			}
#endif
#endif
			psPageArrayData->pasDevPAddr[ui32Index].uiAddr = INVALID_PAGE_ADDR;
		}
	}
	psPageArrayData->iNumPagesAllocated -= ui32PagesFreed;

	PVR_ASSERT(0 <= psPageArrayData->iNumPagesAllocated);

	PVR_DPF((PVR_DBG_MESSAGE,
			"%s: freed %d local memory for PMR @0x%p",
			__func__,
			(ui32PagesFreed * uiContigAllocSize),
			psPageArrayData));

	return PVRSRV_OK;
}

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
static PVRSRV_ERROR PMRFreeZombiePagesLocalMem(PMR_IMPL_ZOMBIEPAGES pvPriv)
{
	PVRSRV_ERROR eError;
	PMR_LMALLOCARRAY_DATA *psZombiePageArray = pvPriv;

	eError = _FreeLMPages(psZombiePageArray, NULL, 0);
	PVR_GOTO_IF_ERROR(eError, e0);

	_FreeLMPageArray(psZombiePageArray);
	return PVRSRV_OK;
e0:
	return eError;
}

/* Allocates a new PMR_LMALLOCARRAY_DATA object and fills it with
 * pages to be extracted from psSrcPageArrayData.
 */
static PVRSRV_ERROR
_ExtractPages(PMR_LMALLOCARRAY_DATA *psSrcPageArrayData,
			  IMG_UINT32 *pai32ExtractIndices,
			  IMG_UINT32 ui32ExtractPageCount,
			  PMR_LMALLOCARRAY_DATA **psOutPageArrayData)
{
	PVRSRV_ERROR eError;
	PMR_LMALLOCARRAY_DATA* psDstPageArrayData;
	IMG_UINT32 i;

	/* Alloc PMR_LMALLOCARRAY_DATA for the extracted pages */
	eError = _AllocLMPageArray(psSrcPageArrayData->psDevNode,
	                           (IMG_UINT64)ui32ExtractPageCount << psSrcPageArrayData->uiLog2AllocSize,
	                           1ULL << psSrcPageArrayData->uiLog2AllocSize,
	                           ui32ExtractPageCount,
	                           ui32ExtractPageCount,
	                           NULL,
	                           psSrcPageArrayData->uiLog2AllocSize,
	                           psSrcPageArrayData->bZeroOnAlloc,
	                           psSrcPageArrayData->bPoisonOnAlloc,
	                           psSrcPageArrayData->bPoisonOnFree,
	                           IMG_FALSE,
	                           psSrcPageArrayData->bOnDemand,
	                           psSrcPageArrayData->bFwLocalAlloc,
	                           psSrcPageArrayData->eFwAllocType,
	                           psSrcPageArrayData->ui32OSid,
	                           psSrcPageArrayData->psPhysHeap,
	                           psSrcPageArrayData->uiAllocFlags,
	                           psSrcPageArrayData->uiPid,
	                           &psDstPageArrayData,
	                           psSrcPageArrayData->psConnection);
	PVR_LOG_GOTO_IF_ERROR(eError, "_AllocLMPageArray", alloc_error);

	psDstPageArrayData->psArena = psSrcPageArrayData->psArena;

	for (i=0; i<ui32ExtractPageCount; i++)
	{
		IMG_UINT32 ui32SrcIdx = pai32ExtractIndices[i];
		if (psSrcPageArrayData->pasDevPAddr[ui32SrcIdx].uiAddr != INVALID_PAGE_ADDR)
		{
			psDstPageArrayData->pasDevPAddr[i].uiAddr = psSrcPageArrayData->pasDevPAddr[ui32SrcIdx].uiAddr;
			psDstPageArrayData->iNumPagesAllocated++;

			psSrcPageArrayData->pasDevPAddr[ui32SrcIdx].uiAddr = INVALID_PAGE_ADDR;
			psSrcPageArrayData->iNumPagesAllocated--;
		}
	}

	*psOutPageArrayData = psDstPageArrayData;

	return PVRSRV_OK;
alloc_error:
	return eError;
}
#endif /* defined(SUPPORT_PMR_PAGES_DEFERRED_FREE) */

/*
 *
 * Implementation of callback functions
 *
 */

/* destructor func is called after last reference disappears, but
   before PMR itself is freed. */
static PVRSRV_ERROR
PMRFinalizeLocalMem(PMR_IMPL_PRIVDATA pvPriv)
{
	PVRSRV_ERROR eError;
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = pvPriv;

	/* We can't free pages until now. */
	if (psLMAllocArrayData->iNumPagesAllocated != 0)
	{
		eError = _FreeLMPages(psLMAllocArrayData, NULL, 0);
		PVR_ASSERT (eError == PVRSRV_OK); /* can we do better? */
	}

	eError = _FreeLMPageArray(psLMAllocArrayData);
	PVR_ASSERT (eError == PVRSRV_OK); /* can we do better? */

	return PVRSRV_OK;
}

#if defined(SUPPORT_PMR_DEFERRED_FREE)
static PVRSRV_ERROR PMRZombifyLocalMem(PMR_IMPL_PRIVDATA pvPriv, PMR *psPMR)
{
	PMR_LMALLOCARRAY_DATA *psPageArrayData = pvPriv;
#if defined(PVRSRV_ENABLE_PROCESS_STATS)
	IMG_PID uiPid = psPageArrayData->uiPid;
#endif

	psPageArrayData->bIsZombie = IMG_TRUE;

#if defined(PVRSRV_ENABLE_PROCESS_STATS)
#if !defined(PVRSRV_ENABLE_MEMORY_STATS)
	{
		IMG_UINT32 uiLog2ChunkSize = psPageArrayData->uiLog2AllocSize;
		IMG_UINT64 uiSize = 0;

		if (psPageArrayData->bIsSparse)
		{
			uiSize = psPageArrayData->iNumPagesAllocated << uiLog2ChunkSize;
		}
		else
		{
			uiSize = psPageArrayData->uiContigAllocSize;
		}

		PVRSRVStatsDecrMemAllocStat(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES, uiSize, uiPid);
		PVRSRVStatsIncrMemAllocStat(PVRSRV_MEM_ALLOC_TYPE_ZOMBIE_LMA_PAGES, uiSize, uiPid);
	}
#else /* !defined(PVRSRV_ENABLE_MEMORY_STATS) */
	{
		IMG_UINT32 i;

		for (i = 0; i < psPageArrayData->uiTotalNumPages; i++)
		{
			if (psPageArrayData->pasDevPAddr[i].uiAddr != INVALID_PAGE_ADDR)
			{
				IMG_CPU_PHYADDR sCpuPAddr = {
					.uiAddr = psPageArrayData->pasDevPAddr[i].uiAddr,
				};

				PVRSRVStatsRemoveMemAllocRecord(PVRSRV_MEM_ALLOC_TYPE_ALLOC_LMA_PAGES,
				                                psPageArrayData->pasDevPAddr[i].uiAddr,
				                                uiPid);
				PVRSRVStatsAddMemAllocRecord(PVRSRV_MEM_ALLOC_TYPE_ZOMBIE_LMA_PAGES,
				                             NULL,
				                             sCpuPAddr,
				                             psPageArrayData->uiContigAllocSize,
				                             NULL,
				                             uiPid
				                             DEBUG_MEMSTATS_VALUES);
			}
		}
	}
#endif /* !defined(PVRSRV_ENABLE_MEMORY_STATS) */
#endif /* defined(PVRSRV_ENABLE_PROCESS_STATS) */

	PVR_UNREFERENCED_PARAMETER(psPMR);

	return PVRSRV_OK;
}
#endif /* defined(SUPPORT_PMR_DEFERRED_FREE) */

/* callback function for locking the system physical page addresses.
   As we are LMA there is nothing to do as we control physical memory. */
static PVRSRV_ERROR
PMRLockSysPhysAddressesLocalMem(PMR_IMPL_PRIVDATA pvPriv)
{

	PVRSRV_ERROR eError;
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData;

	psLMAllocArrayData = pvPriv;

	if (psLMAllocArrayData->bOnDemand)
	{
		/* Allocate Memory for deferred allocation */
		eError = _AllocLMPages(psLMAllocArrayData, NULL);
		PVR_RETURN_IF_ERROR(eError);
	}

	return PVRSRV_OK;
}

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
static PVRSRV_ERROR
PMRUnlockSysPhysAddressesLocalMem(PMR_IMPL_PRIVDATA pvPriv,
                                  PMR_IMPL_ZOMBIEPAGES *ppvZombiePages)
#else
static PVRSRV_ERROR
PMRUnlockSysPhysAddressesLocalMem(PMR_IMPL_PRIVDATA pvPriv)
#endif
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = pvPriv;
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)

	*ppvZombiePages = NULL;
#endif

	if (psLMAllocArrayData->bOnDemand)
	{
		/* Free Memory for deferred allocation */
		eError = _FreeLMPages(psLMAllocArrayData, NULL, 0);
		PVR_RETURN_IF_ERROR(eError);
	}

	PVR_ASSERT(eError == PVRSRV_OK);
	return eError;
}

/* N.B. It is assumed that PMRLockSysPhysAddressesLocalMem() is called _before_ this function! */
static PVRSRV_ERROR
PMRSysPhysAddrLocalMem(PMR_IMPL_PRIVDATA pvPriv,
					   IMG_UINT32 ui32Log2PageSize,
					   IMG_UINT32 ui32NumOfPages,
					   IMG_DEVMEM_OFFSET_T *puiOffset,
					   IMG_BOOL *pbValid,
					   IMG_DEV_PHYADDR *psDevPAddr)
{
	IMG_UINT32 idx;
	IMG_UINT32 uiLog2AllocSize;
	IMG_UINT32 uiNumAllocs;
	IMG_UINT64 uiAllocIndex;
	IMG_DEVMEM_OFFSET_T uiInAllocOffset;
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = pvPriv;

	if (psLMAllocArrayData->uiLog2AllocSize < ui32Log2PageSize)
	{
		PVR_DPF((PVR_DBG_ERROR,
		         "%s: Requested physical addresses from PMR "
		         "for incompatible contiguity %u!",
		         __func__,
		         ui32Log2PageSize));
		return PVRSRV_ERROR_PMR_INCOMPATIBLE_CONTIGUITY;
	}

	uiNumAllocs = psLMAllocArrayData->uiTotalNumPages;
	if (uiNumAllocs > 1)
	{
		PVR_ASSERT(psLMAllocArrayData->uiLog2AllocSize != 0);
		uiLog2AllocSize = psLMAllocArrayData->uiLog2AllocSize;

		for (idx=0; idx < ui32NumOfPages; idx++)
		{
			if (pbValid[idx])
			{
				uiAllocIndex = puiOffset[idx] >> uiLog2AllocSize;
				uiInAllocOffset = puiOffset[idx] - (uiAllocIndex << uiLog2AllocSize);

				PVR_LOG_RETURN_IF_FALSE(uiAllocIndex < uiNumAllocs,
				                        "puiOffset out of range", PVRSRV_ERROR_OUT_OF_RANGE);

				PVR_ASSERT(uiInAllocOffset < (1ULL << uiLog2AllocSize));

				psDevPAddr[idx].uiAddr = psLMAllocArrayData->pasDevPAddr[uiAllocIndex].uiAddr + uiInAllocOffset;
			}
		}
	}
	else
	{
		for (idx=0; idx < ui32NumOfPages; idx++)
		{
			if (pbValid[idx])
			{
				psDevPAddr[idx].uiAddr = psLMAllocArrayData->pasDevPAddr[0].uiAddr + puiOffset[idx];
			}
		}
	}

	return PVRSRV_OK;
}

static PVRSRV_ERROR
PMRAcquireKernelMappingDataLocalMem(PMR_IMPL_PRIVDATA pvPriv,
								 size_t uiOffset,
								 size_t uiSize,
								 void **ppvKernelAddressOut,
								 IMG_HANDLE *phHandleOut,
								 PMR_FLAGS_T ulFlags)
{
	PVRSRV_ERROR eError;
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = NULL;
	void *pvKernLinAddr = NULL;
	IMG_UINT32 ui32PageIndex = 0;
	size_t uiOffsetMask = uiOffset;

	psLMAllocArrayData = pvPriv;

	/* Check that we can map this in contiguously */
	if (psLMAllocArrayData->uiTotalNumPages != 1)
	{
		size_t uiStart = uiOffset;
		size_t uiEnd = uiOffset + uiSize - 1;
		size_t uiPageMask = ~((1 << psLMAllocArrayData->uiLog2AllocSize) - 1);

		/* We can still map if only one page is required */
		if ((uiStart & uiPageMask) != (uiEnd & uiPageMask))
		{
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_PMR_INCOMPATIBLE_CONTIGUITY, e0);
		}

		/* Locate the desired physical page to map in */
		ui32PageIndex = uiOffset >> psLMAllocArrayData->uiLog2AllocSize;
		uiOffsetMask = (1U << psLMAllocArrayData->uiLog2AllocSize) - 1;
	}

	PVR_ASSERT(ui32PageIndex < psLMAllocArrayData->uiTotalNumPages);

	eError = _MapAlloc(psLMAllocArrayData->psDevNode,
						&psLMAllocArrayData->pasDevPAddr[ui32PageIndex],
						psLMAllocArrayData->uiContigAllocSize,
						psLMAllocArrayData->bFwLocalAlloc,
						ulFlags,
						&pvKernLinAddr);

	*ppvKernelAddressOut = ((IMG_CHAR *) pvKernLinAddr) + (uiOffset & uiOffsetMask);
	*phHandleOut = pvKernLinAddr;

	return eError;

	/*
	  error exit paths follow:
	*/
e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

static void PMRReleaseKernelMappingDataLocalMem(PMR_IMPL_PRIVDATA pvPriv,
												 IMG_HANDLE hHandle)
{
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = NULL;
	void *pvKernLinAddr = NULL;

	psLMAllocArrayData = (PMR_LMALLOCARRAY_DATA *) pvPriv;
	pvKernLinAddr = (void *) hHandle;

	_UnMapAlloc(psLMAllocArrayData->psDevNode,
				psLMAllocArrayData->uiContigAllocSize,
				psLMAllocArrayData->bFwLocalAlloc,
				0,
				pvKernLinAddr);
}


static PVRSRV_ERROR
CopyBytesLocalMem(PMR_IMPL_PRIVDATA pvPriv,
				  IMG_DEVMEM_OFFSET_T uiOffset,
				  IMG_UINT8 *pcBuffer,
				  size_t uiBufSz,
				  size_t *puiNumBytes,
				  void (*pfnCopyBytes)(IMG_UINT8 *pcBuffer,
									   IMG_UINT8 *pcPMR,
									   size_t uiSize))
{
	PMR_LMALLOCARRAY_DATA *psLMAllocArrayData = NULL;
	size_t uiBytesCopied;
	size_t uiBytesToCopy;
	size_t uiBytesCopyableFromAlloc;
	void *pvMapping = NULL;
	IMG_UINT8 *pcKernelPointer = NULL;
	size_t uiBufferOffset;
	IMG_UINT64 uiAllocIndex;
	IMG_DEVMEM_OFFSET_T uiInAllocOffset;
	PVRSRV_ERROR eError;

	psLMAllocArrayData = pvPriv;

	uiBytesCopied = 0;
	uiBytesToCopy = uiBufSz;
	uiBufferOffset = 0;

	if (psLMAllocArrayData->uiTotalNumPages > 1)
	{
		while (uiBytesToCopy > 0)
		{
			/* we have to map one alloc in at a time */
			PVR_ASSERT(psLMAllocArrayData->uiLog2AllocSize != 0);
			uiAllocIndex = uiOffset >> psLMAllocArrayData->uiLog2AllocSize;
			uiInAllocOffset = uiOffset - (uiAllocIndex << psLMAllocArrayData->uiLog2AllocSize);
			uiBytesCopyableFromAlloc = uiBytesToCopy;
			if (uiBytesCopyableFromAlloc + uiInAllocOffset > (1ULL << psLMAllocArrayData->uiLog2AllocSize))
			{
				uiBytesCopyableFromAlloc = TRUNCATE_64BITS_TO_SIZE_T((1ULL << psLMAllocArrayData->uiLog2AllocSize)-uiInAllocOffset);
			}

			PVR_ASSERT(uiBytesCopyableFromAlloc != 0);
			PVR_ASSERT(uiAllocIndex < psLMAllocArrayData->uiTotalNumPages);
			PVR_ASSERT(uiInAllocOffset < (1ULL << psLMAllocArrayData->uiLog2AllocSize));

			eError = _MapAlloc(psLMAllocArrayData->psDevNode,
								&psLMAllocArrayData->pasDevPAddr[uiAllocIndex],
								psLMAllocArrayData->uiContigAllocSize,
								psLMAllocArrayData->bFwLocalAlloc,
								PVRSRV_MEMALLOCFLAG_CPU_UNCACHED,
								&pvMapping);
			PVR_GOTO_IF_ERROR(eError, e0);
			pcKernelPointer = pvMapping;
			pfnCopyBytes(&pcBuffer[uiBufferOffset], &pcKernelPointer[uiInAllocOffset], uiBytesCopyableFromAlloc);

			_UnMapAlloc(psLMAllocArrayData->psDevNode,
						psLMAllocArrayData->uiContigAllocSize,
						psLMAllocArrayData->bFwLocalAlloc,
						0,
						pvMapping);

			uiBufferOffset += uiBytesCopyableFromAlloc;
			uiBytesToCopy -= uiBytesCopyableFromAlloc;
			uiOffset += uiBytesCopyableFromAlloc;
			uiBytesCopied += uiBytesCopyableFromAlloc;
		}
	}
	else
	{
			PVR_ASSERT((uiOffset + uiBufSz) <= psLMAllocArrayData->uiContigAllocSize);
			PVR_ASSERT(psLMAllocArrayData->uiContigAllocSize != 0);
			eError = _MapAlloc(psLMAllocArrayData->psDevNode,
								&psLMAllocArrayData->pasDevPAddr[0],
								psLMAllocArrayData->uiContigAllocSize,
								psLMAllocArrayData->bFwLocalAlloc,
								PVRSRV_MEMALLOCFLAG_CPU_UNCACHED,
								&pvMapping);
			PVR_GOTO_IF_ERROR(eError, e0);
			pcKernelPointer = pvMapping;
			pfnCopyBytes(pcBuffer, &pcKernelPointer[uiOffset], uiBufSz);

			_UnMapAlloc(psLMAllocArrayData->psDevNode,
						psLMAllocArrayData->uiContigAllocSize,
						psLMAllocArrayData->bFwLocalAlloc,
						0,
						pvMapping);

			uiBytesCopied = uiBufSz;
	}
	*puiNumBytes = uiBytesCopied;
	return PVRSRV_OK;
e0:
	*puiNumBytes = uiBytesCopied;
	return eError;
}

static void ReadLocalMem(IMG_UINT8 *pcBuffer,
						 IMG_UINT8 *pcPMR,
						 size_t uiSize)
{
	/* NOTE: 'CachedMemCopy' means the operating system default memcpy, which
	 *       we *assume* in the LMA code will be faster, and doesn't need to
	 *       worry about ARM64.
	 */
	OSCachedMemCopy(pcBuffer, pcPMR, uiSize);
}

static PVRSRV_ERROR
PMRReadBytesLocalMem(PMR_IMPL_PRIVDATA pvPriv,
				  IMG_DEVMEM_OFFSET_T uiOffset,
				  IMG_UINT8 *pcBuffer,
				  size_t uiBufSz,
				  size_t *puiNumBytes)
{
	return CopyBytesLocalMem(pvPriv,
							 uiOffset,
							 pcBuffer,
							 uiBufSz,
							 puiNumBytes,
							 ReadLocalMem);
}

static void WriteLocalMem(IMG_UINT8 *pcBuffer,
						  IMG_UINT8 *pcPMR,
						  size_t uiSize)
{
	/* NOTE: 'CachedMemCopy' means the operating system default memcpy, which
	 *       we *assume* in the LMA code will be faster, and doesn't need to
	 *       worry about ARM64.
	 */
	OSCachedMemCopy(pcPMR, pcBuffer, uiSize);
}

static PVRSRV_ERROR
PMRWriteBytesLocalMem(PMR_IMPL_PRIVDATA pvPriv,
					  IMG_DEVMEM_OFFSET_T uiOffset,
					  IMG_UINT8 *pcBuffer,
					  size_t uiBufSz,
					  size_t *puiNumBytes)
{
	return CopyBytesLocalMem(pvPriv,
							 uiOffset,
							 pcBuffer,
							 uiBufSz,
							 puiNumBytes,
							 WriteLocalMem);
}

/*************************************************************************/ /*!
@Function       PMRChangeSparseMemLocalMem
@Description    This function Changes the sparse mapping by allocating and
                freeing of pages. It also changes the GPU maps accordingly.
@Return         PVRSRV_ERROR failure code
*/ /**************************************************************************/
static PVRSRV_ERROR
PMRChangeSparseMemLocalMem(PMR_IMPL_PRIVDATA pPriv,
                           const PMR *psPMR,
                           IMG_UINT32 ui32AllocPageCount,
                           IMG_UINT32 *pai32AllocIndices,
                           IMG_UINT32 ui32FreePageCount,
                           IMG_UINT32 *pai32FreeIndices,
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
                           PMR_IMPL_ZOMBIEPAGES *ppvZombiePages,
#endif
                           IMG_UINT32 uiFlags)
{
	PVRSRV_ERROR eError = PVRSRV_ERROR_INVALID_PARAMS;

	IMG_UINT32 ui32AdtnlAllocPages = 0;
	IMG_UINT32 ui32AdtnlFreePages = 0;
	IMG_UINT32 ui32CommonRequstCount = 0;
	IMG_UINT32 ui32Loop = 0;
	IMG_UINT32 ui32Index = 0;
	IMG_UINT32 uiAllocpgidx;
	IMG_UINT32 uiFreepgidx;

	PMR_LMALLOCARRAY_DATA *psPMRPageArrayData = (PMR_LMALLOCARRAY_DATA *)pPriv;

#if defined(DEBUG)
	IMG_BOOL bPoisonFail = IMG_FALSE;
	IMG_BOOL bZeroFail = IMG_FALSE;
#endif

	/* Fetch the Page table array represented by the PMR */
	IMG_DEV_PHYADDR *psPageArray = psPMRPageArrayData->pasDevPAddr;
	PMR_MAPPING_TABLE *psPMRMapTable = PMR_GetMappigTable(psPMR);

#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	*ppvZombiePages = NULL;
#endif
	/* The incoming request is classified into two operations independent of
	 * each other: alloc & free pages.
	 * These operations can be combined with two mapping operations as well
	 * which are GPU & CPU space mappings.
	 *
	 * From the alloc and free page requests, the net amount of pages to be
	 * allocated or freed is computed. Pages that were requested to be freed
	 * will be reused to fulfil alloc requests.
	 *
	 * The order of operations is:
	 * 1. Allocate new pages from the OS
	 * 2. Move the free pages from free request to alloc positions.
	 * 3. Free the rest of the pages not used for alloc
	 *
	 * Alloc parameters are validated at the time of allocation
	 * and any error will be handled then. */

	if (SPARSE_RESIZE_BOTH == (uiFlags & SPARSE_RESIZE_BOTH))
	{
		ui32CommonRequstCount = (ui32AllocPageCount > ui32FreePageCount) ?
				ui32FreePageCount : ui32AllocPageCount;

		PDUMP_PANIC(SPARSEMEM_SWAP, "Request to swap alloc & free pages not supported");
	}

	if (SPARSE_RESIZE_ALLOC == (uiFlags & SPARSE_RESIZE_ALLOC))
	{
		ui32AdtnlAllocPages = ui32AllocPageCount - ui32CommonRequstCount;
	}
	else
	{
		ui32AllocPageCount = 0;
	}

	if (SPARSE_RESIZE_FREE == (uiFlags & SPARSE_RESIZE_FREE))
	{
		ui32AdtnlFreePages = ui32FreePageCount - ui32CommonRequstCount;
	}
	else
	{
		ui32FreePageCount = 0;
	}

	PVR_LOG_RETURN_IF_FALSE(
	    (ui32CommonRequstCount | ui32AdtnlAllocPages | ui32AdtnlFreePages) != 0,
	    "Invalid combination of parameters: ui32CommonRequstCount,"
	    " ui32AdtnlAllocPages and ui32AdtnlFreePages.",
	    PVRSRV_ERROR_INVALID_PARAMS
	);

	{
		/* Validate the free page indices */
		if (ui32FreePageCount)
		{
			if (NULL != pai32FreeIndices)
			{
				for (ui32Loop = 0; ui32Loop < ui32FreePageCount; ui32Loop++)
				{
					uiFreepgidx = pai32FreeIndices[ui32Loop];

					if (uiFreepgidx >= psPMRPageArrayData->uiTotalNumPages)
					{
						PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_DEVICEMEM_OUT_OF_RANGE, e0);
					}

					if (INVALID_PAGE_ADDR == psPageArray[uiFreepgidx].uiAddr)
					{
						PVR_LOG_GOTO_WITH_ERROR("psPageArray[uiFreepgidx].uiAddr", eError, PVRSRV_ERROR_INVALID_PARAMS, e0);
					}
				}
			}else
			{
				PVR_DPF((PVR_DBG_ERROR,
				         "%s: Given non-zero free count but missing indices array",
				         __func__));
				return PVRSRV_ERROR_INVALID_PARAMS;
			}
		}

		/*The following block of code verifies any issues with common alloc page indices */
		for (ui32Loop = ui32AdtnlAllocPages; ui32Loop < ui32AllocPageCount; ui32Loop++)
		{
			uiAllocpgidx = pai32AllocIndices[ui32Loop];
			if (uiAllocpgidx >= psPMRPageArrayData->uiTotalNumPages)
			{
				PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_DEVICEMEM_OUT_OF_RANGE, e0);
			}

			if ((INVALID_PAGE_ADDR != psPageArray[uiAllocpgidx].uiAddr) ||
					(TRANSLATION_INVALID != psPMRMapTable->aui32Translation[uiAllocpgidx]))
			{
				PVR_LOG_GOTO_WITH_ERROR("Trying to allocate already allocated page again", eError, PVRSRV_ERROR_INVALID_PARAMS, e0);
			}
		}


		ui32Loop = 0;

		/* Allocate new pages */
		if (0 != ui32AdtnlAllocPages)
		{
			/* Say how many pages to allocate */
			psPMRPageArrayData->uiPagesToAlloc = ui32AdtnlAllocPages;

			eError = _AllocLMPages(psPMRPageArrayData, pai32AllocIndices);
			PVR_LOG_GOTO_IF_ERROR(eError, "_AllocLMPages", e0);

			/* Mark the corresponding pages of translation table as valid */
			for (ui32Loop = 0; ui32Loop < ui32AdtnlAllocPages; ui32Loop++)
			{
				psPMRMapTable->aui32Translation[pai32AllocIndices[ui32Loop]] = pai32AllocIndices[ui32Loop];
			}

			psPMRMapTable->ui32NumPhysChunks += ui32AdtnlAllocPages;
		}

		ui32Index = ui32Loop;

		/* Move the corresponding free pages to alloc request */
		for (ui32Loop = 0; ui32Loop < ui32CommonRequstCount; ui32Loop++, ui32Index++)
		{

			uiAllocpgidx = pai32AllocIndices[ui32Index];
			uiFreepgidx  = pai32FreeIndices[ui32Loop];
			psPageArray[uiAllocpgidx] = psPageArray[uiFreepgidx];

			psPMRMapTable->aui32Translation[uiFreepgidx] = TRANSLATION_INVALID;
			psPMRMapTable->aui32Translation[uiAllocpgidx] = uiAllocpgidx;
			psPageArray[uiFreepgidx].uiAddr = INVALID_PAGE_ADDR;

			/* Be sure to honour the attributes associated with the allocation
			 * such as zeroing, poisoning etc. */
			if (psPMRPageArrayData->bPoisonOnAlloc)
			{
				eError = _PoisonAlloc(psPMRPageArrayData->psDevNode,
				                      &psPMRPageArrayData->pasDevPAddr[uiAllocpgidx],
				                      psPMRPageArrayData->bFwLocalAlloc,
				                      psPMRPageArrayData->uiContigAllocSize,
				                      PVRSRV_POISON_ON_ALLOC_VALUE);

				/* Consider this as a soft failure and go ahead but log error to kernel log */
				if (eError != PVRSRV_OK)
				{
#if defined(DEBUG)
					bPoisonFail = IMG_TRUE;
#endif
				}
			}
			else
			{
				if (psPMRPageArrayData->bZeroOnAlloc)
				{
					eError = _ZeroAlloc(psPMRPageArrayData->psDevNode,
					                    &psPMRPageArrayData->pasDevPAddr[uiAllocpgidx],
					                    psPMRPageArrayData->bFwLocalAlloc,
					                    psPMRPageArrayData->uiContigAllocSize);
					/* Consider this as a soft failure and go ahead but log error to kernel log */
					if (eError != PVRSRV_OK)
					{
#if defined(DEBUG)
						/*Don't think we need to zero any pages further*/
						bZeroFail = IMG_TRUE;
#endif
					}
				}
			}
		}

		/*Free the additional free pages */
		if (0 != ui32AdtnlFreePages)
		{
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
			PMR_LMALLOCARRAY_DATA *psExtractedPagesPageArray = NULL;

			eError = _ExtractPages(psPMRPageArrayData, &pai32FreeIndices[ui32Loop], ui32AdtnlFreePages, &psExtractedPagesPageArray);
			PVR_LOG_GOTO_IF_ERROR(eError, "_ExtractPages", e0);

			/* Zombify pages to get proper stats */
			eError = PMRZombifyLocalMem(psExtractedPagesPageArray, NULL);
			PVR_LOG_IF_ERROR(eError, "PMRZombifyLocalMem");

			*ppvZombiePages = psExtractedPagesPageArray;
#else
			eError = _FreeLMPages(psPMRPageArrayData, &pai32FreeIndices[ui32Loop], ui32AdtnlFreePages);
			PVR_LOG_GOTO_IF_ERROR(eError, "_FreeLMPages", e0);
#endif /* SUPPORT_PMR_PAGES_DEFERRED_FREE */
			ui32Index = ui32Loop;
			ui32Loop = 0;

			while (ui32Loop++ < ui32AdtnlFreePages)
			{
				/*Set the corresponding mapping table entry to invalid address */
				psPMRMapTable->aui32Translation[pai32FreeIndices[ui32Index++]] = TRANSLATION_INVALID;
			}

			psPMRMapTable->ui32NumPhysChunks -= ui32AdtnlFreePages;
		}

	}

#if defined(DEBUG)
	if (IMG_TRUE == bPoisonFail)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Error in poisoning the page", __func__));
	}

	if (IMG_TRUE == bZeroFail)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Error in zeroing the page", __func__));
	}
#endif

	/* Update the PMR memory holding information */
	eError = PVRSRV_OK;

e0:
	return eError;
}

/*************************************************************************/ /*!
@Function       PMRChangeSparseMemCPUMapLocalMem
@Description    This function Changes CPU maps accordingly
@Return         PVRSRV_ERROR failure code
*/ /**************************************************************************/
static
PVRSRV_ERROR PMRChangeSparseMemCPUMapLocalMem(PMR_IMPL_PRIVDATA pPriv,
                                              const PMR *psPMR,
                                              IMG_UINT64 sCpuVAddrBase,
                                              IMG_UINT32 ui32AllocPageCount,
                                              IMG_UINT32 *pai32AllocIndices,
                                              IMG_UINT32 ui32FreePageCount,
                                              IMG_UINT32 *pai32FreeIndices)
{
	PVRSRV_ERROR eError;
	IMG_DEV_PHYADDR *psPageArray;
	PMR_LMALLOCARRAY_DATA *psPMRPageArrayData = (PMR_LMALLOCARRAY_DATA *)pPriv;
	uintptr_t sCpuVABase = sCpuVAddrBase;
	IMG_CPU_PHYADDR sCpuAddrPtr;
	IMG_BOOL bValid = IMG_FALSE;

	/*Get the base address of the heap */
	eError = PMR_CpuPhysAddr(psPMR,
	                         psPMRPageArrayData->uiLog2AllocSize,
	                         1,
	                         0,	/* offset zero here mean first page in the PMR */
	                         &sCpuAddrPtr,
	                         &bValid);
	PVR_LOG_RETURN_IF_ERROR(eError, "PMR_CpuPhysAddr");

	/* Phys address of heap is computed here by subtracting the offset of this page
	 * basically phys address of any page = Base address of heap + offset of the page */
	sCpuAddrPtr.uiAddr -= psPMRPageArrayData->pasDevPAddr[0].uiAddr;
	psPageArray = psPMRPageArrayData->pasDevPAddr;

	return OSChangeSparseMemCPUAddrMap((void **)psPageArray,
	                                   sCpuVABase,
	                                   sCpuAddrPtr,
	                                   ui32AllocPageCount,
	                                   pai32AllocIndices,
	                                   ui32FreePageCount,
	                                   pai32FreeIndices,
	                                   IMG_TRUE);
}

static PMR_IMPL_FUNCTAB _sPMRLMAFuncTab = {
	/* pfnLockPhysAddresses */
	&PMRLockSysPhysAddressesLocalMem,
	/* pfnUnlockPhysAddresses */
	&PMRUnlockSysPhysAddressesLocalMem,
	/* pfnDevPhysAddr */
	&PMRSysPhysAddrLocalMem,
	/* pfnAcquireKernelMappingData */
	&PMRAcquireKernelMappingDataLocalMem,
	/* pfnReleaseKernelMappingData */
	&PMRReleaseKernelMappingDataLocalMem,
#if defined(INTEGRITY_OS)
	/* pfnMapMemoryObject */
	NULL,
	/* pfnUnmapMemoryObject */
	NULL,
#endif
	/* pfnReadBytes */
	&PMRReadBytesLocalMem,
	/* pfnWriteBytes */
	&PMRWriteBytesLocalMem,
	/* pfnUnpinMem */
	NULL,
	/* pfnPinMem */
	NULL,
	/* pfnChangeSparseMem*/
	&PMRChangeSparseMemLocalMem,
	/* pfnChangeSparseMemCPUMap */
	&PMRChangeSparseMemCPUMapLocalMem,
#if defined(SUPPORT_PMR_PAGES_DEFERRED_FREE)
	/* pfnFreeZombiePages */
	&PMRFreeZombiePagesLocalMem,
#endif
	/* pfnMMap */
	NULL,
	/* pfnFinalize */
	&PMRFinalizeLocalMem,
	/* pfnGetPMRFactoryLock */
	NULL,
	/* pfnReleasePMRFactoryLock */
	NULL,
#if defined(SUPPORT_PMR_DEFERRED_FREE)
	/* pfnZombify */
	&PMRZombifyLocalMem,
#endif
};

PVRSRV_ERROR
PhysmemNewLocalRamBackedPMR(CONNECTION_DATA *psConnection,
                            PVRSRV_DEVICE_NODE *psDevNode,
                            IMG_DEVMEM_SIZE_T uiSize,
                            IMG_DEVMEM_SIZE_T uiChunkSize,
                            IMG_UINT32 ui32NumPhysChunks,
                            IMG_UINT32 ui32NumVirtChunks,
                            IMG_UINT32 *pui32MappingTable,
                            IMG_UINT32 uiLog2AllocPageSize,
                            PVRSRV_MEMALLOCFLAGS_T uiFlags,
                            const IMG_CHAR *pszAnnotation,
                            IMG_PID uiPid,
                            PMR **ppsPMRPtr,
                            IMG_UINT32 ui32PDumpFlags)
{
	PVRSRV_ERROR eError;
	PVRSRV_ERROR eError2;
	PMR *psPMR = NULL;
	PMR_LMALLOCARRAY_DATA *psPrivData = NULL;
	PMR_FLAGS_T uiPMRFlags;
	PHYS_HEAP *psPhysHeap;
	IMG_BOOL bZero;
	IMG_BOOL bPoisonOnAlloc;
	IMG_BOOL bPoisonOnFree;
	IMG_BOOL bOnDemand;
	IMG_BOOL bContig;
	IMG_BOOL bFwLocalAlloc;
	IMG_BOOL bCpuLocalAlloc;
	FIRMWARE_ALLOC_TYPE eFwAllocType;
	IMG_UINT32 ui32OSid;

	/* For sparse requests we have to do the allocation
	 * in chunks rather than requesting one contiguous block */
	if (ui32NumPhysChunks != ui32NumVirtChunks || ui32NumVirtChunks > 1)
	{
		if (PVRSRV_CHECK_KERNEL_CPU_MAPPABLE(uiFlags))
		{
			PVR_DPF((PVR_DBG_ERROR,
					"%s: LMA kernel mapping functions currently "
					"don't work with discontiguous memory.",
					__func__));
			PVR_GOTO_WITH_ERROR(eError, PVRSRV_ERROR_INVALID_PARAMS, errorOnParam);
		}
		bContig = IMG_FALSE;
	}
	else
	{
		bContig = IMG_TRUE;
	}

	bOnDemand = PVRSRV_CHECK_ON_DEMAND(uiFlags) ? IMG_TRUE : IMG_FALSE;
	bCpuLocalAlloc = PVRSRV_CHECK_CPU_LOCAL(uiFlags) ? IMG_TRUE : IMG_FALSE;
	bZero = PVRSRV_CHECK_ZERO_ON_ALLOC(uiFlags) ? IMG_TRUE : IMG_FALSE;
	bPoisonOnAlloc = PVRSRV_CHECK_POISON_ON_ALLOC(uiFlags) ? IMG_TRUE : IMG_FALSE;
	bPoisonOnFree = PVRSRV_CHECK_POISON_ON_FREE(uiFlags) ? IMG_TRUE : IMG_FALSE;
	bFwLocalAlloc = PVRSRV_CHECK_FW_LOCAL(uiFlags) ? IMG_TRUE : IMG_FALSE;
	eFwAllocType = PVRSRV_FW_ALLOC_TYPE(uiFlags);
	ui32OSid = PVRSRV_FW_RAW_ALLOC_OSID(uiFlags);

	if (bFwLocalAlloc)
	{
		psPhysHeap = psDevNode->apsPhysHeap[PVRSRV_DEVICE_PHYS_HEAP_FW_LOCAL];
	}
	else if (bCpuLocalAlloc)
	{
		psPhysHeap = psDevNode->apsPhysHeap[PVRSRV_DEVICE_PHYS_HEAP_CPU_LOCAL];
	}
	else
	{
		psPhysHeap = psDevNode->apsPhysHeap[PVRSRV_DEVICE_PHYS_HEAP_GPU_LOCAL];
	}

	/* Create Array structure that holds the physical pages */
	eError = _AllocLMPageArray(psDevNode,
	                           uiChunkSize * ui32NumVirtChunks,
	                           uiChunkSize,
	                           ui32NumPhysChunks,
	                           ui32NumVirtChunks,
	                           pui32MappingTable,
	                           uiLog2AllocPageSize,
	                           bZero,
	                           bPoisonOnAlloc,
	                           bPoisonOnFree,
	                           bContig,
	                           bOnDemand,
	                           bFwLocalAlloc,
	                           eFwAllocType,
	                           ui32OSid,
	                           psPhysHeap,
	                           uiFlags,
	                           uiPid,
	                           &psPrivData,
	                           psConnection);
	PVR_GOTO_IF_ERROR(eError, errorOnAllocPageArray);

	if (!bOnDemand)
	{
		/* Allocate the physical pages */
		eError = _AllocLMPages(psPrivData, pui32MappingTable);
		PVR_GOTO_IF_ERROR(eError, errorOnAllocPages);
	}

	/* In this instance, we simply pass flags straight through.

	   Generically, uiFlags can include things that control the PMR
	   factory, but we don't need any such thing (at the time of
	   writing!), and our caller specifies all PMR flags so we don't
	   need to meddle with what was given to us.
	*/
	uiPMRFlags = (PMR_FLAGS_T)(uiFlags & PVRSRV_MEMALLOCFLAGS_PMRFLAGSMASK);
	/* check no significant bits were lost in cast due to different
	   bit widths for flags */
	PVR_ASSERT(uiPMRFlags == (uiFlags & PVRSRV_MEMALLOCFLAGS_PMRFLAGSMASK));

	if (bOnDemand)
	{
		PDUMPCOMMENT("Deferred Allocation PMR (LMA)");
	}

	eError = PMRCreatePMR(psDevNode,
						  psPhysHeap,
						  uiSize,
						  uiChunkSize,
						  ui32NumPhysChunks,
						  ui32NumVirtChunks,
						  pui32MappingTable,
						  uiLog2AllocPageSize,
						  uiPMRFlags,
						  pszAnnotation,
						  &_sPMRLMAFuncTab,
						  psPrivData,
						  PMR_TYPE_LMA,
						  &psPMR,
						  ui32PDumpFlags);
	PVR_LOG_GOTO_IF_ERROR(eError, "PMRCreatePMR", errorOnCreate);

	*ppsPMRPtr = psPMR;
	return PVRSRV_OK;

errorOnCreate:
	if (!bOnDemand && psPrivData->iNumPagesAllocated)
	{
		eError2 = _FreeLMPages(psPrivData, NULL, 0);
		PVR_ASSERT(eError2 == PVRSRV_OK);
	}

errorOnAllocPages:
	eError2 = _FreeLMPageArray(psPrivData);
	PVR_ASSERT(eError2 == PVRSRV_OK);

errorOnAllocPageArray:
errorOnParam:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}
