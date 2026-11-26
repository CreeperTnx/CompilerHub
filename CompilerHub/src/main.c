/*
 * CompilerHub CLI - 把各种编程语言编译成 exe/pkg/apk，并能反编译回伪 C
 *
 * 用法:
 *   ch compile  -i foo.c        -t exe   -o foo
 *   ch compile  -i bar.py       -t pkg   -o bar.pkg
 *   ch compile  -i Main.java    -t apk   -o app.apk
 *   ch decompile -i foo -o foo.c      --pseudo-c
 *   ch decompile -i foo               (纯反汇编打印到 stdout)
 *   ch detect  -i baz.go                (只猜语言)
 *   ch version
 */
#include "utils.h"
#include "compiler.h"
#include "packager.h"
#include "decompile.h"
#include "server.h"
#include <unistd.h>
#include <getopt.h>

static void usage(void) {
    fprintf(stdout,
        "CompilerHub v%s — compile anything, decompile back to pseudo-C\n"
        "\n"
        "USAGE:\n"
        "  ch <command> [options]\n"
        "\n"
        "COMMANDS:\n"
        "  compile     把源码编译成 exe / pkg / apk\n"
        "  decompile   把二进制反汇编 / 生成伪 C\n"
        "  serve       启动 Web UI (默认 :8601)\n"
        "  detect      只识别输入文件语言类型\n"
        "  version     打印版本\n"
        "\n"
        "compile 选项:\n"
        "  -i, --input <file>     输入源码\n"
        "  -o, --output <file>    输出文件 (默认: 改扩展名)\n"
        "  -t, --target <t>       exe | pkg | apk  (默认 exe)\n"
        "  -O, --optimize         编译时开启优化\n"
        "\n"
        "decompile 选项:\n"
        "  -i, --input <binary>\n"
        "  -o, --output <file>    输出 (默认 stdout)\n"
        "      --arch auto|x86_64|arm64|arm32\n"
        "      --pseudo-c         生成伪 C (默认纯反汇编)\n"
        "\n"
        "EXIT CODES: 0=OK  1=ERROR\n",
        CH_VERSION);
}

static int cmd_compile(int argc, char **argv) {
    ch_compile_cfg_t cfg = {0};
    cfg.target = BIN_EXE;
    static struct option longopts[] = {
        {"input",   required_argument, 0, 'i'},
        {"output",  required_argument, 0, 'o'},
        {"target",  required_argument, 0, 't'},
        {"optimize",no_argument,       0, 'O'},
        {"verbose", no_argument,       0, 'v'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:t:Ov", longopts, NULL)) != -1) {
        switch (c) {
            case 'i': cfg.input   = optarg; break;
            case 'o': cfg.output  = optarg; break;
            case 't':
                if (!strcmp(optarg, "exe"))  cfg.target = BIN_EXE;
                else if (!strcmp(optarg, "pkg")) cfg.target = BIN_PKG;
                else if (!strcmp(optarg, "apk")) cfg.target = BIN_APK;
                else { CH_LOG_ERR("unknown target %s", optarg); return 1; }
                break;
            case 'O': cfg.optimize = 1; break;
            case 'v': cfg.verbose  = 1; break;
        }
    }
    if (!cfg.input) { CH_LOG_ERR("missing -i"); return 1; }
    return ch_compile(&cfg) == 0 ? 0 : 1;
}

static int cmd_decompile(int argc, char **argv) {
    ch_decompile_cfg_t cfg = {0};
    int pseudo = 0;
    static struct option longopts[] = {
        {"input",    required_argument, 0, 'i'},
        {"output",   required_argument, 0, 'o'},
        {"arch",     required_argument, 0, 'a'},
        {"pseudo-c", no_argument,       0, 'p'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:a:p", longopts, NULL)) != -1) {
        switch (c) {
            case 'i': cfg.input  = optarg; break;
            case 'o': cfg.output = optarg; break;
            case 'a':
                if      (!strcmp(optarg,"x86_64")) cfg.arch = CH_DASM_X86_64;
                else if (!strcmp(optarg,"arm64"))  cfg.arch = CH_DASM_ARM64;
                else if (!strcmp(optarg,"arm32"))  cfg.arch = CH_DASM_ARM32;
                else                                 cfg.arch = CH_DASM_AUTO;
                break;
            case 'p': pseudo = 1; break;
        }
    }
    cfg.emit_pseudo_c = pseudo;
    if (!cfg.input) { CH_LOG_ERR("missing -i"); return 1; }
    return ch_decompile(&cfg) == 0 ? 0 : 1;
}

static int cmd_detect(int argc, char **argv) {
    if (argc < 3) { CH_LOG_ERR("need file path"); return 1; }
    ch_lang_t l = ch_detect_lang(argv[2]);
    if (l == LANG_UNKNOWN) {
        fprintf(stdout, "unknown\n");
        return 1;
    }
    fprintf(stdout, "%s\n", ch_lang_name(l));
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];

    /* 让子进程继承 /usr/local/bin 等常见路径，便于探测 gcc/go 等 */
    putenv("PATH=/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/go/bin:$PATH");

    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-v")) {
        printf("CompilerHub %s  build=%s\n", CH_VERSION, __DATE__);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(); return 0;
    }
    if (!strcmp(cmd, "compile"))   return cmd_compile(argc-1, argv+1);
    if (!strcmp(cmd, "decompile")) return cmd_decompile(argc-1, argv+1);
    if (!strcmp(cmd, "detect"))    return cmd_detect(argc, argv);
    if (!strcmp(cmd, "serve"))     {
        const char *port = "8601";
        if (argc >= 3 && !strcmp(argv[2], "-p") && argc >= 4) port = argv[3];
        else if (argc >= 3 && !strncmp(argv[2], "--port=", 7)) port = argv[2] + 7;
        else if (argc >= 3 && argv[2][0] != '-') port = argv[2];
        return ch_serve(port);
    }

    CH_LOG_ERR("unknown command: %s", cmd);
    usage();
    return 1;
}
