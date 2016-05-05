/*
 * Copyright (C) 2009 by David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ARMV8_DPM_H
#define __ARMV8_DPM_H

/**
 * @file
 * This is the interface to the Debug Programmers Model for ARMv6 and
 * ARMv7 processors.  ARMv6 processors (such as ARM11xx implementations)
 * introduced a model which became part of the ARMv7-AR architecture
 * which is most familiar through the Cortex-A series parts.  While
 * specific details differ (like how to write the instruction register),
 * the high level models easily support shared code because those
 * registers are compatible.
 */

struct dpm_bpwp {
	unsigned number;
	uint32_t address;
	uint32_t control;
	/* true if hardware state needs flushing */
	bool dirty;
};

struct dpm_bp {
	struct breakpoint *bp;
	struct dpm_bpwp bpwp;
};

struct dpm_wp {
	struct watchpoint *wp;
	struct dpm_bpwp bpwp;
};

/**
 * This wraps an implementation of DPM primitives.  Each interface
 * provider supplies a structure like this, which is the glue between
 * upper level code and the lower level hardware access.
 *
 * It is a PRELIMINARY AND INCOMPLETE set of primitives, starting with
 * support for CPU register access.
 */
struct arm_dpm {
	struct arm *arm;

	/** Cache of DIDR */
	uint64_t didr;

	/** Invoke before a series of instruction operations */
	int (*prepare)(struct arm_dpm *);

	/** Invoke after a series of instruction operations */
	int (*finish)(struct arm_dpm *);

	/** Runs one instruction. */
	int (*instr_execute)(struct arm_dpm *,
			uint32_t opcode);


	/* WRITE TO CPU */

	/** Runs one instruction, writing data to DCC before execution. */
	int (*instr_write_data_dcc)(struct arm_dpm *,
			uint32_t opcode, uint32_t data);

	/** Runs one instruction, writing data to R0 before execution. */
	int (*instr_write_data_r0)(struct arm_dpm *,
			uint32_t opcode, uint32_t data);


	/** Runs one instruction, writing 64 bits data to DCC before execution. */
	int (*instr_write_data_dcc_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t data);

	/** Runs one instruction, writing 64-bits data to R0 before execution. */
	int (*instr_write_data_r0_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t data);

	/** Optional core-specific operation invoked after CPSR writes. */
	int (*instr_cpsr_sync)(struct arm_dpm *dpm);

	/* READ FROM CPU */

	/** Runs one instruction, reading data from dcc after execution. */
	int (*instr_read_data_dcc)(struct arm_dpm *,
			uint32_t opcode, uint32_t *data);

	/** Runs one instruction, reading data from r0 after execution. */
	int (*instr_read_data_r0)(struct arm_dpm *,
			uint32_t opcode, uint32_t *data);

	/** Runs one instruction, reading data from dcc after execution. */
	int (*instr_read_data_dcc_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t *data);

	/** Runs one instruction, reading data from r0 after execution. */
	int (*instr_read_data_r0_64)(struct arm_dpm *,
			uint32_t opcode, uint64_t *data);

	/* BREAKPOINT/WATCHPOINT SUPPORT */

	/**
	 * Enables one breakpoint or watchpoint by writing to the
	 * hardware registers.  The specified breakpoint/watchpoint
	 * must currently be disabled.  Indices 0..15 are used for
	 * breakpoints; indices 16..31 are for watchpoints.
	 */
	int (*bpwp_enable)(struct arm_dpm *, unsigned index_value,
			uint32_t addr, uint32_t control);

	/**
	 * Disables one breakpoint or watchpoint by clearing its
	 * hardware control registers.  Indices are the same ones
	 * accepted by bpwp_enable().
	 */
	int (*bpwp_disable)(struct arm_dpm *, unsigned index_value);

	/* The breakpoint and watchpoint arrays are private to the
	 * DPM infrastructure.  There are nbp indices in the dbp
	 * array.  There are nwp indices in the dwp array.
	 */

	unsigned nbp;
	unsigned nwp;
	struct dpm_bp *dbp;
	struct dpm_wp *dwp;

	/** Address of the instruction which triggered a watchpoint. */
	uint64_t wp_pc;

	/** Recent value of DSCR. */
	uint32_t dscr;

	/* FIXME -- read/write DCSR methods and symbols */
};

