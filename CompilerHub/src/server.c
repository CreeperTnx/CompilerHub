/*
 * CompilerHub 内置 HTTP 服务器 (极简版)
 *
 * 设计原则：
 *   - 只用 POSIX socket / pthread / pipe，零第三方库。
 *   - 前端 HTML 嵌进 .c 里 (CH_UI_HTML)，一个二进制就搞定。
 *   - API 层走子进程 popen，直接复用 ch 自己 —— 不做任何业务复制。
 *   - 支持多用户同时跑编译：每个请求 fork 独立进程。
 */
#include "server.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>

/* 前端 HTML —— 黑/灰调，大字号，按钮灰 #666666 */
static const char CH_UI_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><title>CompilerHub</title>"
"<style>"
" :root { --bg:#111; --panel:#1a1a1a; --fg:#ddd; --muted:#666; --btn:#666666; --accent:#f5d742; --border:#333; }"
" * { box-sizing: border-box; }"
" body { margin:0; background:var(--bg); color:var(--fg); font-family:-apple-system,'PingFang SC','Helvetica Neue',sans-serif; font-size:17px; line-height:1.6; }"
" header { padding:20px 28px; border-bottom:1px solid var(--border); }"
" h1 { font-size:32px; margin:0; color:var(--accent); letter-spacing:2px; }"
" header .sub { color:var(--muted); font-size:14px; margin-top:6px; }"
" main { display:grid; grid-template-columns:1fr 1fr; gap:20px; padding:20px 28px; }"
" .card { background:var(--panel); border:1px solid var(--border); border-radius:8px; padding:18px 20px; }"
" .card h2 { font-size:22px; margin:0 0 14px 0; color:var(--accent); border-bottom:1px dashed var(--border); padding-bottom:8px; }"
" label { display:block; font-size:15px; color:var(--muted); margin:12px 0 4px 0; }"
" input, select, textarea { width:100%; background:#0b0b0b; border:1px solid var(--border); color:var(--fg); font-size:16px; padding:10px 12px; border-radius:4px; font-family:'SF Mono',Menlo,monospace; }"
" textarea { height:260px; resize:vertical; }"
" button { background:var(--btn); color:#eee; border:none; border-radius:6px; padding:12px 20px; font-size:17px; cursor:pointer; margin-top:14px; }"
" button:hover { background:#7a7a7a; }"
" #run-output { background:#000; border:1px solid var(--border); padding:14px; font-family:'SF Mono',Menlo,monospace; font-size:14px; color:#ccc; white-space:pre-wrap; word-break:break-all; max-height:480px; overflow:auto; }"
" .tab { display:flex; gap:2px; margin-bottom:12px; }"
" .tab button { margin:0; border-radius:4px 4px 0 0; background:#2a2a2a; font-size:15px; padding:8px 16px; }"
" .tab button.active { background:var(--btn); }"
" .foot { padding:12px 28px; color:var(--muted); font-size:13px; border-top:1px solid var(--border); margin-top:20px; }"
"</style></head><body>"
"<header><h1>CompilerHub</h1><div class='sub'>v" CH_VERSION " · 多语言编译 → exe / pkg / apk · 二进制 → 伪 C</div></header>"
"<main>"
"<section class='card'>"
"<div class='tab'><button class='active' id='tab-compile'>编译</button><button id='tab-decompile'>反编译</button></div>"
"<div id='panel-compile'>"
"<label>源代码 (支持 .c/.cpp/.py/.js/.go/.rs/.java 等)</label>"
"<textarea id='code' placeholder='#include <stdio.h>\nint main(){printf(\"hi\\n\"); return 0;}\n'></textarea>"
"<div style='display:flex; gap:10px; align-items:center; margin-top:8px;'>"
"<button type='button' id='btn-upload' style='background:#2a2a2a; font-size:15px; padding:8px 14px;'>📂 选择文件</button>"
"<span id='file-name' style='color:#666; font-size:14px;'>未选择文件</span>"
"<input type='file' id='file-input' accept='.c,.cpp,.cc,.cxx,.h,.hpp,.py,.js,.mjs,.go,.rs,.java,.swift,.kt,.txt' style='display:none'>"
"</div>"
"<div style='display:grid; grid-template-columns:2fr 1fr 1fr; gap:10px; margin-top:10px;'>"
"<label style='margin-top:0'>输出文件</label><label style='margin-top:0'>目标</label><label style='margin-top:0'>优化</label>"
"</div>"
"<div style='display:grid; grid-template-columns:2fr 1fr 1fr; gap:10px;'>"
"<input id='out' placeholder='/tmp/out'>"
"<select id='target'><option value='exe' selected>exe</option><option value='pkg'>pkg</option><option value='apk'>apk</option></select>"
"<select id='opt'><option value='0' selected>普通</option><option value='1'>-O2</option></select>"
"</div>"
"<button id='btn-compile'>▶ 编译</button>"
"</div>"
"<div id='panel-decompile' style='display:none'>"
"<label>二进制文件路径</label>"
"<input id='binary' placeholder='/path/to/binary'>"
"<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px;'>"
"<label style='margin-top:0'>架构</label><label style='margin-top:0'>输出</label>"
"</div>"
"<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px;'>"
"<select id='arch'><option value='auto' selected>auto</option><option value='x86_64'>x86_64</option><option value='arm64'>arm64</option></select>"
"<select id='dec-out'><option value='dasm' selected>反汇编</option><option value='pseudo'>伪 C</option></select>"
"</div>"
"<button id='btn-decompile'>▶ 反编译</button>"
"</div>"
"</section>"
"<section class='card'>"
"<h2>输出</h2>"
"<div id='run-output'>等待操作…</div>"
"</section>"
"</main>"
"<footer class='foot'>所有操作在本机执行，不向任何网络发送数据。</footer>"
"<script>"
" const $ = id => document.getElementById(id);"
" const tabs = document.querySelectorAll('.tab button');"
" tabs.forEach((b,i)=>b.onclick=()=>{ tabs.forEach(x=>x.classList.remove('active')); b.classList.add('active'); $('panel-compile').style.display=i?'none':'block'; $('panel-decompile').style.display=i?'block':'none'; });"
" function run(path, body) {"
"   return fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.text());"
" }"
" $('btn-compile').onclick = async () => {"
"   const code = $('code').value;"
"   if(!code.trim()){ $('run-output').textContent='请先输入源代码或上传文件'; return; }"
"   $('run-output').textContent = '编译中…';"
"   const form = new URLSearchParams();"
"   form.set('code', code);"
"   form.set('target', $('target').value);"
"   form.set('optimize', $('opt').value);"
"   form.set('output', $('out').value || '');"
"   $('run-output').textContent = await run('/api/compile', form.toString());"
" };"
" /* 文件上传 → 填入 textarea */"
" $('btn-upload').onclick = () => $('file-input').click();"
" $('file-input').onchange = async (e) => {"
"   const f = e.target.files && e.target.files[0];"
"   if(!f) return;"
"   $('file-name').textContent = f.name + '  (' + Math.round(f.size/1024) + ' KB)';"
"   const text = await f.text();"
"   $('code').value = text;"
"   /* 自动填 .out 扩展名推断输出名 */"
"   if(!$('out').value){"
"     const dot = f.name.lastIndexOf('.');"
"     const base = dot>0 ? f.name.slice(0,dot) : f.name;"
"     $('out').value = '/tmp/' + base + '_out';"
"   }"
" };"
" $('btn-decompile').onclick = async () => {"
"   const b = $('binary').value.trim();"
"   if(!b){ $('run-output').textContent='请输入二进制路径'; return; }"
"   $('run-output').textContent = '反编译中…';"
"   const form = new URLSearchParams();"
"   form.set('binary', b);"
"   form.set('arch', $('arch').value);"
"   form.set('kind', $('dec-out').value);"
"   $('run-output').textContent = await run('/api/decompile', form.toString());"
" };"
"</script></body></html>";

