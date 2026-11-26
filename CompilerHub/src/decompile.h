#ifndef CH_DECOMPILE_H
#define CH_DECOMPILE_H

#include "utils.h"

typedef enum {
    CH_DASM_AUTO = 0,
    CH_DASM_X86_64,
    CH_DASM_ARM64,
    CH_DASM_ARM32
} ch_dasm_arch_t;

typedef struct {
    const char *input;       /* 二进制文件 */
    const char *output;      /* .c / .dasm 文件，NULL 表示 stdout */
    ch_dasm_arch_t arch;
    int          emit_pseudo_c; /* 1 = 输出伪 C，0 = 纯反汇编 */
} ch_decompile_cfg_t;

int ch_decompile(const ch_decompile_cfg_t *cfg);

#endif
