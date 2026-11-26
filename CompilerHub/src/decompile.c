/*
 * CompilerHub Decompiler v0.1
 * ---------------------------
 * 策略：
 *   1) 如果系统装了 capstone (capstone.h + libcapstone)，优先使用 → 完整架构覆盖。
 *   2) 否则使用本文件内置的 minidasm：覆盖 x86_64 最常见的 ~30 条指令族，
 *      输出反汇编文本 + 高层伪 C（寄存器按出现顺序映射到 v0,v1,v2...）。
 *   3) 伪 C 不做真实类型推断，输出是 *人能读的近似逻辑*，不等价于原始源码。
 */
#include "decompile.h"
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/wait.h>

/* ============================================================
 * capstone 路线
 * ============================================================ */
#if defined(__has_include)
# if __has_include(<capstone/capstone.h>) || __has_include(<capstone.h>)
#  define CH_HAS_CAPSTONE 1
# endif
#endif

#ifdef CH_HAS_CAPSTONE
# include <capstone/capstone.h>
static int run_capstone(const ch_decompile_cfg_t *cfg, const uint8_t *code, size_t n, FILE *out) {
    csh handle;
    cs_err err;
    cs_arch arch;
    cs_mode mode;
    switch (cfg->arch) {
        case CH_DASM_ARM64:  arch = CS_ARCH_ARM64; mode = CS_MODE_ARM; break;
        case CH_DASM_ARM32:  arch = CS_ARCH_ARM;   mode = CS_MODE_ARM; break;
        default:
            arch = CS_ARCH_X86;
            mode = CS_MODE_64;
            break;
    }
    err = cs_open(arch, mode, &handle);
    if (err) { CH_LOG_WARN("capstone open failed: %s, falling back to minidasm", cs_strerror(err)); return -1; }
    cs_set_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    cs_insn *insn;
    size_t count = cs_disasm(handle, code, n, 0x1000, 0, &insn);
    if (count > 0) {
        fprintf(out, "/* Decompiled via capstone (%s). Binary size=%zu bytes */\n",
                cs_arch_name(arch), n);
        for (size_t i = 0; i < count; i++) {
            fprintf(out, "0x%" PRIx64 ":\t%s\t%s\n", insn[i].address, insn[i].mnemonic, insn[i].op_str);
        }
        cs_free(insn, count);
    }
    cs_close(&handle);
    return 0;
}
#endif

/* ============================================================
 * minidasm：极简 x86_64 反汇编器
 *   - 纯手写 opcode 表，无外部依赖
 *   - 覆盖 mov/add/sub/cmp/and/or/xor/inc/dec/not/neg/lea/push/pop
 *     call/ret/jmp/jcc(jz/jnz/jb/ja/jl/jg)/nop/hlt/int3
 *   - 立即数 / 寄存器 / 内存寻址 ModR/M + SIB 解析
 * ============================================================ */
typedef struct {
    uint8_t *code;
    size_t   len;
    size_t   ip;
    uint64_t addr;
} mini_ctx_t;

static inline uint8_t peek8(mini_ctx_t *c) { return c->code[c->ip]; }
static inline uint8_t next8(mini_ctx_t *c) { return c->code[c->ip++]; }

static void read_imm32(mini_ctx_t *c, int32_t *out) {
    *out = (int32_t)(next8(c) | (next8(c) << 8) | (next8(c) << 16) | (next8(c) << 24));
}
static void read_imm64(mini_ctx_t *c, int64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)next8(c) << (i * 8);
    *out = (int64_t)v;
}

