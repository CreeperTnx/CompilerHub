/*
 * CompilerHub 编译调度器
 * ----------------------
 * 核心哲学：本工具不内置任何语言的编译器，而是把"多语言 → 可执行"这件事
 * 统一抽象成"调度外部工具 + 规范化产物"。
 */
#include "compiler.h"
#include "packager.h"
#include <ctype.h>
#include <unistd.h>

ch_lang_t ch_detect_lang(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return LANG_UNKNOWN;
    dot++;
    if (strcmp(dot, "c")   == 0) return LANG_C;
    if (strcmp(dot, "cpp") == 0 || strcmp(dot, "cc") == 0 || strcmp(dot, "cxx") == 0 || strcmp(dot, "C") == 0) return LANG_CPP;
    if (strcmp(dot, "py")  == 0) return LANG_PYTHON;
    if (strcmp(dot, "js")  == 0 || strcmp(dot, "mjs") == 0) return LANG_JS;
    if (strcmp(dot, "go")  == 0) return LANG_GO;
    if (strcmp(dot, "rs")  == 0) return LANG_RUST;
    if (strcmp(dot, "java")== 0) return LANG_JAVA;
    if (strcmp(dot, "swift")== 0) return LANG_SWIFT;
    if (strcmp(dot, "kt")  == 0) return LANG_KOTLIN;
    return LANG_UNKNOWN;
}

const char *ch_lang_name(ch_lang_t l) {
    switch (l) {
        case LANG_C: return "c";
        case LANG_CPP: return "c++";
        case LANG_PYTHON: return "python";
        case LANG_JS: return "javascript";
        case LANG_GO: return "go";
        case LANG_RUST: return "rust";
        case LANG_JAVA: return "java";
        case LANG_SWIFT: return "swift";
        case LANG_KOTLIN: return "kotlin";
        default: return "unknown";
    }
}

const char *ch_target_ext(ch_target_t t) {
    switch (t) {
        case BIN_EXE: return EXE_SUFFIX;
        case BIN_PKG: return "pkg";
        case BIN_APK: return "apk";
    }
    return "bin";
}

static int have(const char *tool) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

static int compile_exe_c(const ch_compile_cfg_t *cfg, ch_lang_t lang, const char *out) {
    const char *cc = have("clang") ? "clang" : "gcc";
    char cmd[8192];
    const char *std = (lang == LANG_CPP) ? "c++17" : "c11";
    snprintf(cmd, sizeof cmd, "%s %s%s %s -o '%s' '%s'",
             cc, lang==LANG_CPP?"-std=":"-std=", std,
             cfg->optimize ? "-O2" : "-O0",
             out, cfg->input);
    CH_LOG_INFO("cmd: %s", cmd);
    return system(cmd) == 0 ? 0 : -1;
}

static int compile_exe_go(const ch_compile_cfg_t *cfg, const char *out) {
    if (!have("go")) { CH_LOG_ERR("go toolchain not found"); return -1; }
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "go build -o '%s' '%s'", out, cfg->input);
    return system(cmd) == 0 ? 0 : -1;
}

static int compile_exe_rust(const ch_compile_cfg_t *cfg, const char *out) {
    if (!have("rustc")) { CH_LOG_ERR("rustc not found"); return -1; }
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "rustc %s -o '%s' '%s'",
             cfg->optimize ? "-O" : "-C opt-level=0", out, cfg->input);
    return system(cmd) == 0 ? 0 : -1;
}

static int compile_exe_java(const ch_compile_cfg_t *cfg, const char *out) {
    if (!have("javac")) { CH_LOG_ERR("javac not found"); return -1; }
    char cls[2048];
    snprintf(cls, sizeof cls, "mkdir -p '%s' && javac -d '%s' '%s'", out, out, cfg->input);
    return system(cls) == 0 ? 0 : -1;
}

static int build_script_pkg(const ch_compile_cfg_t *cfg, const char *out, ch_lang_t lang) {
    char tmpdir[1024];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/ch_pkg_%d", (int)getpid());
    ch_mkdir_p(tmpdir);
    char *appdir = ch_join_path(tmpdir, "app");
    char *interpdir = ch_join_path(tmpdir, "interpreter");
    ch_mkdir_p(appdir);
    ch_mkdir_p(interpdir);

    char cp[2048];
    snprintf(cp, sizeof cp, "cp '%s' '%s/' 2>/dev/null || cp -R '%s' '%s/'",
             cfg->input, appdir, cfg->input, appdir);
    system(cp);

    char *metapath = ch_join_path(tmpdir, "meta.json");
    char meta[2048];
    snprintf(meta, sizeof meta,
             "{\"lang\":\"%s\",\"entry\":\"%s\",\"target\":\"pkg\"}\n",
             ch_lang_name(lang), ch_basename(cfg->input));
    ch_write_file(metapath, meta, strlen(meta));
    free(metapath);

    const char *interp_cmd = (lang == LANG_PYTHON) ? "python3" :
                             (lang == LANG_JS)      ? "node"    : "sh";
    char run_body[2048];
    snprintf(run_body, sizeof run_body, "#!/bin/sh\n"
             "DIR=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\"\n"
             "exec %s \"$DIR/app/%s\" \"$@\"\n",
             interp_cmd, ch_basename(cfg->input));
    char *run_path = ch_join_path(tmpdir, "run");
    ch_write_file(run_path, run_body, strlen(run_body));

    char chmod[2048];
    snprintf(chmod, sizeof chmod, "chmod +x '%s'", run_path);
    system(chmod);

    int rc = ch_zip_dir(tmpdir, out);

    char rmcmd[1024];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", tmpdir);
    system(rmcmd);

    free(appdir); free(interpdir); free(run_path);
    return rc == 0 ? 0 : -1;
}

