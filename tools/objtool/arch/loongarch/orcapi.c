// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <stdlib.h>

#include "../../orc.h"
#include "../../warn.h"

#ifndef R_LARCH_SOP_PUSH_PCREL
#define R_LARCH_SOP_PUSH_PCREL 22
#endif

#ifndef R_LARCH_SOP_PUSH_DUP
#define R_LARCH_SOP_PUSH_DUP 24
#endif

#ifndef R_LARCH_SOP_PUSH_ABSOLUTE
#define R_LARCH_SOP_PUSH_ABSOLUTE 23
#endif

#ifndef R_LARCH_SOP_SR
#define R_LARCH_SOP_SR 34
#endif

#ifndef R_LARCH_SOP_SL
#define R_LARCH_SOP_SL 33
#endif

#ifndef R_LARCH_SOP_SUB
#define R_LARCH_SOP_SUB 32
#endif

#ifndef R_LARCH_SOP_POP_32_U
#define R_LARCH_SOP_POP_32_U 46
#endif

static struct orc_entry empty = {
	.sp_reg = ORC_REG_UNDEFINED,
	.type = ORC_TYPE_CALL,
};

int create_orc(struct objtool_file *file)
{
	struct instruction *insn;

	for_each_insn(file, insn) {
		struct orc_entry *orc = &insn->orc;
		struct cfi_reg *cfa = &insn->cfi.cfa;
		struct cfi_reg *fp = &insn->cfi.regs[CFI_FP];
		struct cfi_reg *ra = &insn->cfi.regs[CFI_RA];

		orc->end = insn->cfi.end;

		if (cfa->base == CFI_UNDEFINED) {
			orc->sp_reg = ORC_REG_UNDEFINED;
			continue;
		}

		switch (cfa->base) {
		case CFI_SP:
			orc->sp_reg = ORC_REG_SP;
			break;
		case CFI_FP:
			orc->sp_reg = ORC_REG_FP;
			break;
		default:
			WARN_FUNC("unknown CFA base reg %d",
				  insn->sec, insn->offset, cfa->base);
			return -1;
		}

		switch (fp->base) {
		case CFI_UNDEFINED:
			orc->fp_reg = ORC_REG_UNDEFINED;
			orc->fp_offset = 0;
			break;
		case CFI_CFA:
			orc->fp_reg = ORC_REG_PREV_SP;
			orc->fp_offset = fp->offset;
			break;
		default:
			WARN_FUNC("unknown FP base reg %d",
					insn->sec, insn->offset, fp->base);
		}

		switch (ra->base) {
		case CFI_UNDEFINED:
			orc->ra_reg = ORC_REG_UNDEFINED;
			orc->ra_offset = 0;
			break;
		case CFI_CFA:
			orc->ra_reg = ORC_REG_PREV_SP;
			orc->ra_offset = ra->offset;
			break;
		default:
			WARN_FUNC("unknown RA base reg %d",
				  insn->sec, insn->offset, ra->base);
		}

		orc->sp_offset = cfa->offset;
		orc->type = insn->cfi.type;
	}

	return 0;
}

int arch_create_orc_entry(struct elf *elf, struct section *u_sec, struct section *ip_relocsec,
				unsigned int idx, struct section *insn_sec,
				unsigned long insn_off, struct orc_entry *o)
{
	struct orc_entry *orc;
	struct reloc *reloc, *p, *c;

	/* populate ORC data */
	orc = (struct orc_entry *)u_sec->data->d_buf + idx;
	memcpy(orc, o, sizeof(*orc));

	/* populate reloc for ip
	 * Since loongarch64 has no PC32 at present, fake as follows,
	 * R_LARCH_SOP_PUSH_PCREL
	 * R_LARCH_SOP_PUSH_DUP
	 * R_LARCH_SOP_PUSH_ABSOLUTE
	 * R_LARCH_SOP_SR
	 * R_LARCH_SOP_PUSH_ABSOLUTE
	 * R_LARCH_SOP_SL
	 * R_LARCH_SOP_SUB
	 * R_LARCH_SOP_POP_32_U
	 */
	reloc = malloc(sizeof(*reloc) * 8);
	if (!reloc) {
		perror("malloc");
		return -1;
	}
	memset(reloc, 0, sizeof(*reloc) * 8);

	insn_to_reloc_sym_addend(insn_sec, insn_off, reloc);
	if (!reloc->sym) {
		WARN("missing symbol for insn at offset 0x%lx",
		     insn_off);
		return -1;
	}

	p = reloc;
	c = reloc;

	p->type = R_LARCH_SOP_PUSH_PCREL;
	p->offset = idx * sizeof(int);
	p->sec = ip_relocsec;

	c++;
	c->type = R_LARCH_SOP_PUSH_DUP;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_PUSH_ABSOLUTE;
	c->addend = 0x20;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_SR;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_PUSH_ABSOLUTE;
	c->addend = 0x20;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_SL;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_SUB;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;
	p = c;

	c++;
	c->type = R_LARCH_SOP_POP_32_U;
	c->offset = idx * sizeof(int);
	c->sec = ip_relocsec;
	p->next = c;

	elf_add_reloc(elf, reloc);

	return 0;
}

int arch_create_orc_entry_empty(struct elf *elf, struct section *u_sec,
				struct section *ip_relasec, unsigned int idx,
				struct section *insn_sec, unsigned long insn_off)
{
	return arch_create_orc_entry(elf, u_sec, ip_relasec, idx,
				      insn_sec, insn_off, &empty);
}

static const char *reg_name(unsigned int reg)
{
	switch (reg) {
	case ORC_REG_SP:
		return "sp";
	case ORC_REG_FP:
		return "fp";
	case ORC_REG_PREV_SP:
		return "prevsp";
	default:
		return "?";
	}
}

static const char *orc_type_name(unsigned int type)
{
	switch (type) {
	case ORC_TYPE_CALL:
		return "call";
	case ORC_TYPE_REGS:
		return "regs";
	default:
		return "?";
	}
}

static void print_reg(unsigned int reg, int offset)
{
	if (reg == ORC_REG_UNDEFINED)
		printf(" (und) ");
	else
		printf("%s + %3d", reg_name(reg), offset);
}

void arch_print_reg(struct orc_entry orc)
{
	printf(" sp:");

	print_reg(orc.sp_reg, orc.sp_offset);

	printf(" fp:");

	print_reg(orc.fp_reg, orc.fp_offset);

	printf(" ra:");
	print_reg(orc.ra_reg, orc.ra_offset);

	printf(" type:%s end:%d\n",
	       orc_type_name(orc.type), orc.end);
}
