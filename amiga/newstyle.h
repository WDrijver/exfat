/* Shim.  docs/toolchain/_ApolloLib/ApolloCrossDev_Debug.c includes a local
   "newstyle.h", which in sagasd.device is a private copy.  Here we use the
   NDK 3.2 one instead so there is a single definition in the build. */
#ifndef EXFAT_NEWSTYLE_SHIM_H
#define EXFAT_NEWSTYLE_SHIM_H
#include <devices/newstyle.h>
#endif
