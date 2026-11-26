#ifndef CH_COMPILER_H
#define CH_COMPILER_H

#include "utils.h"

typedef enum {
    LANG_UNKNOWN = 0,
    LANG_C, LANG_CPP, LANG_PYTHON, LANG_JS,
    LANG_GO, LANG_RUST, LANG_JAVA,
    LANG_SWIFT, LANG_KOTLIN
} ch_lang_t;

typedef enum {
    BIN_EXE,   /* 原生可执行 (host 平台) */
    BIN_PKG,   /* 自包含 pkg (zip)：内含解释器 + 源码 */
    BIN_APK    /* Android APK 骨架 (zip 对齐) */
} ch_target_t;

typedef struct {
    const char *input;
    const char *output;     /* 可为 NULL，自动推断 */
    ch_target_t target;
    int         optimize;   /* -O2 等 */
    int         verbose;
} ch_compile_cfg_t;

ch_lang_t ch_detect_lang(const char *path);
const char *ch_lang_name(ch_lang_t l);
const char *ch_target_ext(ch_target_t t);

/* 返回 0 成功，非 0 失败 */
int ch_compile(const ch_compile_cfg_t *cfg);

#endif
