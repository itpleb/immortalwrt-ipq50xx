/*
 * Copyright (c) 2018, ARM Limited and Contributors. All rights reserved.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#ifndef __PLATFORM_DEF_H__
#define __PLATFORM_DEF_H__

/* Enable the dynamic translation tables library. */
#define PLAT_XLAT_TABLES_DYNAMIC     1

#include <board_qti_def.h>
#include <common_def.h>

/*----------------------------------------------------------------------------*/
/* SOC_VERSION definitions */
/*----------------------------------------------------------------------------*/
#define QTI_A53_MIDR     0x51AF8014

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* MPIDR_PRIMARY_CPU
 * You just need to have the correct core_affinity_val i.e. [7:0]
 * and cluster_affinity_val i.e. [15:8]
 * the other bits will be ignored */
/*----------------------------------------------------------------------------*/
#define MPIDR_PRIMARY_CPU	0x0000
/*----------------------------------------------------------------------------*/

/*
 *  Macros for local power states encoded by State-ID field
 *  within the power-state parameter.
 */
/* Local power state for power domains in Run state. */
#define QTI_LOCAL_STATE_RUN	0
/* Local power state for clock-gating. Valid only for CPU and not cluster power
   domains */
#define QTI_LOCAL_STATE_STB	1
/* Local power state for retention. Valid for CPU and cluster power
   domains */
#define QTI_LOCAL_STATE_RET	2
/* Local power state for OFF/power-down. Valid for CPU and cluster power
   domains */
#define QTI_LOCAL_STATE_OFF	3
/* Local power state for OFF/power-down. Valid for CPU and cluster power
   domains */
#define QTI_LOCAL_STATE_DEEPOFF	0xF

/*
 * This macro defines the deepest retention state possible. A higher state
 * id will represent an invalid or a power down state.
 */
#define PLAT_MAX_RET_STATE	QTI_LOCAL_STATE_RET

/*
 * This macro defines the deepest power down states possible. Any state ID
 * higher than this is invalid.
 */
#define PLAT_MAX_OFF_STATE	QTI_LOCAL_STATE_DEEPOFF

/******************************************************************************
 * Required platform porting definitions common to all ARM standard platforms
 *****************************************************************************/

/*
 * Platform specific page table and MMU setup constants.
 */
#define MAX_MMAP_REGIONS	(PLAT_QTI_MMAP_ENTRIES)

#define PLAT_PHY_ADDR_SPACE_SIZE	(1ull << 36)
#define PLAT_VIRT_ADDR_SPACE_SIZE	(1ull << 36)

#define ARM_CACHE_WRITEBACK_SHIFT	6

/*
 * Some data must be aligned on the biggest cache line size in the platform.
 * This is known only to the platform as it might have a combination of
 * integrated and external caches.
 */
#define CACHE_WRITEBACK_GRANULE		(1 << ARM_CACHE_WRITEBACK_SHIFT)

/*
 * One cache line needed for bakery locks on ARM platforms
 */
#define PLAT_PERCPU_BAKERY_LOCK_SIZE	(1 * CACHE_WRITEBACK_GRANULE)

/*----------------------------------------------------------------------------*/
/* PSCI power domain topology definitions */
/*----------------------------------------------------------------------------*/
/* One domain to represent Cx level */
#define PLAT_CX_RAIL_COUNT		1

/* There is one top-level FCM cluster */
#define PLAT_CLUSTER_COUNT       	1

/* No. of cores in the FCM cluster */
#define PLAT_CLUSTER0_CORE_COUNT	4

#define PLATFORM_CORE_COUNT		(PLAT_CLUSTER0_CORE_COUNT)

#define PLAT_NUM_PWR_DOMAINS		(PLAT_CX_RAIL_COUNT	+\
					PLAT_CLUSTER_COUNT	+\
					PLATFORM_CORE_COUNT)

#define PLAT_MAX_PWR_LVL		2

/*****************************************************************************/
/* Memory mapped Generic timer interfaces  */
/*****************************************************************************/

/*----------------------------------------------------------------------------*/
/* GIC-600 constants */
/*----------------------------------------------------------------------------*/
#define BASE_GICD_BASE		0xB000000
#define BASE_GICC_BASE		0xB002000

#define QTI_GICD_BASE      	BASE_GICD_BASE
#define QTI_GICC_BASE      	BASE_GICC_BASE


/*----------------------------------------------------------------------------*/
/* Device address space for mapping. Excluding starting 4K */
/*----------------------------------------------------------------------------*/
#define QTI_DEVICE_BASE				0x00000000UL
#define QTI_DEVICE_SIZE				(0x40000000UL - QTI_DEVICE_BASE)

#define QTI_DDR_BASE                            0x40000000UL
#define QTI_SHARED_IMEM_BASE			0x08600000
#define QTI_SHARED_IMEM_RO_BASE                 (QTI_SHARED_IMEM_BASE + 0x1000)
#define QTI_SHARED_IMEM_RO_SIZE			0x4000
#define QTI_SHARED_IMEM_RW_BASE			(QTI_SHARED_IMEM_RO_BASE + QTI_SHARED_IMEM_RO_SIZE)
#define QTI_SHARED_IMEM_RW_SIZE			0x2000
#define QTI_SHARED_IMEM_DBG_STACK_SIZE		0x200
#define QTI_SHARED_IMEM_TF_STACK_CANARY_ADDR	(QTI_SHARED_IMEM_BASE + 0x7F0)

/*******************************************************************************
 * BL31 specific defines.
 ******************************************************************************/
/*
 * Put BL31 at DDR as per memory map. BL31_BASE is calculated using the
 * current BL31 debug size plus a little space for growth.
 */
#define BL31_BASE						0x4A600000
#define BL31_SIZE						0x200000
#define QTI_TRUSTED_MAILBOX_SIZE				0x1000
#define BL31_LIMIT						(BL31_BASE + BL31_SIZE - QTI_TRUSTED_MAILBOX_SIZE)

/*******************************************************************************
 * Diag Region Defines
 ******************************************************************************/
/*
 * DIAG Start is placed 8K apart from __PIL_REGION_END__
 */
#define QTI_DIAG_RG_START               0x4A669000
#define QTI_DIAG_RG_SIZE                0x3000
#define QTI_DIAG_RG_LIMIT               QTI_DIAG_RG_START + QTI_DIAG_RG_SIZE

#define QTI_PIL_RG_SIZE			0x1000
#define QTI_PIL_HEAP_RG_SIZE		0x18000
#define QTI_PIL_HEAP_REGION_START	(BL31_LIMIT - QTI_PIL_HEAP_RG_SIZE)
/*----------------------------------------------------------------------------*/
/* Mailbox base address */
/*----------------------------------------------------------------------------*/
#define QTI_TRUSTED_MAILBOX_BASE		(BL31_BASE + BL31_SIZE - QTI_TRUSTED_MAILBOX_SIZE)
/*----------------------------------------------------------------------------*/

#endif /* __PLATFORM_DEF_H__ */
