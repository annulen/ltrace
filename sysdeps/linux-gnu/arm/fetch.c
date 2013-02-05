/*
 * This file is part of ltrace.
 * Copyright (C) 2013 Petr Machata, Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include <assert.h>
#include <elf.h>
#include <libelf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "backend.h"
#include "fetch.h"
#include "library.h"
#include "ltrace-elf.h"
#include "proc.h"
#include "ptrace.h"
#include "regs.h"
#include "type.h"
#include "value.h"

static int
get_hardfp(uint64_t abi_vfp_args)
{
	if (abi_vfp_args == 2)
		fprintf(stderr,
			"Tag_ABI_VFP_args value 2 (tool chain-specific "
			"conventions) not supported.\n");
	return abi_vfp_args == 1;
}

int
arch_elf_init(struct ltelf *lte, struct library *lib)
{
	/* Nothing in this section is strictly critical.  It's not
	 * that much of a deal if we fail to guess right whether the
	 * ABI is softfp or hardfp.  */
	unsigned hardfp = 0;

	Elf_Scn *scn;
	Elf_Data *data;
	GElf_Shdr shdr;
	if (elf_get_section_type(lte, SHT_ARM_ATTRIBUTES, &scn, &shdr) < 0
	    || (scn != NULL && (data = elf_loaddata(scn, &shdr)) == NULL)) {
		fprintf(stderr,
			"Error when obtaining ARM attribute section: %s\n",
			elf_errmsg(-1));
		goto done;

	} else if (scn != NULL && data != NULL) {
		GElf_Xword offset = 0;
		uint8_t version;
		if (elf_read_next_u8(data, &offset, &version) < 0) {
			goto done;
		} else if (version != 'A') {
			fprintf(stderr, "Unsupported ARM attribute section "
				"version %d ('%c').\n", version, version);
			goto done;
		}

		do {
			const char signature[] = "aeabi";
			/* N.B. LEN is including the length field
			 * itself.  */
			uint32_t sec_len;
			if (elf_read_u32(data, offset, &sec_len) < 0
			    || !elf_can_read_next(data, offset, sec_len)) {
				goto done;
			}
			const GElf_Xword next_offset = offset + sec_len;
			offset += 4;

			if (sec_len < 4 + sizeof signature
			    || strcmp(signature, data->d_buf + offset) != 0)
				goto skip;
			offset += sizeof signature;

			const GElf_Xword offset0 = offset;
			uint64_t tag;
			uint32_t sub_len;
			if (elf_read_next_uleb128(data, &offset, &tag) < 0
			    || elf_read_next_u32(data, &offset, &sub_len) < 0
			    || !elf_can_read_next(data, offset0, sub_len))
				goto done;

			if (tag != 1)
				/* IHI0045D_ABI_addenda: "section and
				 * symbol attributes are deprecated
				 * [...] consumers are permitted to
				 * ignore them."  */
				goto skip;

			while (offset < offset0 + sub_len) {
				if (elf_read_next_uleb128(data,
							  &offset, &tag) < 0)
					goto done;

				switch (tag) {
					uint64_t v;
				case 6: /* Tag_CPU_arch */
				case 7: /* Tag_CPU_arch_profile */
				case 8: /* Tag_ARM_ISA_use */
				case 9: /* Tag_THUMB_ISA_use */
				case 10: /* Tag_FP_arch */
				case 11: /* Tag_WMMX_arch */
				case 12: /* Tag_Advanced_SIMD_arch */
				case 13: /* Tag_PCS_config */
				case 14: /* Tag_ABI_PCS_R9_use */
				case 15: /* Tag_ABI_PCS_RW_data */
				case 16: /* Tag_ABI_PCS_RO_data */
				case 17: /* Tag_ABI_PCS_GOT_use */
				case 18: /* Tag_ABI_PCS_wchar_t */
				case 19: /* Tag_ABI_FP_rounding */
				case 20: /* Tag_ABI_FP_denormal */
				case 21: /* Tag_ABI_FP_exceptions */
				case 22: /* Tag_ABI_FP_user_exceptions */
				case 23: /* Tag_ABI_FP_number_model */
				case 24: /* Tag_ABI_align_needed */
				case 25: /* Tag_ABI_align_preserved */
				case 26: /* Tag_ABI_enum_size */
				case 27: /* Tag_ABI_HardFP_use */
				case 28: /* Tag_ABI_VFP_args */
				case 29: /* Tag_ABI_WMMX_args */
				case 30: /* Tag_ABI_optimization_goals */
				case 31: /* Tag_ABI_FP_optimization_goals */
				case 32: /* Tag_compatibility */
				case 34: /* Tag_CPU_unaligned_access */
				case 36: /* Tag_FP_HP_extension */
				case 38: /* Tag_ABI_FP_16bit_format */
				case 42: /* Tag_MPextension_use */
				case 70: /* Tag_MPextension_use as well */
				case 44: /* Tag_DIV_use */
				case 64: /* Tag_nodefaults */
				case 66: /* Tag_T2EE_use */
				case 68: /* Tag_Virtualization_use */
				uleb128:
					if (elf_read_next_uleb128
						(data, &offset, &v) < 0)
						goto done;
					if (tag == 28)
						hardfp = get_hardfp(v);
					if (tag != 32)
						continue;

					/* Tag 32 has two arguments,
					 * fall through.  */

				case 4:	/* Tag_CPU_raw_name */
				case 5:	/* Tag_CPU_name */
				case 65: /* Tag_also_compatible_with */
				case 67: /* Tag_conformance */
				ntbs:
					offset += strlen(data->d_buf
							 + offset) + 1;
					continue;
				}

				/* Handle unknown tags in a generic
				 * manner, if possible.  */
				if (tag <= 32) {
					fprintf(stderr,
						"Unknown tag %lld "
						"at offset %#llx "
						"of ARM attribute section.",
						tag, offset);
					goto skip;
				} else if (tag % 2 == 0) {
					goto uleb128;
				} else {
					goto ntbs;
				}
			}

		skip:
			offset = next_offset;

		} while (elf_can_read_next(data, offset, 1));

	}