/* 基础 HTTP 工具 */
static char *url_decode(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = ch_xmalloc(n);
    char *o = out;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%') {
            char hex[3] = { s[1], s[2], 0 };
            long v = strtol(hex, NULL, 16);
            *o++ = (char)v; s += 3;
        } else { *o++ = *s++; }
    }
    *o = 0;
    return out;
}

static void parse_query(const char *body, char **params) {
    /* params 是容量 N*2 的 char**，每对 [key,value]，以 NULL 结束 */
    if (!body) { params[0]=params[1]=NULL; return; }
    char *dup = ch_xstrdup(body);
    char *tok = strtok(dup, "&");
    int i = 0;
    while (tok && i < 14) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = 0;
            params[i++] = url_decode(tok);
            params[i++] = url_decode(eq + 1);
        }
        tok = strtok(NULL, "&");
    }
    params[i] = NULL;
    free(dup);
}

static const char *query_get(char **params, const char *key) {
    for (int i = 0; params[i]; i += 2)
        if (strcmp(params[i], key) == 0) return params[i+1];
    return NULL;
}

static void send_response(int fd, int code, const char *mime, const char *body) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, (code==200?"OK":"Bad Request"), mime, strlen(body));
    write(fd, head, n);
    write(fd, body, strlen(body));
}

