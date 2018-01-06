/*
 * Functions for dumping PE code and data sections
 *
 * Copyright 2018 Zebediah Figura
 *
 * This file is part of Semblance.
 *
 * Semblance is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Semblance is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Semblance; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <string.h>
#include "semblance.h"
#include "pe.h"
#include "x86_instr.h"

#ifdef USE_WARN
#define warn_at(...) \
    do { fprintf(stderr, "Warning: %x: ", ip); \
        fprintf(stderr, __VA_ARGS__); } while(0)
#else
#define warn_at(...)
#endif

struct section *addr2section(dword addr, const struct pe *pe) {
    /* Even worse than the below, some data is sensitive to which section it's in! */

    int i;
    for (i = 0; i < pe->header.file.NumberOfSections; i++) {
         if (addr >= pe->sections[i].address && addr < pe->sections[i].address + pe->sections[i].min_alloc)
            return &pe->sections[i];
    }

    return NULL;
}

long addr2offset(dword addr, const struct pe *pe) {
    /* Everything inside a PE file is built so that the file is read while it's
     * already loaded. Offsets aren't file offsets, they're *memory* offsets.
     * We don't want to load the whole file, so we have to search through each
     * section to figure out where in the *file* a virtual address points. */

    struct section *section = addr2section(addr, pe);
    if (!section) return 0;
    return addr - section->address + section->offset;
}

/* index function */
static char *get_export_name(dword ip, const struct pe *pe) {
    int i;
    for (i = 0; i < pe->export_count; i++) {
        if (pe->exports[i].address == ip)
            return pe->exports[i].name;
    }
    return NULL;
}

static char *get_imported_name(dword offset, const struct pe *pe) {
    unsigned i;

    offset -= pe->header.opt.ImageBase;

    for (i = 0; i < pe->import_count; i++) {
        unsigned index = (offset - pe->imports[i].nametab_addr) / sizeof(dword);
        if (index < pe->imports[i].count)
            return pe->imports[i].nametab[index];
    }
    return NULL;
}

static int print_pe_instr(const struct section *sec, dword ip, byte *p, char *out, const struct pe *pe) {
    struct instr instr = {0};
    char arg0[32] = "", arg1[32] = "";
    unsigned len;

    char *comment = NULL;
    char ip_string[9];

    len = get_instr(ip, p, &instr, 1);

    sprintf(ip_string, "%8x", ip);

    /* Check for relocations.
     * PE relocations work a little differently, in that instead of directly
     * altering each of the relevant bytes (well, dwords) in the image, we 
     * relocate a large block of addresses at once and then reference it.
     * As a result we need to check if the given offset falls within the
     * relocated section of the import tables. */

    if (instr.op.opcode == 0xff && (instr.op.subcode == 2 || instr.op.subcode == 4)
        && instr.modrm_disp == DISP_16 && instr.modrm_reg == 8) {
        /* call/jmp to an absolute memory address */
        comment = get_imported_name(instr.arg0, pe);
    }

    /* check if we are referencing a named export */
    if (instr.op.arg0 == REL16 && !comment)
        comment = get_export_name(instr.arg0, pe);

    print_instr(out, ip_string, p, len, sec->instr_flags[ip - sec->address], &instr, arg0, arg1, comment);

    return len;
}

static void print_disassembly(const struct section *sec, const struct pe *pe) {
    dword relip = 0, ip;

    byte buffer[MAX_INSTR];
    char out[256];

    while (relip < sec->length) {
        fseek(f, sec->offset + relip, SEEK_SET);

        /* find a valid instruction */
        if (!(sec->instr_flags[relip] & INSTR_VALID)) {
            if (opts & DISASSEMBLE_ALL) {
                /* still skip zeroes */
                if (read_byte() == 0) {
                    printf("     ...\n");
                    relip++;
                }
                while (read_byte() == 0) relip++;
            } else {
                printf("     ...\n");
                while ((relip < sec->length) && (relip < sec->min_alloc) && !(sec->instr_flags[relip] & INSTR_VALID)) relip++;
            }
        }

        ip = relip + sec->address;
        fseek(f, sec->offset + relip, SEEK_SET);
        if (relip >= sec->length || relip >= sec->min_alloc) return;

        /* Instructions can "hang over" the end of a segment.
         * Zero should be supplied. */
        memset(buffer, 0, sizeof(buffer));

        fread(buffer, 1, min(sizeof(buffer), sec->length - relip), f);

        if (sec->instr_flags[relip] & INSTR_FUNC) {
            char *name = get_export_name(ip, pe);
            printf("\n");
            printf("%x <%s>:\n", ip, name ? name : "no name");
        }

        relip += print_pe_instr(sec, ip, buffer, out, pe);
        printf("%s\n", out);
    }
    putchar('\n');
}

