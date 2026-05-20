
#include <string.h>
#include <windows.h>
#include "patch_utils.h"
#include "log.h"

void hotpatch_call(void* target, void* replacement) {
    uint8_t bytes[] = { 0xE8, 0, 0, 0, 0 };
    *(uint32_t*)&bytes[1] = (uintptr_t)replacement - (uintptr_t)target - 5;
    mem_write(target, bytes);
}

void hotpatch_icall(void* target, void* replacement) {
    uint8_t bytes[] = { 0xE8, 0, 0, 0, 0, 0x90 };
    *(uint32_t*)&bytes[1] = (uintptr_t)replacement - (uintptr_t)target - 5;
    mem_write(target, bytes);
}

void hotpatch_jump(void* target, void* replacement) {
    uint8_t bytes[] = { 0xE9, 0, 0, 0, 0, 0xCC };
    *(uint32_t*)&bytes[1] = (uintptr_t)replacement - (uintptr_t)target - 5;
    mem_write(target, bytes);
}

void hotpatch_ret(void* target, uint16_t pop_bytes) {
    uint8_t bytes[] = { 0xC2, 0, 0, 0xCC };
    *(uint16_t*)&bytes[1] = pop_bytes;
    mem_write(target, bytes);
}

void hotpatch_rel32(void* target, void* replacement) {
    mem_write(target, (uintptr_t)replacement - (uintptr_t)target - 4);
}

void hotpatch_import(void* addr, void* replacement) {
    mem_write(addr, replacement);
}

// Function-entry hooks with trampolines now live in safetyhook (deps/
// safetyhook). The hand-rolled mini-LDE + hotpatch_entry that lived here
// has been removed — safetyhook's Zydis-backed disassembler handles the
// long tail of x86 quirks (rel32 relocation, RIP-relative, etc.) that
// our 150-line LDE wouldn't, and its InlineHook template catches
// calling-convention bugs at compile time.
//
// Keep an unreachable stub below just so any leftover references to the
// removed symbols would surface as a linker error rather than miscompile.

#if 0
static size_t modrm_extra(const uint8_t* mod_byte) {
    uint8_t mod = (*mod_byte >> 6) & 0x03;
    uint8_t rm  =  *mod_byte       & 0x07;
    // mod==11: register direct, no extra
    if (mod == 0b11) return 1;
    // mod==00 && rm==5: direct disp32 (no SIB), 4 bytes
    if (mod == 0b00 && rm == 0b101) return 1 + 4;
    // SIB present when rm==4 and mod != 11
    bool has_sib = (rm == 0b100);
    size_t extra = 1; // modrm byte itself
    if (has_sib) {
        extra += 1; // SIB byte
        // SIB with base==5 and mod==00 means disp32-relative
        if (mod == 0b00) {
            uint8_t sib_base = mod_byte[1] & 0x07;
            if (sib_base == 0b101) extra += 4;
        }
    }
    if (mod == 0b01) extra += 1; // disp8
    if (mod == 0b10) extra += 4; // disp32
    return extra;
}

