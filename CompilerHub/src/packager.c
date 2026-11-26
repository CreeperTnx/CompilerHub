#include "packager.h"
#include <unistd.h>

int ch_pack_native_into_pkg(const char *exe_path, const char *out_pkg) {
    char tmpdir[1024];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/ch_npkg_%d", (int)getpid());
    ch_mkdir_p(tmpdir);
    char *bindir = ch_join_path(tmpdir, "bin");
    char *metadir = ch_join_path(tmpdir, "meta");
    ch_mkdir_p(bindir);
    ch_mkdir_p(metadir);

    char *appbin = ch_join_path(bindir, "app");
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "cp '%s' '%s' && chmod +x '%s'", exe_path, appbin, appbin);
    system(cmd);

    char run[] = "#!/bin/sh\n"
                 "DIR=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\"\n"
                 "exec \"$DIR/bin/app\" \"$@\"\n";
    char *run_path = ch_join_path(tmpdir, "run");
    ch_write_file(run_path, run, strlen(run));
    char modcmd[2048];
    snprintf(modcmd, sizeof modcmd, "chmod +x '%s'", run_path);
    system(modcmd);

    char *pkgjson = ch_join_path(metadir, "pkg.json");
    char meta[1024];
    snprintf(meta, sizeof meta,
             "{\"type\":\"native-pkg\",\"arch\":\"%s\",\"os\":\"%s\"}\n",
#if defined(__aarch64__) || defined(__arm64__)
             "arm64",
#elif defined(__x86_64__)
             "x86_64",
#else
             "unknown",
#endif
#ifdef __APPLE__
             "darwin"
#elif defined(__linux__)
             "linux"
#else
             "windows"
#endif
    );
    ch_write_file(pkgjson, meta, strlen(meta));
    free(pkgjson);

    int rc = ch_zip_dir(tmpdir, out_pkg);
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf '%s'", tmpdir);
    system(rm);

    free(bindir); free(metadir); free(appbin); free(run_path);
    return rc == 0 ? 0 : -1;
}
