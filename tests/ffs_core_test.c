#include "ffs_core.h"
#include <stdio.h>
static int fail(const char *m){fprintf(stderr,"ffs core test: %s\n",m);return 1;}
int main(void){ifs_ffs_u32 f=0;
if(ifs_ffs_classify_dostype(IFS_FFS_DOS_FFS,&f)||f!=0)return fail("FFS");
if(ifs_ffs_classify_dostype(IFS_FFS_DOS_INTL_FFS,&f)||f!=IFS_FFS_VARIANT_INTL)return fail("INTL FFS");
if(ifs_ffs_classify_dostype(IFS_FFS_DOS_DC_FFS,&f)||f!=(IFS_FFS_VARIANT_INTL|IFS_FFS_VARIANT_DIRCACHE))return fail("DC FFS");
if(ifs_ffs_classify_dostype(IFS_FFS_MUFS_GENERIC,&f)||f!=(IFS_FFS_VARIANT_MUFS|IFS_FFS_VARIANT_INTL))return fail("generic MUFS");
if(ifs_ffs_classify_dostype(0x444F5300U,&f)==0)return fail("OFS accepted");
return 0;}
