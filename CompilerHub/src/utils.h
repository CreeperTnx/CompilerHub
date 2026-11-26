#ifndef CH_UTILS_H
#define CH_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CH_VERSION "0.1.0"
#define CH_LOG_INFO(fmt, ...)  fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define CH_LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define CH_LOG_ERR(fmt, ...)   fprintf(stderr, "[ERR ] " fmt "\n", ##__VA_ARGS__)

/* 安全分配 */
void *ch_xmalloc(size_t n);
void *ch_xcalloc(size_t n, size_t sz);
char *ch_xstrdup(const char *s);
char *ch_xstrvcat(size_t cap, const char *sep, int n, ...);

/* 文件/路径 */
int   ch_file_exists(const char *path);
char *ch_read_file(const char *path, size_t *out_len);
int   ch_write_file(const char *path, const void *buf, size_t len);
char *ch_dirname(const char *path);
char *ch_basename(const char *path);
char *ch_replace_ext(const char *path, const char *new_ext);
int   ch_mkdir_p(const char *path);
char *ch_join_path(const char *a, const char *b);
int   ch_exec(const char *cmd, char *out, size_t out_sz);

/* 简单 zip (用外部 zip 命令做 fallback) */
int ch_zip_dir(const char *dir, const char *out_zip);

#endif