int armv8_dpm_setup(struct arm_dpm *dpm);
int armv8_dpm_initialize(struct arm_dpm *dpm);

int armv8_dpm_read_current_registers(struct arm_dpm *);
int dpmv8_modeswitch(struct arm_dpm *dpm, enum arm_mode mode);


int armv8_dpm_write_dirty_registers(struct arm_dpm *, bool bpwp);

void armv8_dpm_report_wfar(struct arm_dpm *, uint64_t wfar);

/* DSCR bits; see ARMv7a arch spec section C10.3.1.
 * Not all v7 bits are valid in v6.
 */
#define DSCR_DEBUG_STATUS_MASK		(0x1F <<  0)
#define DSCR_ERR					(0x1 <<  6)
#define DSCR_SYS_ERROR_PEND			(0x1 <<  7)
#define DSCR_CUR_EL					(0x3 <<  8)
#define DSCR_EL_STATUS_MASK			(0xF << 10)
#define DSCR_HDE					(0x1 << 14)
#define DSCR_SDD					(0x1 << 16)
#define DSCR_NON_SECURE             (0x1 << 18)
#define DSCR_MA						(0x1 << 20)
#define DSCR_TDA					(0x1 << 21)
#define DSCR_INTDIS_MASK			(0x3 << 22)
#define DSCR_ITE					(0x1 << 24)
#define DSCR_PIPE_ADVANCE           (0x1 << 25)
#define DSCR_TXU					(0x1 << 26)
#define DSCR_RTO					(0x1 << 27) /* bit 28 is reserved */
#define DSCR_ITO					(0x1 << 28)
#define DSCR_DTR_TX_FULL            (0x1 << 29)
#define DSCR_DTR_RX_FULL            (0x1 << 30) /* bit 31 is reserved */



/* Methods of entry into debug mode */
#define DSCR_NON_DEBUG			(0x2)
#define DSCR_RESTARTING			(0x1)
#define DSCR_BKPT				(0x7)
#define DSCR_EXT_DEBUG			(0x13)
#define DSCR_HALT_STEP_NORMAL	(0x1B)
#define DSCR_HALT_STEP_EXECLU	(0x1F)
#define DSCR_OS_UNLOCK			(0x23)
#define DSCR_RESET_CATCH		(0x27)
#define DSCR_WATCHPOINT			(0x2B)
#define DSCR_HLT				(0x2F)
#define DSCR_SW_ACCESS_DBG		(0x33)
#define DSCR_EXCEPTION_CATCH	(0x37)
#define DSCR_HALT_STEP			(0x3B)
#define DSCR_HALT_MASK			(0x3C)

#define DSCR_ENTRY(dscr)            ((dscr) & 0x3)
#define DSCR_RUN_MODE(dscr)         ((dscr) & (DSCR_HALT_MASK))

/*DRCR registers*/
#define DRCR_CSE				(1 << 2)
#define DRCR_CSPA				(1 << 3)
#define DRCR_CBRRQ				(1 << 4)

/* EDECR value */
# define EDECR_SS_HALTING_STEP_ENABLE (1 << 2)

/* DTR modes */
#define DSCR_EXT_DCC_NON_BLOCKING     (0x0 << 20)
#define DSCR_EXT_DCC_STALL_MODE       (0x1 << 20)
#define DSCR_EXT_DCC_FAST_MODE        (0x2 << 20)  /* bits 22, 23 are reserved */


/* DRCR (debug run control register) bits */
#define DRCR_HALT				(1 << 0)
#define DRCR_RESTART			(1 << 1)
#define DRCR_CLEAR_EXCEPTIONS	(1 << 2)

/* PRCR (processor debug status register) bits */
#define PRSR_PU					(1 << 0)
#define PRSR_SPD				(1 << 1)
#define PRSR_RESET				(1 << 2)
#define PRSR_SR					(1 << 3)
#define PRSR_HALT				(1 << 4)
#define PRSR_OSLK				(1 << 5)
#define PRSR_DLK				(1 << 6)
#define PRSR_EDAD				(1 << 7)
#define PRSR_SDAD				(1 << 8)
#define PRSR_EPMAD				(1 << 9)
#define PRSR_SPMAD				(1 << 10)
#define PRSR_SDR				(1 << 11)



void armv8_dpm_report_dscr(struct arm_dpm *dpm, uint32_t dcsr);

#endif /* __ARM_DPM_H */
