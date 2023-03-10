/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2020 Loongson Technology Co., Ltd.
 */
#ifndef __ASM_PREFETCH_H
#define __ASM_PREFETCH_H

#define Pref_Load	0
#define Pref_Store	8

#ifdef __ASSEMBLY__

	.macro	__pref hint addr
#ifdef CONFIG_CPU_HAS_PREFETCH
	preld	\hint, \addr, 0
#endif
	.endm

	.macro	pref_load addr
	__pref	Pref_Load, \addr
	.endm

	.macro	pref_store addr
	__pref	Pref_Store, \addr
	.endm

#endif

#endif /* __ASM_PREFETCH_H */
