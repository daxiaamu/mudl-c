#ifndef NETWORK_H
#define NETWORK_H

#include <winsock2.h>

/* Present since Windows Vista, but absent from older MinGW-w64 headers. */
#ifndef IP_UNICAST_IF
#define IP_UNICAST_IF 31
#endif

int network_bind_interface(SOCKET fd, int family, const char* interface_name,
                           char* err, int err_n);

#endif /* NETWORK_H */
