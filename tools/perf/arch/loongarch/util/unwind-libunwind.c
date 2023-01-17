// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <libunwind.h>
#include "perf_regs.h"
#include "../../util/unwind.h"
#include "util/debug.h"

int libunwind__arch_reg_id(int regnum)
{
	switch (regnum) {
	case UNW_LOONGARCH_R1:
		return PERF_REG_LOONGARCH_R1;
	case UNW_LOONGARCH_R2:
		return PERF_REG_LOONGARCH_R2;
	case UNW_LOONGARCH_R3:
		return PERF_REG_LOONGARCH_R3;
	case UNW_LOONGARCH_R4:
		return PERF_REG_LOONGARCH_R4;
	case UNW_LOONGARCH_R5:
		return PERF_REG_LOONGARCH_R5;
	case UNW_LOONGARCH_R6:
		return PERF_REG_LOONGARCH_R6;
	case UNW_LOONGARCH_R7:
		return PERF_REG_LOONGARCH_R7;
	case UNW_LOONGARCH_R8:
		return PERF_REG_LOONGARCH_R8;
	case UNW_LOONGARCH_R9:
		return PERF_REG_LOONGARCH_R9;
	case UNW_LOONGARCH_R10:
		return PERF_REG_LOONGARCH_R10;
	case UNW_LOONGARCH_R11:
		return PERF_REG_LOONGARCH_R11;
	case UNW_LOONGARCH_R12:
		return PERF_REG_LOONGARCH_R12;
	case UNW_LOONGARCH_R13:
		return PERF_REG_LOONGARCH_R13;
	case UNW_LOONGARCH_R14:
		return PERF_REG_LOONGARCH_R14;
	case UNW_LOONGARCH_R15:
		return PERF_REG_LOONGARCH_R15;
	case UNW_LOONGARCH_R16:
		return PERF_REG_LOONGARCH_R16;
	case UNW_LOONGARCH_R17:
		return PERF_REG_LOONGARCH_R17;
	case UNW_LOONGARCH_R18:
		return PERF_REG_LOONGARCH_R18;
	case UNW_LOONGARCH_R19:
		return PERF_REG_LOONGARCH_R19;
	case UNW_LOONGARCH_R20:
		return PERF_REG_LOONGARCH_R20;
	case UNW_LOONGARCH_R21:
		return PERF_REG_LOONGARCH_R21;
	case UNW_LOONGARCH_R22:
		return PERF_REG_LOONGARCH_R22;
	case UNW_LOONGARCH_R23:
		return PERF_REG_LOONGARCH_R23;
	case UNW_LOONGARCH_R24:
		return PERF_REG_LOONGARCH_R24;
	case UNW_LOONGARCH_R25:
		return PERF_REG_LOONGARCH_R25;
	case UNW_LOONGARCH_R26:
		return PERF_REG_LOONGARCH_R26;
	case UNW_LOONGARCH_R27:
		return PERF_REG_LOONGARCH_R27;
	case UNW_LOONGARCH_R28:
		return PERF_REG_LOONGARCH_R28;
	case UNW_LOONGARCH_R29:
		return PERF_REG_LOONGARCH_R29;
	case UNW_LOONGARCH_R30:
		return PERF_REG_LOONGARCH_R30;
	case UNW_LOONGARCH_R31:
		return PERF_REG_LOONGARCH_R31;
	case UNW_LOONGARCH_PC:
		return PERF_REG_LOONGARCH_PC;
	default:
		pr_err("unwind: invalid reg id %d\n", regnum);
		return -EINVAL;
	}

	return -EINVAL;
}