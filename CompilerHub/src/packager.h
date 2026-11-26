#ifndef CH_PACKAGER_H
#define CH_PACKAGER_H

#include "utils.h"

#ifdef __APPLE__
# define EXE_SUFFIX ""
#else
# define EXE_SUFFIX "exe"
#endif

/* 把一个已编译好的原生可执行文件，塞进 pkg 容器，附带运行时外壳 */
int ch_pack_native_into_pkg(const char *exe_path, const char *out_pkg);

#endif
