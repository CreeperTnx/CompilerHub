#include "utils.h"
#include <unistd.h>
#include <limits.h>
#include <ctype.h>

void *ch_xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { CH_LOG_ERR("out of memory (req %zu)", n); exit(1); }
    return p;
}
void *ch_xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) { CH_LOG_ERR("out of memory"); exit(1); }
    return p;
}
char *ch_xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = ch_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *ch_xstrvcat(size_t cap, const char *sep, int n, ...) {
    char *buf = ch_xmalloc(cap);
    buf[0] = '\0';
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        const char *s = va_arg(ap, const char *);
        if (i > 0 && sep) strncat(buf, sep, cap - strlen(buf) - 1);
        strncat(buf, s, cap - strlen(buf) - 1);
    }
    va_end(ap);
    return buf;
}

int ch_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

char *ch_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = ch_xmalloc(n + 1);
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

int ch_write_file(const char *path, const void *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : -1;
}

static char *_dup_sub(const char *s, size_t a, size_t b) {
    char *r = ch_xmalloc(b - a + 1);
    memcpy(r, s + a, b - a);
    r[b - a] = '\0';
    return r;
}

char *ch_dirname(const char *path) {
    const char *sl = strrchr(path, '/');
    if (!sl) return ch_xstrdup(".");
    if (sl == path) return ch_xstrdup("/");
    return _dup_sub(path, 0, sl - path);
}
char *ch_basename(const char *path) {
    const char *sl = strrchr(path, '/');
    return sl ? ch_xstrdup(sl + 1) : ch_xstrdup(path);
}
char *ch_replace_ext(const char *path, const char *new_ext) {
    size_t n = strlen(path);
    size_t ext_len = strlen(new_ext);
    const char *dot = strrchr(path, '.');
    const char *sl  = strrchr(path, '/');
    if (dot && (!sl || dot > sl))
        n = (size_t)(dot - path);
    char *r = ch_xmalloc(n + 1 + ext_len + 1);
    memcpy(r, path, n);
    r[n] = '\0';
    strcat(r, ".");
    strcat(r, new_ext);
    return r;
}

int ch_mkdir_p(const char *path) {
    char *dup = ch_xstrdup(path);
    for (char *p = dup + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dup, 0755) != 0 && errno != EEXIST) { free(dup); return -1; }
            *p = '/';
        }
    }
    if (mkdir(dup, 0755) != 0 && errno != EEXIST) { free(dup); return -1; }
    free(dup);
    return 0;
}

char *ch_join_path(const char *a, const char *b) {
    if (!a || !*a) return ch_xstrdup(b);
    if (!b || !*b) return ch_xstrdup(a);
    size_t na = strlen(a);
    int slash_a = (a[na-1] == '/');
    int slash_b = (b[0]   == '/');
    char *r;
    if (slash_a && slash_b) {
        r = ch_xmalloc(na + strlen(b));
        memcpy(r, a, na);
        strcpy(r + na, b + 1);
    } else if (slash_a || slash_b) {
        r = ch_xmalloc(na + strlen(b) + 1);
        sprintf(r, "%s%s", a, b);
    } else {
        r = ch_xmalloc(na + strlen(b) + 2);
        sprintf(r, "%s/%s", a, b);
    }
    return r;
}

int ch_exec(const char *cmd, char *out, size_t out_sz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (out) {
        size_t n = fread(out, 1, out_sz - 1, fp);
        out[n] = '\0';
    }
    int st = pclose(fp);
    return WEXITSTATUS(st);
}

int ch_zip_dir(const char *dir, const char *out_zip) {
    /* mac 自带 /usr/bin/zip 如果输出文件没有扩展名，会偷偷追加 .zip ——
       我们要的是准确的路径，所以强制加上扩展名（只加扩展名，绝不改用户给的名字）。 */
    char safe_out[4096];
    snprintf(safe_out, sizeof safe_out, "%s", out_zip);
    const char *dot = strrchr(safe_out, '.');
    if (!dot || strchr(dot, '/')) {
        /* 无扩展名 —— 根据输出路径名猜：apk/pkg/bin 任一关键字？默认加 .zip 让 zip 命令不再动 */
        if (strstr(safe_out, ".apk") || strstr(safe_out, "apk"))      snprintf(safe_out, sizeof safe_out, "%s.apk", out_zip);
        else if (strstr(safe_out, ".pkg") || strstr(safe_out, "pkg")) snprintf(safe_out, sizeof safe_out, "%s.pkg", out_zip);
        else                                                          snprintf(safe_out, sizeof safe_out, "%s.zip", out_zip);
    }
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "command -v zip >/dev/null 2>&1 && { cd '%s' && zip -r '%s' . >/dev/null 2>&1; }",
             dir, safe_out);
    int rc = system(cmd);
    if (rc != 0) {
        CH_LOG_WARN("zip not available; pkg/apk output will be a plain tar.gz stub instead");
        snprintf(cmd, sizeof cmd,
                 "command -v tar >/dev/null 2>&1 && tar -czf '%s' -C '%s' . 2>/dev/null",
                 safe_out, dir);
        rc = system(cmd);
    }
    if (rc != 0) { CH_LOG_ERR("failed to create archive at %s", safe_out); return -1; }
    /* 清理：如果原来用户写的 out 和最终 safe_out 不同（zip 悄悄换名了），删掉那份 */
    if (strcmp(out_zip, safe_out) != 0) unlink(out_zip);
    return 0;
}