size_t x86_insn_len(const uint8_t* p) {
    size_t i = 0;
    bool   op16 = false;

    // -- prefixes ----------------------------------------------------------
    while (true) {
        uint8_t b = p[i];
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||  // seg
            b == 0x64 || b == 0x65 || b == 0x67 ||
            b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        if (b == 0x66) { op16 = true; i++; continue; }
        break;
    }

    uint8_t op = p[i++];

    // -- 0F two-byte escape -----------------------------------------------
    if (op == 0x0F) {
        uint8_t op2 = p[i++];
        // 0F 80..8F = JCC rel32 (relative — can't trampoline-relocate)
        if (op2 >= 0x80 && op2 <= 0x8F) {
            // Caller treats relative jumps in stolen bytes as a bail signal.
            return i + 4;
        }
        // 0F 1F (multi-byte NOP), 0F 10-17 (movups etc.), 0F 28-2F (mov/cvt),
        // 0F 40-4F (CMOVcc), 0F B6/B7 (movzx), 0F BE/BF (movsx), 0F AF (imul),
        // most others — all take a modrm.
        return i + modrm_extra(&p[i]);
    }

    // -- short relative jumps (rel8) --------------------------------------
    if ((op >= 0x70 && op <= 0x7F) ||  // JCC short
         op == 0xEB ||                 // JMP short
         op == 0xE3) {                 // JECXZ
        return i + 1;
    }

    // -- near relative jumps (rel32) --------------------------------------
    if (op == 0xE8 || op == 0xE9) return i + 4;

    // -- single-byte no-operand opcodes -----------------------------------
    switch (op) {
        case 0x06: case 0x07: case 0x0E: case 0x16: case 0x17:
        case 0x1E: case 0x1F:                       // legacy seg push/pop
        case 0x27: case 0x2F: case 0x37: case 0x3F: // DAA/DAS/AAA/AAS
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: // INC reg
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: // DEC reg
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: // PUSH reg
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: // POP reg
        case 0x60: case 0x61:                       // PUSHA/POPA
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: // NOP / XCHG eax,reg
        case 0x98: case 0x99: case 0x9B: case 0x9C: case 0x9D:
        case 0x9E: case 0x9F:                       // CWDE/CDQ/WAIT/PUSHF/POPF/SAHF/LAHF
        case 0xC3: case 0xCB: case 0xCC:            // RET / RETF / INT3
        case 0xCE: case 0xCF: case 0xD6: case 0xD7: // INTO / IRET / SALC / XLAT
        case 0xF4: case 0xF5: case 0xF8: case 0xF9:
        case 0xFA: case 0xFB: case 0xFC: case 0xFD: // HLT/CMC/CLC/STC/CLI/STI/CLD/STD
            return i;
    }

    // -- imm8 operand only ------------------------------------------------
    switch (op) {
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C:
        case 0x34: case 0x3C: // ALU al, imm8
        case 0x6A:            // PUSH imm8
        case 0xA8:            // TEST al, imm8
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: // MOV r8, imm8
        case 0xCD:            // INT imm8
        case 0xD4: case 0xD5: // AAM/AAD imm8
        case 0xE0: case 0xE1: case 0xE2: // LOOPNE/LOOPE/LOOP
        case 0xE4: case 0xE5: case 0xE6: case 0xE7: // IN/OUT imm8
            return i + 1;
    }

    // -- imm16 operand only -----------------------------------------------
    if (op == 0xC2 || op == 0xCA) return i + 2;  // RET imm16 / RETF imm16

    // -- imm32 (or imm16 with 0x66 prefix) operand only -------------------
    switch (op) {
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D:
        case 0x35: case 0x3D: // ALU eax, imm32
        case 0x68:            // PUSH imm32
        case 0xA9:            // TEST eax, imm32
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: // MOV r32, imm32
            return i + (op16 ? 2 : 4);
    }

    // -- moffs (no modrm, just disp32) ------------------------------------
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        return i + 4; // MOV al/eax, [disp32] / [disp32], al/eax
    }

    // -- single-byte opcode + modrm ---------------------------------------
    switch (op) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09:
        case 0x0A: case 0x0B: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x20: case 0x21:
        case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39:
        case 0x3A: case 0x3B:                       // ALU + modrm
        case 0x62: case 0x63:                       // BOUND / ARPL
        case 0x69:                                  // IMUL r, r/m, imm32 -> +4 below
        case 0x6B:                                  // IMUL r, r/m, imm8  -> +1 below
        case 0x84: case 0x85: case 0x86: case 0x87: // TEST/XCHG
        case 0x88: case 0x89: case 0x8A: case 0x8B: // MOV
        case 0x8C: case 0x8E:                       // MOV seg
        case 0x8D: case 0x8F:                       // LEA / POP r/m
        case 0xC4: case 0xC5: case 0xC6: case 0xC7: // LES/LDS/MOV imm
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: // shift/rotate
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: // x87
        case 0xFE: case 0xFF: {                     // INC/DEC r/m, CALL/JMP/PUSH r/m
            size_t extra = modrm_extra(&p[i]);
            if (op == 0x69)             extra += op16 ? 2 : 4; // IMUL imm32/imm16
            else if (op == 0x6B)        extra += 1;            // IMUL imm8
            else if (op == 0xC6)        extra += 1;            // MOV r/m8, imm8
            else if (op == 0xC7)        extra += op16 ? 2 : 4; // MOV r/m32, imm
            return i + extra;
        }
    }

    // -- 80/81/82/83 group: ALU r/m, imm ----------------------------------
    if (op == 0x80 || op == 0x82) return i + modrm_extra(&p[i]) + 1;
    if (op == 0x81)               return i + modrm_extra(&p[i]) + (op16 ? 2 : 4);
    if (op == 0x83)               return i + modrm_extra(&p[i]) + 1;

    // -- F6/F7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV ----------------------------
    if (op == 0xF6 || op == 0xF7) {
        // TEST (sub-op 0/1) has imm; others don't
        size_t extra = modrm_extra(&p[i]);
        uint8_t subop = (p[i] >> 3) & 0x07;
        if (subop == 0 || subop == 1) extra += (op == 0xF6) ? 1 : (op16 ? 2 : 4);
        return i + extra;
    }

    // Unknown — caller will fall back.
    log_printf("x86_insn_len: unknown opcode 0x%02X at %p\n", op, p);
    return 0;
}