/* 从 socket 读请求头，直到遇到空行，返回 body 长度 */
static int read_request(int fd, char *req_hdr, size_t hdr_sz, char *body, size_t body_sz) {
    size_t i = 0;
    while (i < hdr_sz - 1) {
        ssize_t r = recv(fd, req_hdr + i, 1, 0);
        if (r <= 0) return -1;
        if (i >= 3 && req_hdr[i-3]=='\r' && req_hdr[i-2]=='\n' &&
                     req_hdr[i-1]=='\r' && req_hdr[i]=='\n') break;
        i++;
    }
    req_hdr[i] = 0;
    /* 查找 Content-Length */
    const char *cl = strstr(req_hdr, "Content-Length:");
    size_t clen = 0;
    if (cl) {
        cl += 15;
        while (*cl == ' ' || *cl == '\t') cl++;
        clen = atoi(cl);
    }
    if (clen > body_sz - 1) clen = body_sz - 1;
    size_t rb = 0;
    while (rb < clen) {
        ssize_t r = recv(fd, body + rb, clen - rb, 0);
        if (r <= 0) break;
        rb += r;
    }
    body[rb] = 0;
    return (int)rb;
}

static void serve_static(int fd, const char *path) {
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(fd, 200, "text/html; charset=utf-8", CH_UI_HTML);
        return;
    }
    send_response(fd, 404, "text/plain", "404 Not Found");
}

static const char *CH_BIN = "/Users/liuxiaobin/coed/Wnhchl/Python虚拟机/CompilerHub/ch";

static void api_compile(int fd, char **params) {
    const char *code   = query_get(params, "code");
    const char *target = query_get(params, "target");
    const char *opt    = query_get(params, "optimize");
    const char *out    = query_get(params, "output");
    char err[256];
    if (!code || !*code) { send_response(fd, 400, "text/plain", "missing code"); return; }
    /* 猜语言 */
    const char *lang_hint = "c";
    if (strstr(code, "def ") || strstr(code, "import ") || strstr(code, "print(")) lang_hint = "py";
    else if (strstr(code, "package main") || strstr(code, "func ")) lang_hint = "go";
    else if (strstr(code, "fn main") || strstr(code, "use std")) lang_hint = "rs";
    else if (strstr(code, "public static void main") || strstr(code, "import java")) lang_hint = "java";
    else if (strstr(code, "console.log") || strstr(code, "function ")) lang_hint = "js";

    char srcpath[512];
    snprintf(srcpath, sizeof srcpath, "/tmp/ch_ui_%d.%s", (int)getpid(), lang_hint);
    if (ch_write_file(srcpath, code, strlen(code)) != 0) {
        snprintf(err, sizeof err, "write tmp failed: %s", strerror(errno));
        send_response(fd, 500, "text/plain", err); return;
    }

    char outpath[512];
    if (out && *out) snprintf(outpath, sizeof outpath, "%s", out);
    else             snprintf(outpath, sizeof outpath, "/tmp/ch_ui_%d_out", (int)getpid());

    char cmd[4096];
    snprintf(cmd, sizeof cmd,
        "%s compile -i '%s' -t %s%s -o '%s' 2>&1; echo; echo EXITCODE=$?",
        CH_BIN, srcpath,
        (target && *target) ? target : "exe",
        (opt && opt[0]=='1') ? " -O" : "",
        outpath);

    char *result = ch_xmalloc(65536);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        size_t n = fread(result, 1, 65535, fp);
        result[n] = 0;
        pclose(fp);
    }
    send_response(fd, 200, "text/plain", result);
    unlink(srcpath);
    free(result);
}

static void api_decompile(int fd, char **params) {
    const char *binary = query_get(params, "binary");
    const char *arch   = query_get(params, "arch");
    const char *kind   = query_get(params, "kind");
    char err[256];
    if (!binary || !*binary) { send_response(fd, 400, "text/plain", "missing binary"); return; }
    char cmd[4096];
    int pos = snprintf(cmd, sizeof cmd, "%s decompile -i '%s'", CH_BIN, binary);
    if (arch && *arch && strcmp(arch,"auto") != 0)
        pos += snprintf(cmd + pos, sizeof cmd - pos, " --arch %s", arch);
    if (kind && strcmp(kind,"pseudo") == 0)
        pos += snprintf(cmd + pos, sizeof cmd - pos, " --pseudo-c");
    snprintf(cmd + pos, sizeof cmd - pos, " 2>&1");
    char *result = ch_xmalloc(65536);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        size_t n = fread(result, 1, 65535, fp);
        result[n] = 0;
        pclose(fp);
    } else {
        snprintf(err, sizeof err, "popen failed: %s", strerror(errno));
        send_response(fd, 500, "text/plain", err); free(result); return;
    }
    send_response(fd, 200, "text/plain", result);
    free(result);
}

