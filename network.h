#ifndef NETWORK_H
#define NETWORK_H

#include <winsock2.h>

int network_bind_interface(SOCKET fd, int family, const char* interface_name,
                           char* err, int err_n);

#endif /* NETWORK_H */
