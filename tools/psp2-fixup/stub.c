/*
 * Copyright (C) 2015 PSP2SDK Project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "elf_psp2.h"
#include "elf.h"
#include "stub.h"

static int addStub(Psp2_Rela_Short *relaFstub, const scn_t *fstub,
	uint32_t *fnid, const Elf32_Rel *relMark, Elf32_Word relMarkSize,
	const void *mark, Elf32_Addr offset,
	const scn_t *scns, const Elf32_Sym *symtab)
{
	const Elf32_Sym *sym;
	Elf32_Word i, type;
	Elf32_Addr addend;

	if (relaFstub == NULL || fnid == NULL
		|| relMark == NULL || mark == NULL)
	{
		return EINVAL;
	}

	for (i = 0; i < relMarkSize / sizeof(Elf32_Rel); i++) {
		if (relMark[i].r_offset == offset
			+ offsetof(sce_libgen_mark_stub, stub))
		{
			type = ELF32_R_TYPE(relMark[i].r_info);
			sym = symtab + ELF32_R_SYM(relMark[i].r_info);

			PSP2_R_SET_SHORT(relaFstub, 1);
			PSP2_R_SET_SYMSEG(relaFstub, scns[sym->st_shndx].phndx);
			PSP2_R_SET_TYPE(relaFstub, type);
			PSP2_R_SET_DATSEG(relaFstub, fstub->phndx);
			PSP2_R_SET_OFFSET(relaFstub,
				fstub->segOffset + relMark[i].r_offset);

			addend = sym->st_value;
			if (type == R_ARM_ABS32 || type == R_ARM_TARGET1)
				addend += *(uint32_t *)((uintptr_t)mark + relMark[i].r_offset);
			PSP2_R_SET_ADDEND(relaFstub, addend);
		}
	}

	*fnid = *(uint32_t *)((uintptr_t)mark + offset
			+ offsetof(sce_libgen_mark_stub, nid));

	return 0;
}

int buildStubs(sceScns_t *sceScns, FILE *srcFp, const scn_t *scns,
	const char *strtab, Elf32_Sym *symtab)
{
	union {
		uint8_t size;
		sce_libgen_mark_head head;
		sce_libgen_mark_stub stub;
	} *markContent, *p;
	sceLib_stub *stubHeads;
	Psp2_Rela_Short *relaFstubEnt, *relaStubEnt;
	Elf32_Off offset, fnidOffset, fstubOffset, stubOffset;
	Elf32_Rel *relMarkContent;
	Elf32_Sym *sym;
	Elf32_Word i, *fnidEnt;

	if (sceScns == NULL || srcFp == NULL
		|| scns == NULL || strtab == NULL || symtab == NULL)
	{
		return EINVAL;
	}

	markContent = malloc(sceScns->mark->shdr.sh_size);
	if (markContent == NULL) {
		perror(strtab + sceScns->mark->shdr.sh_name);
		return errno;
	}

	relMarkContent = malloc(sceScns->relMark->orgSize);
	if (relMarkContent == NULL) {
		perror(strtab + sceScns->relMark->shdr.sh_name);
		return errno;
	}

	sceScns->relFstub->content = malloc(sceScns->relFstub->shdr.sh_size);
	if (sceScns->relFstub->content == NULL) {
		perror(strtab + sceScns->relFstub->shdr.sh_name);
		return errno;
	}

	sceScns->relStub->content = malloc(sceScns->relStub->shdr.sh_size);
	if (sceScns->relFstub->content == NULL) {
		perror(strtab + sceScns->relStub->shdr.sh_name);
		return errno;
	}

	sceScns->fnid->content = malloc(sceScns->fnid->shdr.sh_size);
	if (sceScns->fnid->content == NULL) {
		perror(strtab + sceScns->fnid->shdr.sh_name);
		return errno;
	}

	sceScns->stub->content = malloc(sceScns->stub->shdr.sh_size);
	if (sceScns->stub->content == NULL) {
		perror(strtab + sceScns->stub->shdr.sh_name);
		return errno;
	}

	if (fseek(srcFp, sceScns->mark->orgOffset, SEEK_SET)) {
		perror(strtab + sceScns->mark->shdr.sh_name);
		return errno;
	}

	if (fread(markContent, sceScns->mark->shdr.sh_size, 1, srcFp) <= 0) {
		strtab += sceScns->mark->shdr.sh_name;
		if (feof(srcFp)) {
			fprintf(stderr, "%s: Unexpected EOF\n", strtab);
			return EILSEQ;
		} else {
			perror(strtab);
			return errno;
		}
	}

	if (fseek(srcFp, sceScns->relMark->orgOffset, SEEK_SET)) {
		perror(strtab + sceScns->relMark->shdr.sh_name);
		return errno;
	}

	if (fread(relMarkContent, sceScns->relMark->orgSize, 1, srcFp) <= 0) {
		strtab += sceScns->relMark->shdr.sh_name;
		if (feof(srcFp)) {
			fprintf(stderr, "%s: Unexpected EOF\n", strtab);
			return EILSEQ;
		} else {
			perror(strtab);
			return errno;
		}
	}

	relaFstubEnt = sceScns->relFstub->content;
	relaStubEnt = sceScns->relStub->content;
	fnidEnt = sceScns->fnid->content;
	stubHeads = sceScns->stub->content;
	p = markContent;
	fnidOffset = 0;
	fstubOffset = 0;
	stubOffset = 0;
	offset = 0;
	while (offset < sceScns->mark->shdr.sh_size) {
		if (p->size == sizeof(sce_libgen_mark_head)) {
			stubHeads->size = sizeof(sceLib_stub);
			stubHeads->ver = 1;
			stubHeads->flags = 0;
			stubHeads->nid = p->head.nid;

			PSP2_R_SET_SHORT(relaStubEnt, 1);
			PSP2_R_SET_SYMSEG(relaStubEnt, sceScns->fnid->phndx);
			PSP2_R_SET_TYPE(relaStubEnt, R_ARM_ABS32);
			PSP2_R_SET_DATSEG(relaStubEnt, sceScns->stub->phndx);
			PSP2_R_SET_OFFSET(relaStubEnt, sceScns->stub->segOffset
				+ stubOffset + offsetof(sceLib_stub, funcNids));
			PSP2_R_SET_ADDEND(relaStubEnt, fnidOffset);
			relaStubEnt++;

			PSP2_R_SET_SHORT(relaStubEnt, 1);
			PSP2_R_SET_SYMSEG(relaStubEnt, sceScns->fstub->phndx);
			PSP2_R_SET_TYPE(relaStubEnt, R_ARM_ABS32);
			PSP2_R_SET_DATSEG(relaStubEnt, sceScns->stub->phndx);
			PSP2_R_SET_OFFSET(relaStubEnt, sceScns->stub->segOffset
				+ stubOffset + offsetof(sceLib_stub, funcStubs));
			PSP2_R_SET_ADDEND(relaStubEnt, fstubOffset);
			relaStubEnt++;

			stubHeads->varNids = 0;
			stubHeads->varStubs = 0;
			stubHeads->unkNids = 0;
			stubHeads->unkStubs = 0;

			stubHeads->funcNum = 0;
			stubHeads->varNum = 0;
			stubHeads->unkNum = 0;

			for (i = 0; i < sceScns->relMark->orgSize / sizeof(Elf32_Rel); i++) {
				sym = symtab + ELF32_R_SYM(relMarkContent[i].r_info);
				if (sym->st_value != sceScns->mark->shdr.sh_addr + offset)
					continue;

				addStub(relaFstubEnt, sceScns->fstub, fnidEnt,
					relMarkContent, sceScns->relMark->shdr.sh_size,
					markContent,
					relMarkContent[i].r_offset
						- offsetof(sce_libgen_mark_stub, head),
					scns, symtab);
				relaFstubEnt++;
				fnidEnt++;
				fstubOffset += sizeof(Psp2_Rela_Short);
				fnidOffset += 4;
				stubHeads->funcNum++;
			}

			stubOffset += sizeof(sceLib_stub);
			stubHeads++;
		} else if (p->size == 0) {
			printf("%s: Invalid mark\n",
				strtab + sceScns->mark->shdr.sh_name);
			return EILSEQ;
		}

		offset += p->size;
		p = (void *)((uintptr_t)p + p->size);
	}

	free(markContent);
	free(relMarkContent);

	return 0;
}