/* /download?path=/tmp/xxx — 发文件给浏览器。路径强制限制在允许目录里 */
static void api_download(int fd, const char *full_path) {
    /* full_path 形如: /download?path=%2Ftmp%2Fmain_out  */
    const char *q = strchr(full_path, '?');
    if (!q || strncmp(q+1, "path=", 5) != 0) {
        send_response(fd, 400, "text/plain", "missing path"); return;
    }
    char *rel = url_decode(q + 6);

    /* 安全：拒绝 ..、拒绝绝对路径外的敏感目录 */
    if (strstr(rel, "..") || rel[0] != '/') {
        send_response(fd, 403, "text/plain", "forbidden"); free(rel); return;
    }
    /* 白名单：只允许 /tmp 和 /Users/<me>/CompilerHub 下 */
    const char *allow1 = "/tmp/";
    const char *allow2 = "/Users/liuxiaobin/coed/Wnhchl/Python虚拟机/CompilerHub/";
    if (!(strncmp(rel, allow1, strlen(allow1)) == 0) &&
        !(strncmp(rel, allow2, strlen(allow2)) == 0)) {
        send_response(fd, 403, "text/plain", "only /tmp and CompilerHub dir allowed");
        free(rel); return;
    }
    if (!ch_file_exists(rel)) {
        char err[256]; snprintf(err, sizeof err, "file not found: %s", rel);
        send_response(fd, 404, "text/plain", err); free(rel); return;
    }

    size_t n = 0;
    char *buf = ch_read_file(rel, &n);
    if (!buf) { send_response(fd, 500, "text/plain", "read failed"); free(rel); return; }

    const char *mime = "application/octet-stream";
    const char *dot = strrchr(rel, '.');
    if (dot) {
        if      (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".cpp") || !strcmp(dot, ".py") ||
                 !strcmp(dot, ".js") || !strcmp(dot, ".rs") || !strcmp(dot, ".go") || !strcmp(dot, ".java") ||
                 !strcmp(dot, ".swift") || !strcmp(dot, ".kt") || !strcmp(dot, ".txt"))
            mime = "text/plain; charset=utf-8";
        else if (!strcmp(dot, ".pkg") || !strcmp(dot, ".apk") || !strcmp(dot, ".zip"))
            mime = "application/zip";
    }
    /* 抓文件名 */
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    char head[1024];
    snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        mime, n, base);
    write(fd, head, strlen(head));
    write(fd, buf, n);
    free(buf); free(rel);
}

/* 每个连接一个线程 */
typedef struct { int fd; } conn_t;

static void *handle_conn(void *arg) {
    int fd = ((conn_t*)arg)->fd;
    free(arg);
    char hdr[4096], body[65536];
    if (read_request(fd, hdr, sizeof hdr, body, sizeof body) < 0) { close(fd); return NULL; }

    char method[16], path[256];
    if (sscanf(hdr, "%15s %255s", method, path) != 2) {
        send_response(fd, 400, "text/plain", "bad request"); close(fd); return NULL;
    }
    char *params[16] = {0};
    parse_query(body, params);

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
        serve_static(fd, "/");
    else if (strcmp(path, "/api/compile") == 0 && strcmp(method, "POST") == 0)
        api_compile(fd, params);
    else if (strcmp(path, "/api/decompile") == 0 && strcmp(method, "POST") == 0)
        api_decompile(fd, params);
    else if (strncmp(path, "/download", 9) == 0 && strcmp(method, "GET") == 0)
        api_download(fd, path);
    else
        send_response(fd, 404, "text/plain", "404 Not Found");

    for (int i = 0; params[i]; i++) free(params[i]);
    close(fd);
    return NULL;
}

int ch_serve(const char *port_s) {
    int port = port_s ? atoi(port_s) : 8601;
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { CH_LOG_ERR("socket: %s", strerror(errno)); return 1; }
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr*)&sa, sizeof sa) < 0) {
        CH_LOG_ERR("bind :%d : %s", port, strerror(errno)); close(srv); return 1;
    }
    if (listen(srv, 16) < 0) {
        CH_LOG_ERR("listen: %s", strerror(errno)); close(srv); return 1;
    }
    fprintf(stderr,
        "\n============================================\n"
        "  CompilerHub UI 已启动\n"
        "  浏览器打开: http://localhost:%d/\n"
        "  Ctrl+C 停止\n"
        "============================================\n\n", port);

    for (;;) {
        struct sockaddr_in ca;
        socklen_t clen = sizeof ca;
        int c = accept(srv, (struct sockaddr*)&ca, &clen);
        if (c < 0) { CH_LOG_WARN("accept: %s", strerror(errno)); continue; }
        conn_t *cc = ch_xmalloc(sizeof *cc);
        cc->fd = c;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_conn, cc) != 0) {
            close(c); free(cc); continue;
        }
        pthread_detach(tid);
    }
    close(srv);
    return 0;
}