done:
	lib->arch.hardfp = hardfp;
	return 0;
}

void
arch_elf_destroy(struct ltelf *lte)
{
}

void
arch_library_init(struct library *lib)
{
}

void
arch_library_destroy(struct library *lib)
{
}

void
arch_library_clone(struct library *retp, struct library *lib)
{
	retp->arch = lib->arch;
}

struct fetch_context {
	struct pt_regs regs;
	struct {
		union {
			double d[32];
			float s[64];
		};
		uint32_t fpscr;
	} fpregs;
	bool hardfp:1;
};

static int
fetch_register_banks(struct process *proc, struct fetch_context *context)
{
	if (context->hardfp
	    && ptrace(PTRACE_GETVFPREGS, proc->pid,
		      NULL, &context->fpregs) == -1)
		return -1;

	return 0;
}

struct fetch_context *
arch_fetch_arg_init(enum tof type, struct process *proc,
		    struct arg_type_info *ret_info)
{
	struct fetch_context *context = malloc(sizeof(*context));
	context->hardfp = proc->libraries->arch.hardfp;
	if (context == NULL
	    || fetch_register_banks(proc, context) < 0) {
		free(context);
		return NULL;
	}

	return context;
}

struct fetch_context *
arch_fetch_arg_clone(struct process *proc,
		     struct fetch_context *context)
{
	struct fetch_context *clone = malloc(sizeof(*context));
	if (clone == NULL)
		return NULL;
	*clone = *context;
	return clone;
}

int
arch_fetch_arg_next(struct fetch_context *ctx, enum tof type,
		    struct process *proc,
		    struct arg_type_info *info, struct value *valuep)
{
	return 0;
}

int
arch_fetch_retval(struct fetch_context *ctx, enum tof type,
		  struct process *proc, struct arg_type_info *info,
		  struct value *valuep)
{
	if (fetch_register_banks(proc, ctx) < 0)
		return -1;

	switch (info->type) {
	case ARGTYPE_VOID:
		return 0;

	case ARGTYPE_FLOAT:
	case ARGTYPE_DOUBLE:
		if (ctx->hardfp) {
			size_t sz = type_sizeof(proc, info);
			assert(sz != (size_t)-1);
			unsigned char *data = value_reserve(valuep, sz);
			if (data == NULL)
				return -1;
			memmove(data, &ctx->fpregs, sz);
			return 0;
		}
		/* Fall through.  */

	case ARGTYPE_CHAR:
	case ARGTYPE_SHORT:
	case ARGTYPE_USHORT:
	case ARGTYPE_INT:
	case ARGTYPE_UINT:
	case ARGTYPE_LONG:
	case ARGTYPE_ULONG:
	case ARGTYPE_POINTER:
	case ARGTYPE_ARRAY:
	case ARGTYPE_STRUCT:
		return -1;
	}
	assert(info->type != info->type);
	abort();
}

void
arch_fetch_arg_done(struct fetch_context *context)
{
	free(context);
}
