#include "ofs_core.h"
#include <stdio.h>
static int fail(const char *m){fprintf(stderr,"ofs core test: %s\n",m);return 1;}
int main(void){ifs_ofs_u32 f=0;
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_OFS,&f)||f!=0)return fail("OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_INTL_OFS,&f)||f!=IFS_OFS_VARIANT_INTL)return fail("INTL OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_DOS_DC_OFS,&f)||f!=(IFS_OFS_VARIANT_INTL|IFS_OFS_VARIANT_DIRCACHE))return fail("DC OFS");
if(ifs_ofs_classify_dostype(IFS_OFS_MUFS_DC_OFS,&f)||f!=(IFS_OFS_VARIANT_MUFS|IFS_OFS_VARIANT_INTL|IFS_OFS_VARIANT_DIRCACHE))return fail("MUFS DC OFS");
if(ifs_ofs_classify_dostype(0x444F5301U,&f)==0)return fail("FFS accepted");
return 0;}
