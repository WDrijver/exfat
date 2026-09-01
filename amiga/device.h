/* Shim.  docs/toolchain/_ApolloLib/ApolloCrossDev_Debug.c is shared with
   sagasd.device and includes that project's "device.h" for traceCommand(),
   which prints the unit number of an IORequest.  This handler is not a
   device and has no units; supply just enough for the file to compile.
   traceCommand() itself is compiled out unless DEBUG has DBG_CMD set. */
#ifndef EXFAT_DEVICE_SHIM_H
#define EXFAT_DEVICE_SHIM_H

#include <exec/types.h>

struct DeviceUnit
{
	ULONG	unitNum;
};

#endif