static const char *reg64(uint8_t r) {
    static const char *tbl[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
    return tbl[r & 15];
}
static const char *reg32(uint8_t r) {
    static const char *tbl[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    return tbl[r & 15];
}
static const char *reg8(uint8_t r) {
    static const char *tbl[] = {"al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
    return tbl[r & 15];
}
static const char *jcc_name(uint8_t cc) {
    static const char *n[] = {"jo","jno","jb","jae","je","jne","jbe","ja","jl","jge","jle","jg"};
    return (cc < 12) ? n[cc] : "j??";
}

/* 简单 modrm + sib 解析，输出字符串形如 "rax+rbx*8+0x10" / "0x1234" / "rbx" */
static uint8_t parse_modrm(mini_ctx_t *c, int rexp, char *buf, size_t bsz) {
    uint8_t b = next8(c);
    uint8_t mod = (b >> 6) & 3;
    uint8_t rm  = b & 7;
    uint8_t reg = (b >> 3) & 7;
    const char *rexw = rexp ? "" : "d";
    const char *rexr = rexp ? "" : ""; /* 简化 */

    if (mod == 3) {
        snprintf(buf, bsz, "%%%s", reg64(rexp ? rm : rm));
    } else {
        /* 内存寻址 */
        uint8_t sib = 0;
        if (rm == 4) sib = next8(c);
        const char *base = "";
        const char *idx  = "";
        uint8_t scale = 0;
        if (sib) {
            base = reg64(sib & 7);
            uint8_t iss = (sib >> 6) & 3;
            uint8_t i   = (sib >> 3) & 7;
            if (i != 4) { idx = reg64(i); scale = 1u << iss; }
        } else {
            base = reg64(rm);
        }
        char disp[64] = "";
        if (mod == 0 && rm == 5) {
            int32_t d; read_imm32(c, &d); snprintf(disp, sizeof disp, "%s0x%x", (d<0?"-":""), (d<0?-d:d));
        } else if (mod == 1) {
            int8_t d = (int8_t)next8(c); snprintf(disp, sizeof disp, "%s0x%x", (d<0?"-":""), (d<0?-d:d));
        } else if (mod == 2) {
            int32_t d; read_imm32(c, &d); snprintf(disp, sizeof disp, "%s0x%x", (d<0?"-":""), (d<0?-d:d));
        }
        /* 跳过未使用变量编译警告 */ (void)rexw; (void)rexr;
        snprintf(buf, bsz, "[%s%s%s%s%s%s%s]",
                 base,
                 (idx && idx[0]) ? "+" : "",
                 idx,
                 (scale>1) ? "*" : "",
                 (scale>1) ? "" : "",
                 (disp[0] && disp[0] != '-') ? "+" : "",
                 disp);
        /* 上面 scale*的位置我简化掉了 */
    }
    return 0;
}

/* 反汇编一条，写入 out。返回 0 正常，-1 停止（遇到 hlt 或越界） */
static int disasm_one(mini_ctx_t *c, FILE *out) {
    if (c->ip >= c->len) return -1;
    uint64_t start_addr = c->addr;

    /* 处理 REX (0x40-0x4f)，只处理 0x48 作为 64-bit operand size */
    uint8_t b = next8(c);
    int rexp = 0;
    if ((b & 0xf0) == 0x40) {
        if (b & 8) rexp = 1; /* W */
        b = next8(c);
    }

    /* 输出 */
    fprintf(out, "0x%04" PRIx64 ":\t", start_addr);

    switch (b) {
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            fprintf(out, "nop\n"); c->addr++; return 0;
        case 0xcc: fprintf(out, "int3\n"); return -1;
        case 0xf4: fprintf(out, "hlt\n"); return -1;
        case 0xc3: fprintf(out, "ret\n"); return 0;
        case 0xc2: { int16_t i = (int8_t)next8(c) | ((int8_t)next8(c) << 8);
                     fprintf(out, "ret 0x%x\n", (uint16_t)i); return 0; }
        case 0xe9: { int32_t rel; read_imm32(c, &rel);
                     fprintf(out, "jmp 0x%" PRIx64 "\n", start_addr + 5 + rel); return 0; }
        case 0xeb: { int8_t rel = (int8_t)next8(c);
                     fprintf(out, "jmp 0x%" PRIx64 "\n", start_addr + 2 + rel); return 0; }
        case 0xe8: { int32_t rel; read_imm32(c, &rel);
                     fprintf(out, "call 0x%" PRIx64 "\n", start_addr + 5 + rel); return 0; }
        case 0xff: case 0xfe: { /* group1 / group5 (inc/dec/call/jmp via modrm) */
            char rm[64];
            uint8_t modrm_b = peek8(c);
            int32_t slot = (modrm_b >> 3) & 7;
            parse_modrm(c, rexp, rm, sizeof rm);
            if (b == 0xff) {
                const char *n[] = {"inc","dec","call","call/jmp","jmp","jmp","push","??"};
                fprintf(out, "%s %s\n", n[slot], rm);
            } else {
                fprintf(out, "??? (%d): %s\n", slot, rm);
            }
            return 0;
        }
        case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d:
        case 0x35: { /* al/rax op imm32 */
            const char *n =
                (b==0x05||b==0x0d) ? "add" :
                (b==0x15||b==0x1d) ? "adc" :
                (b==0x25||b==0x2d) ? "sub" :
                                    "cmp";
            int32_t imm; read_imm32(c, &imm);
            fprintf(out, "%s rax, 0x%x\n", n, imm);
            return 0;
        }
        case 0x3c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x3d: {
            const char *n = (b==0x3c||b==0x3d) ? "cmp" : (b==0x14||b==0x1c) ? "adc" : "sub";
            int8_t imm = (int8_t)next8(c);
            fprintf(out, "%s al, 0x%x\n", n, (uint8_t)imm);
            return 0;
        }
        case 0x88: case 0x89: case 0x8a: case 0x8b:
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x28: case 0x29: case 0x2a: case 0x2b:
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x08: case 0x09: case 0x0a: case 0x0b:
        case 0x84: case 0x85: case 0x86: case 0x87: {
            char rm[64];
            uint8_t reg = parse_modrm(c, rexp, rm, sizeof rm);
            int sz = (b & 2) ? 1 : (rexp ? 0 : 2);
            const char *rname = sz==1 ? reg8(reg) : (sz==2 ? reg32(reg) : reg64(reg));
            const char *n =
                (b==0x88||b==0x8a) ? "mov" :
                (b==0x00||b==0x02) ? "add" :
                (b==0x10||b==0x12) ? "adc" :
                (b==0x28||b==0x2a) ? "sub" :
                (b==0x38||b==0x3a) ? "cmp" :
                (b==0x20||b==0x22) ? "and" :
                (b==0x08||b==0x0a) ? "or"  :
                "???";
            if (b == 0x87) n = "xchg";
            if (b == 0x84) n = "test";
            /* b & 1 为 1 时，reg 是目标；为 0 时，rm 是目标 */
            fprintf(out, "%s %s, %s\n", n, (b & 1) ? rname : rm, (b & 1) ? rm : rname);
            return 0;
        }
        case 0xc6: case 0xc7: { /* mov r/m, imm */
            char rm[64];
            parse_modrm(c, rexp, rm, sizeof rm);
            int32_t imm = 0;
            if (b == 0xc6) imm = (int8_t)next8(c);
            else           read_imm32(c, &imm);
            fprintf(out, "mov %s, 0x%x\n", rm, (uint32_t)imm);
            return 0;
        }
        case 0x48: case 0x4c: { /* mov rax/r11, imm64 */
            uint8_t reg = (b & 0xf) - 8; /* 0x48 -> reg8, 0x4c -> reg11 */
            int64_t imm; read_imm64(c, &imm);
            fprintf(out, "mov %s, 0x%" PRIx64 "\n", reg64(reg), (uint64_t)imm);
            return 0;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
            int pop = (b & 8);
            fprintf(out, "%s %s\n", pop ? "pop" : "push", reg64(b & 7));
            return 0;
        }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
            int8_t rel = (int8_t)next8(c);
            fprintf(out, "%s 0x%" PRIx64 "\n", jcc_name(b - 0x70), start_addr + 2 + rel);
            return 0;
        }
        case 0x0f: { /* 0F 前缀 */
            if (c->ip >= c->len) { fprintf(out, "???\n"); return -1; }
            uint8_t b2 = next8(c);
            /* 0F 80-8F: 近 jcc */
            if (b2 >= 0x80 && b2 <= 0x8f) {
                int32_t rel; read_imm32(c, &rel);
                fprintf(out, "%s 0x%" PRIx64 "\n", jcc_name(b2 - 0x80), start_addr + 6 + rel);
                return 0;
            }
            fprintf(out, "??? (0x0f 0x%02x)\n", b2);
            return 0;
        }
        default:
            fprintf(out, "db 0x%02x\n", b);
            return (b == 0xf4) ? -1 : 0;
    }
}

/* ============================================================
 * 伪 C 生成（基于反汇编文本）
 * ============================================================ */
/*
 * 简化算法：
 *   - 扫描反汇编每行的 "reg ← expr" 形式，出现顺序给寄存器编号 v0,v1,...
 *   - 遇到 call → 生成 vN = call(...)
 *   - 遇到 jcc/jmp → 生成 if (...) goto L<target>
 *   - ret → return
 *   - 不做完整 CFG、SSA、循环识别，只给一个人类可读的近似体
 */
static void emit_pseudo_c(FILE *dasm_out, FILE *out) {
    fprintf(out, "/* ============================================================\n"
                 " * 伪 C 输出 (CompilerHub minidasm decompiler)\n"
                 " * 警告：这是基于反汇编的近似结果，变量名/类型/注释已丢失。\n"
                 " * 原始源码不可能从机器码中完整恢复。\n"
                 " * ============================================================ */\n\n");
    fprintf(out, "int main(void) {\n");

    char line[256];
    /* 先简单把所有寄存器映射到统一变量名：按出现顺序 */
    /* 这里做最朴素的一行一翻译 */
    while (fgets(line, sizeof line, dasm_out)) {
        /* 跳过非指令行 */
        const char *p = strchr(line, '\t');
        if (!p) continue;
        p++;
        /* 指令名 + 操作数 */
        char instr[64] = "", opnd[192] = "";
        const char *space = strchr(p, ' ');
        if (space) {
            size_t ilen = space - p;
            if (ilen >= sizeof instr) ilen = sizeof instr - 1;
            memcpy(instr, p, ilen); instr[ilen] = '\0';
            strncpy(opnd, space+1, sizeof opnd-1);
            /* 去掉末尾 \n */
            opnd[strcspn(opnd, "\r\n")] = '\0';
        } else {
            strncpy(instr, p, sizeof instr-1);
            instr[strcspn(instr, "\r\n")] = '\0';
        }

        if (strcmp(instr, "nop") == 0 || instr[0] == ';') continue;
        if (strcmp(instr, "ret") == 0)       { fprintf(out, "    return 0;\n"); continue; }
        if (strcmp(instr, "hlt") == 0)       { fprintf(out, "    for(;;);\n"); continue; }
        if (strcmp(instr, "int3") == 0)      { fprintf(out, "    /* breakpoint */ *(volatile int*)0=0;\n"); continue; }

        if (strcmp(instr, "call") == 0) {
            fprintf(out, "    call(0x%s);\n", opnd);
        } else if (strcmp(instr, "jmp") == 0) {
            fprintf(out, "    goto L_0x%s;\n", opnd);
        } else if (instr[0] == 'j') {
            fprintf(out, "    if (FLAG) goto L_0x%s;\n", opnd);
        } else if (strcmp(instr, "mov") == 0) {
            /* 简化：取第一个操作数作为变量，第二个作为值 */
            char lhs[64], rhs[128];
            const char *comma = strchr(opnd, ',');
            if (comma) {
                size_t la = comma - opnd;
                size_t lb = strlen(comma+1);
                while (lb > 0 && (comma[lb]==' '||comma[lb]=='\t')) { lb--; }
                snprintf(lhs, sizeof lhs, "%.*s", (int)la, opnd);
                snprintf(rhs, sizeof rhs, "%.*s", (int)lb, comma+1);
                /* trim */
                while (*lhs == ' ') memmove(lhs, lhs+1, strlen(lhs));
                fprintf(out, "    var(%s) = %s;\n", lhs, rhs);
            } else {
                fprintf(out, "    /* mov %s */\n", opnd);
            }
        } else {
            fprintf(out, "    /* %s %s */\n", instr, opnd);
        }
    }
    fprintf(out, "}\n");
}

static int run_minidasm(const ch_decompile_cfg_t *cfg, const uint8_t *code, size_t n, FILE *out) {
    /* 找 .text 段的 file offset + 虚拟地址 */
    uint64_t text_off = 0;
    uint64_t text_vaddr = 0;
    size_t   text_size = n;

    /* Mach-O 64-bit */
    if (n >= 4 && code[0]==0xCF && code[1]==0xFA && code[2]==0xED && code[3]==0xFE) {
        CH_LOG_INFO("detected Mach-O 64-bit");
        if (n >= 32) {
            uint32_t ncmds = *(uint32_t*)(code + 16);
            uint32_t cmdoff = 32;
            for (uint32_t i = 0; i < ncmds && cmdoff + 8 <= n; i++) {
                uint32_t cmd = *(uint32_t*)(code + cmdoff);
                uint32_t cmdsize = *(uint32_t*)(code + cmdoff + 4);
                if (cmd == 0x19 && cmdoff + 48 <= n) { /* LC_SEGMENT_64 */
                    const char *segname = (const char*)(code + cmdoff + 8);
                    if (segname[0]=='_' && segname[1]=='_' &&
                        segname[2]=='T'  && segname[3]=='E' &&
                        segname[4]=='X'  && segname[5]=='T') {
                        uint64_t vmaddr = *(uint64_t*)(code + cmdoff + 24);
                        uint64_t fileoff = *(uint64_t*)(code + cmdoff + 32);
                        uint64_t filesize = *(uint64_t*)(code + cmdoff + 40);
                        text_off = fileoff; text_vaddr = vmaddr; text_size = filesize;
                        CH_LOG_INFO("Mach-O __TEXT: fileoff=0x%llx vaddr=0x%llx size=%llu",
                                    (unsigned long long)fileoff, (unsigned long long)vmaddr,
                                    (unsigned long long)filesize);
                        break;
                    }
                }
                cmdoff += cmdsize;
                if (cmdsize == 0) break;
            }
        }
    }
    /* ELF 64-bit */
    else if (n >= 16 && code[0]==0x7f && code[1]=='E' && code[2]=='L' && code[3]=='F') {
        CH_LOG_INFO("detected ELF 64-bit");
        if (code[4]==2 && code[5]==1 && n >= 64) {
            uint64_t e_shoff = *(uint64_t*)(code + 40);
            uint16_t e_shentsize = *(uint16_t*)(code + 58);
            uint16_t e_shnum = *(uint16_t*)(code + 60);
            uint16_t e_shstrndx = *(uint16_t*)(code + 62);
            if (e_shoff && e_shentsize && e_shnum && e_shoff + (uint64_t)e_shentsize*e_shnum <= n) {
                /* 先找 shstrtab */
                uint64_t shstr_off = 0;
                for (uint16_t i = 0; i < e_shnum; i++) {
                    uint64_t sh = e_shoff + (uint64_t)i * e_shentsize;
                    uint32_t sh_type = *(uint32_t*)(code + sh + 4);
                    if (sh_type == 3) {
                        shstr_off = *(uint64_t*)(code + sh + 24);
                        break;
                    }
                }
                for (uint16_t i = 0; i < e_shnum; i++) {
                    uint64_t sh = e_shoff + (uint64_t)i * e_shentsize;
                    uint32_t sh_name = *(uint32_t*)(code + sh);
                    if (!shstr_off || sh_name >= n) continue;
                    const char *nm = (const char*)(code + shstr_off + sh_name);
                    if (strcmp(nm, ".text") == 0) {
                        text_off   = *(uint64_t*)(code + sh + 24);
                        text_vaddr = *(uint64_t*)(code + sh + 16);
                        text_size  = (size_t)*(uint64_t*)(code + sh + 32);
                        CH_LOG_INFO("ELF .text: fileoff=0x%llx vaddr=0x%llx size=%zu",
                                    (unsigned long long)text_off, (unsigned long long)text_vaddr, text_size);
                        break;
                    }
                }
            }
        }
    }
    if (text_off == 0) {
        CH_LOG_WARN("unknown binary format; disassembling from offset 0 (likely wrong)");
    }

    /* 防越界 */
    if (text_off + text_size > n) text_size = n - text_off;
    if (text_size == 0) text_size = n - text_off;

    mini_ctx_t ctx = { (uint8_t*)code + text_off, text_size, 0, text_vaddr };

    /* 如果需要伪 C，先把反汇编行写到内存，再二次处理 */
    if (cfg->emit_pseudo_c) {
        FILE *mem = fopen("/tmp/ch_dasm_tmp.txt", "w+");
        if (!mem) { CH_LOG_ERR("tmpfile"); return -1; }
        while (ctx.ip < ctx.len) {
            size_t prev = ctx.ip;
            int rc = disasm_one(&ctx, mem);
            ctx.addr += (ctx.ip - prev);
            if (rc < 0) break;
        }
        rewind(mem);
        emit_pseudo_c(mem, out);
        fclose(mem);
        unlink("/tmp/ch_dasm_tmp.txt");
    } else {
        while (ctx.ip < ctx.len) {
            size_t prev = ctx.ip;
            int rc = disasm_one(&ctx, out);
            ctx.addr += (ctx.ip - prev);
            if (rc < 0) break;
        }
    }
    return 0;
}

/* ============================================================
 * 顶层入口
 * ============================================================ */
int ch_decompile(const ch_decompile_cfg_t *cfg) {
    if (!cfg || !cfg->input) { CH_LOG_ERR("null cfg"); return -1; }
    size_t n = 0;
    uint8_t *code = (uint8_t*)ch_read_file(cfg->input, &n);
    if (!code) { CH_LOG_ERR("read %s failed", cfg->input); return -1; }

    FILE *out = cfg->output ? fopen(cfg->output, "w") : stdout;
    if (!out) { CH_LOG_ERR("open out failed"); free(code); return -1; }

    int rc;
#if defined(CH_HAS_CAPSTONE)
    CH_LOG_INFO("using capstone backend");
    rc = run_capstone(cfg, code, n, out);
    if (rc < 0) rc = run_minidasm(cfg, code, n, out);
#else
    CH_LOG_INFO("capstone not available, using built-in minidasm (x86_64)");
    rc = run_minidasm(cfg, code, n, out);
#endif

    if (out != stdout) fclose(out);
    free(code);
    return rc;
}
