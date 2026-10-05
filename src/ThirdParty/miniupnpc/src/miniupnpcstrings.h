/* band3's stand-in for the miniupnpcstrings.h that miniupnpc's own build
 * generates from miniupnpcstrings.h.in: the strings it sends in its HTTP and
 * SSDP User-Agent headers. Upstream fills OS_STRING with the build machine's
 * OS name and version; band3 sends its own name instead. */
#ifndef MINIUPNPCSTRINGS_H_INCLUDED
#define MINIUPNPCSTRINGS_H_INCLUDED

#define OS_STRING "band3"
#define MINIUPNPC_VERSION_STRING "2.3.3"

/* according to "UPnP Device Architecture 1.1", as upstream's default */
#define UPNP_VERSION_MAJOR 1
#define UPNP_VERSION_MINOR 1
#define UPNP_VERSION_STRING "UPnP/1.1"

#endif