static void scan_segment(dword ip, struct pe *pe) {
    struct section *sec = addr2section(ip, pe);
    dword relip;

    byte buffer[MAX_INSTR];
    struct instr instr;
    int instr_length;
    int i;

//    fprintf(stderr, "scanning at %x, in section %s\n", ip, sec ? sec->name : "<none>");

    if (!sec) {
        warn_at("Attempt to scan byte not in image.\n");
        return;
    }

    relip = ip - sec->address;

    if ((sec->instr_flags[relip] & (INSTR_VALID|INSTR_SCANNED)) == INSTR_SCANNED)
        warn_at("Attempt to scan byte that does not begin instruction.\n");

    /* This code assumes that one stretch of code won't span multiple sections.
     * Is this a valid assumption? */

    while (relip < sec->length) {
        /* check if we've already read from here */
        if (sec->instr_flags[relip] & INSTR_SCANNED) return;

        /* read the instruction */
        fseek(f, sec->offset + relip, SEEK_SET);
        memset(buffer, 0, sizeof(buffer));
        fread(buffer, 1, min(sizeof(buffer), sec->length-relip), f);
        instr_length = get_instr(ip, buffer, &instr, 1);

        /* mark the bytes */
        sec->instr_flags[relip] |= INSTR_VALID;
        for (i = relip; i < relip+instr_length && i < sec->min_alloc; i++) sec->instr_flags[i] |= INSTR_SCANNED;

        /* instruction which hangs over the minimum allocation */
        if (i < ip+instr_length && i == sec->min_alloc) break;

        /* handle conditional and unconditional jumps */
        /* todo: reloc */
        if (instr.op.flags & OP_BRANCH) {
            /* relative jump, loop, or call */
            struct section *tsec = addr2section(instr.arg0, pe);

            if (tsec)
            {
                dword trelip = instr.arg0 - tsec->address;

                if (!strcmp(instr.op.name, "call"))
                    tsec->instr_flags[trelip] |= INSTR_FUNC;
                else
                    tsec->instr_flags[trelip] |= INSTR_JUMP;
    
                /* scan it */
                scan_segment(instr.arg0, pe);
            } else
                warn_at("Branch '%s' to byte %x not in image.\n", instr.op.name, instr.arg0);
        }

        if (instr.op.flags & OP_STOP)
            return;

        ip += instr_length;
        relip = ip - sec->address;
    }

    warn_at("Scan reached the end of section.\n");
}

static void print_section_flags(dword flags) {
    char buffer[1024] = "";
    int alignment = (flags & 0x00f00000) / 0x100000;

    /* Most of these shouldn't occur in an image file, either because they're
     * COFF flags that PE doesn't want or because they're object-only. Print
     * the COFF names. */
    if (flags & 0x00000001) strcat(buffer, ", STYP_DSECT");
    if (flags & 0x00000002) strcat(buffer, ", STYP_NOLOAD");
    if (flags & 0x00000004) strcat(buffer, ", STYP_GROUP");
    if (flags & 0x00000008) strcat(buffer, ", STYP_PAD");
    if (flags & 0x00000010) strcat(buffer, ", STYP_COPY");
    if (flags & 0x00000020) strcat(buffer, ", code");
    if (flags & 0x00000040) strcat(buffer, ", data");
    if (flags & 0x00000080) strcat(buffer, ", bss");
    if (flags & 0x00000100) strcat(buffer, ", S_NEWCFN");
    if (flags & 0x00000200) strcat(buffer, ", STYP_INFO");
    if (flags & 0x00000400) strcat(buffer, ", STYP_OVER");
    if (flags & 0x00000800) strcat(buffer, ", STYP_LIB");
    if (flags & 0x00001000) strcat(buffer, ", COMDAT");
    if (flags & 0x00002000) strcat(buffer, ", STYP_MERGE");
    if (flags & 0x00004000) strcat(buffer, ", STYP_REVERSE_PAD");
    if (flags & 0x00008000) strcat(buffer, ", FARDATA");
    if (flags & 0x00010000) strcat(buffer, ", (unknown flags 0x10000)");
    if (flags & 0x00020000) strcat(buffer, ", purgeable");  /* or 16BIT */
    if (flags & 0x00040000) strcat(buffer, ", locked");
    if (flags & 0x00080000) strcat(buffer, ", preload");
    if (flags & 0x01000000) strcat(buffer, ", extended relocations");
    if (flags & 0x02000000) strcat(buffer, ", discardable");
    if (flags & 0x04000000) strcat(buffer, ", not cached");
    if (flags & 0x08000000) strcat(buffer, ", not paged");
    if (flags & 0x10000000) strcat(buffer, ", shared");
    if (flags & 0x20000000) strcat(buffer, ", executable");
    if (flags & 0x40000000) strcat(buffer, ", readable");
    if (flags & 0x80000000) strcat(buffer, ", writable");

    printf("    Flags: 0x%08x (%s)\n", flags, buffer+2);
    printf("    Alignment: %d (2**%d)\n", 1 << alignment, alignment);
}

/* We don't actually know what sections contain code. In theory it could be any
 * of them. Fortunately we actually have everything we need already. */

void read_sections(struct pe *pe) {
    int i;

    /* We already read the section header (unlike NE, we had to in order to read
     * everything else), so our job now is just to scan the section contents. */

    /* todo: relocations */

    for (i = 0; i < pe->export_count; i++) {
        scan_segment(pe->exports[i].address, pe);
    }

    if (!(pe->header.file.Characteristics & 0x2000))
        scan_segment(pe->header.opt.AddressOfEntryPoint, pe);
}

void print_sections(struct pe *pe) {
    int i;
    struct section *sec;

    for (i = 0; i < pe->header.file.NumberOfSections; i++) {
        sec = &pe->sections[i];

        putchar('\n');
        printf("Section %s (start = 0x%x, length = 0x%x, minimum allocation = 0x%x):\n",
            sec->name, sec->offset, sec->length, sec->min_alloc);
        printf("    Address: %x\n", sec->address);
        print_section_flags(sec->flags);

        if (sec->flags & 0x40) {
            /* data: todo */
        } else if (sec->flags & 0x20) {
            /* todo: FULL_CONTENTS */
            print_disassembly(sec, pe);
        }
    }
}