static int build_apk_stub(const ch_compile_cfg_t *cfg, const char *out, ch_lang_t lang) {
    char tmpdir[1024];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/ch_apk_%d", (int)getpid());
    ch_mkdir_p(tmpdir);
    char *arm64d  = ch_join_path(tmpdir, "lib/arm64-v8a");
    char *arm32d  = ch_join_path(tmpdir, "lib/armeabi-v7a");
    char *assetsd = ch_join_path(tmpdir, "assets");
    ch_mkdir_p(arm64d);
    ch_mkdir_p(arm32d);
    ch_mkdir_p(assetsd);

    char cp[2048];
    snprintf(cp, sizeof cp, "cp '%s' '%s/' 2>/dev/null", cfg->input, assetsd);
    system(cp);

    const char *man_path = "AndroidManifest.xml";
    char *mp = ch_join_path(tmpdir, man_path);
    char man[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"com.compilerhub.stub\">\n"
        "    <application android:label=\"CHStub\" android:allowBackup=\"true\">\n"
        "        <activity android:name=\".Main\" android:exported=\"true\">\n"
        "            <intent-filter>\n"
        "                <action android:name=\"android.intent.action.MAIN\"/>\n"
        "                <category android:name=\"android.intent.category.LAUNCHER\"/>\n"
        "            </intent-filter>\n"
        "        </activity>\n"
        "    </application>\n"
        "</manifest>\n";
    ch_write_file(mp, man, strlen(man));
    free(mp);

    char *readme = ch_join_path(assetsd, "README.txt");
    char meta[2048];
    snprintf(meta, sizeof meta,
             "CompilerHub APK stub\n"
             "source  : %s\n"
             "language: %s\n"
             "NOTE    : This is a stub APK (zip + manifest skeleton).\n"
             "          Real APK building requires Android SDK (aapt2, d8, apksigner).\n",
             cfg->input, ch_lang_name(lang));
    ch_write_file(readme, meta, strlen(meta));
    free(readme);

    int rc = ch_zip_dir(tmpdir, out);

    char rmcmd[1024];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", tmpdir);
    system(rmcmd);

    free(arm64d); free(arm32d); free(assetsd);
    return rc == 0 ? 0 : -1;
}

int ch_compile(const ch_compile_cfg_t *cfg) {
    if (!cfg || !cfg->input) { CH_LOG_ERR("null config"); return -1; }
    if (!ch_file_exists(cfg->input)) { CH_LOG_ERR("input not found: %s", cfg->input); return -1; }

    ch_lang_t lang = ch_detect_lang(cfg->input);
    if (lang == LANG_UNKNOWN) { CH_LOG_ERR("unknown language for %s", cfg->input); return -1; }

    /* 脚本语言没有原生编译器 —— 选 exe 自动降级为 pkg（不丢用户意图） */
    ch_target_t target = cfg->target;
    int native_lang = (lang==LANG_C || lang==LANG_CPP || lang==LANG_GO || lang==LANG_RUST || lang==LANG_JAVA);
    if (target == BIN_EXE && !native_lang) {
        CH_LOG_WARN("%s 没有原生编译器在本宿主可用；自动把 target 从 exe 降级为 pkg", ch_lang_name(lang));
        target = BIN_PKG;
    }

    const char *out_ext = ch_target_ext(target);
    char *out;
    if (cfg->output && cfg->output[0]) {
        /* 用户给了输出路径 —— 强制把后缀换成目标类型的规范后缀
           （pkg 一定 .pkg，apk 一定 .apk，exe 一定 .后缀），
           防止 macOS zip 命令偷偷追加 .zip 导致日志说 OK→main_out 但
           实际产物是 main_out.zip，前端下载 404 */
        out = ch_replace_ext(cfg->output, out_ext);
    } else {
        out = ch_replace_ext(cfg->input, out_ext);
    }

    CH_LOG_INFO("compile: lang=%s target=%s input=%s output=%s",
                ch_lang_name(lang), out_ext, cfg->input, out);

    int rc = -1;
    switch (target) {
        case BIN_EXE:
            if (lang == LANG_C || lang == LANG_CPP) rc = compile_exe_c(cfg, lang, out);
            else if (lang == LANG_GO)               rc = compile_exe_go(cfg, out);
            else if (lang == LANG_RUST)             rc = compile_exe_rust(cfg, out);
            else if (lang == LANG_JAVA)             rc = compile_exe_java(cfg, out);
            break;
        case BIN_PKG:
            if (lang == LANG_C || lang == LANG_CPP || lang == LANG_GO || lang == LANG_RUST) {
                char *tmpexe = ch_replace_ext(cfg->input, "tmpbin");
                ch_compile_cfg_t sub = *cfg;
                sub.target = BIN_EXE;
                sub.output = tmpexe;
                if (ch_compile(&sub) == 0)
                    rc = ch_pack_native_into_pkg(tmpexe, out);
                unlink(tmpexe);
                free(tmpexe);
            } else {
                rc = build_script_pkg(cfg, out, lang);
            }
            break;
        case BIN_APK:
            rc = build_apk_stub(cfg, out, lang);
            break;
    }

    if (rc == 0) CH_LOG_INFO("OK → %s", out);
    else         CH_LOG_ERR("failed to produce %s", out);
    free(out);
    return rc;
}
