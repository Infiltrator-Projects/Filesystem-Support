#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "amigaffs.h"
#include "../../amiga_common/linux/amigados_linux.h"