void* hotpatch_entry(void* func, void* hook) {
    constexpr size_t MIN_STOLEN = 5; // size of our JMP rel32
    constexpr size_t MAX_STOLEN = 15; // sanity cap; ample for any prologue
    static uint8_t* tramp_arena = nullptr;
    static size_t   tramp_used  = 0;
    constexpr size_t TRAMP_ARENA_BYTES = 4096;

    // -- find safe split point in func's prologue -------------------------
    const uint8_t* p = (const uint8_t*)func;
    size_t stolen = 0;
    while (stolen < MIN_STOLEN) {
        size_t n = x86_insn_len(p + stolen);
        if (n == 0 || stolen + n > MAX_STOLEN) {
            log_printf("hotpatch_entry: can't split prologue at %p (have %zu bytes)\n",
                       func, stolen);
            return nullptr;
        }
        // Don't steal a relative-jump instruction — its rel32 would point
        // to the wrong target after relocation.
        uint8_t op = p[stolen];
        if (op == 0xE8 || op == 0xE9 || op == 0xEB ||
            (op >= 0x70 && op <= 0x7F) ||
            (op == 0x0F && (p[stolen + 1] & 0xF0) == 0x80)) {
            log_printf("hotpatch_entry: prologue at %p contains relative jump "
                       "(op=%02X at +%zu); manual hook required\n",
                       func, op, stolen);
            log_printf("  first 16 bytes:");
            for (size_t k = 0; k < 16; ++k) log_printf(" %02X", p[k]);
            log_printf("\n");
            return nullptr;
        }
        stolen += n;
    }

    // -- allocate trampoline page on first use ----------------------------
    if (!tramp_arena) {
        tramp_arena = (uint8_t*)VirtualAlloc(
            nullptr, TRAMP_ARENA_BYTES,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp_arena) {
            log_printf("hotpatch_entry: trampoline VirtualAlloc failed: %lu\n",
                       GetLastError());
            return nullptr;
        }
    }
    size_t tramp_size = stolen + 5; // stolen bytes + JMP rel32 back
    if (tramp_used + tramp_size > TRAMP_ARENA_BYTES) {
        log_printf("hotpatch_entry: trampoline arena exhausted (%zu/%zu)\n",
                   tramp_used, TRAMP_ARENA_BYTES);
        return nullptr;
    }
    uint8_t* tramp = tramp_arena + tramp_used;
    tramp_used += tramp_size;

    // -- emit trampoline: original prologue bytes + JMP back --------------
    memcpy(tramp, func, stolen);
    tramp[stolen] = 0xE9;
    int32_t back = (int32_t)((uintptr_t)func + stolen)
                 - (int32_t)((uintptr_t)tramp + stolen + 5);
    memcpy(tramp + stolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, tramp_size);

    // -- splice JMP-to-hook into the function entry -----------------------
    // Write the JMP rel32 *only* (5 bytes). Don't use hotpatch_jump here
    // because it writes 6 bytes (JMP + trailing 0xCC INT3 for padding),
    // and that 0xCC would land on `func + 5` — exactly where the
    // trampoline jumps back to after running the stolen prologue. We
    // need `func + 5` (or whatever `stolen` is) to keep its original
    // first-byte-of-next-instruction so execution continues cleanly.
    uint8_t jmp_bytes[5] = { 0xE9, 0, 0, 0, 0 };
    int32_t rel = (int32_t)((uintptr_t)hook)
                - (int32_t)((uintptr_t)func + 5);
    memcpy(&jmp_bytes[1], &rel, 4);
    mem_write(func, jmp_bytes);
    FlushInstructionCache(GetCurrentProcess(), func, 5);

    return tramp;
}
#endif // 0 (mini-LDE retired in favor of safetyhook)