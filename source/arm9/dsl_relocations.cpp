// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <cstdio>
#include <cstdint>

#include <nds/ndstypes.h>

#include "dsl.h"
#include "dsl_internal.hpp"

// ELF relocation definitions
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Word;

typedef struct {
    Elf32_Addr r_offset; // Location (virtual address)
    Elf32_Word r_info;   // (symbol table index << 8) | (type of relocation)
} Elf32_Rel;

// ARM relocation types
#define R_ARM_NONE          0
#define R_ARM_ABS32         2
#define R_ARM_REL32         3
#define R_ARM_THM_CALL      10
#define R_ARM_BASE_PREL     25
#define R_ARM_GOT_BREL      26
#define R_ARM_CALL          28
#define R_ARM_JUMP24        29
#define R_ARM_THM_JUMP24    30
#define R_ARM_TARGET1       38
#define R_ARM_TLS_IE32      107
#define R_ARM_TLS_LE32      108

// Size of thread control block for TLS relocations
#define TCB_SIZE 8

std::expected<void, std::string_view> dsl_apply_relocations(FILE *f, dsl_section_header *section,
                         uint8_t num_sections, uint8_t *loaded_mem,
                         size_t addr_space_size, size_t loaded_mem_size,
                         dsl_symbol_table *sym_table)
{
    uint8_t *veneer_ptr = loaded_mem + ((addr_space_size + 3) & ~3);
    uint8_t *veneer_end = loaded_mem + loaded_mem_size;

    for (unsigned int i = 0; i < num_sections; i++)
    {
        int type = section[i].type;

        if (type != DSL_SEGMENT_RELOCATIONS)
            continue;

        size_t size = section[i].size;
        size_t data_offset = section[i].data_offset;

        if (fseek(f, data_offset, SEEK_SET) != 0)
            return std::unexpected("can't seek relocations");

        size_t num_relocs = size / sizeof(Elf32_Rel);

        for (size_t r = 0; r < num_relocs; r++)
        {
            Elf32_Rel rel;
            if (fread(&rel, sizeof(Elf32_Rel), 1, f) != 1)
                return std::unexpected("can't read relocation");

            int rel_type = rel.r_info & 0xFF;
            int rel_symbol = rel.r_info >> 8;

            if ((rel_type == R_ARM_ABS32) || (rel_type == R_ARM_TARGET1))
            {
                // R_ARM_TARGET1 behaves as R_ARM_ABS32 due to the linker option
                // -Wl,--target1-abs.
                uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                    *ptr = sym->value;
                else
                    *ptr += (uintptr_t)loaded_mem;
            }
            else if (rel_type == R_ARM_THM_CALL)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t bl_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;

                    bool to_arm = false;
                    if ((sym_addr & 1) == 0)
                        to_arm = true;

                    int32_t jump_value = sym_addr - bl_addr;

                    if (to_arm)
                        jump_value -= 2;
                    else
                        jump_value -= 4;

                    if ((jump_value > 0x3FFFFF) || (jump_value <= -0x3FFFFF))
                        return std::unexpected("R_ARM_THM_CALL outside of range");

                    uint16_t *ptr = (uint16_t *)(loaded_mem + rel.r_offset);

                    ptr[0] = 0xF000 | (0x07FF & (jump_value >> 12));

                    if (to_arm)
                        ptr[1] = 0xE800 | (0x07FE & (jump_value >> 1));
                    else
                        ptr[1] = 0xF800 | (0x07FF & (jump_value >> 1));
                }
            }
            else if (rel_type == R_ARM_JUMP24)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t b_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;

                    uint32_t jump_target = sym_addr;

                    if ((sym_addr & 1) == 1)
                    {
                        // AAELF32 specifies that R_ARM_JUMP24 can't interwork
                        // by converting the instruction. It requires a veneer
                        // to transition to Thumb.
                        if (veneer_ptr + 8 > veneer_end)
                            return std::unexpected("R_ARM_JUMP24 no space for veneer");

                        uint32_t *veneer = (uint32_t *)veneer_ptr;

                        // Arm veneer:
                        //   LDR pc, [pc, #-4]
                        //   .word thumb_target
                        veneer[0] = 0xE51FF004;
                        veneer[1] = sym_addr;

                        jump_target = (uint32_t)veneer_ptr;
                        veneer_ptr += 8;
                    }

                    int32_t jump_value = jump_target - b_addr;
                    jump_value -= 6;

                    if ((jump_value > 0x7FFFFF) || (jump_value <= -0x7FFFFF))
                        return std::unexpected("R_ARM_JUMP24 outside of range");

                    uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                    *ptr = (*ptr & 0xFF000000)
                         | ((jump_value >> 2) & 0x00FFFFFF);
                }
            }
            else if (rel_type == R_ARM_CALL)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t bl_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;

                    bool to_arm = false;
                    if ((sym_addr & 1) == 0)
                        to_arm = true;

                    int32_t jump_value = sym_addr - bl_addr;

                    if (to_arm)
                        jump_value -= 6;
                    else
                        jump_value -= 8;

                    if ((jump_value > 0x7FFFFF) || (jump_value <= -0x7FFFFF))
                        return std::unexpected("R_ARM_CALL outside of range");

                    uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                    if (!to_arm)
                    {
                        *ptr = 0xFA000000
                             | ((jump_value >> 2) & 0x00FFFFFF)
                             | ((jump_value & BIT(1)) << 23);
                    }
                    else
                    {
                        *ptr = (*ptr & 0xFF000000)
                             | ((jump_value >> 2) & 0x00FFFFFF);
                    }
                }
            }
            else if (rel_type == R_ARM_TLS_IE32)
            {
                uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                // R_ARM_TLS_IE32: GOT(S) + A - P
                // Initial Exec TLS model. Without a proper GOT implementation,
                // this is simplified to absolute relocation similar to R_ARM_ABS32.
                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                    *ptr = sym->value;
                else
                    *ptr += (uintptr_t)loaded_mem;
            }
            else if (rel_type == R_ARM_TLS_LE32)
            {
                uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                *ptr = sym->value + TCB_SIZE;
            }
            else
            {
                return std::unexpected("unknown relocation");
            }
        }

        break;
    }

    return {};
}
