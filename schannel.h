#ifndef MUDL_SCHANNEL_H
#define MUDL_SCHANNEL_H

#include "http.h"

int schannel_connect(http_client_t* client);
int schannel_send(http_client_t* client, const char* data, int length);
int schannel_recv(http_client_t* client, char* buffer, int buffer_size);
void schannel_close(http_client_t* client);
void schannel_global_cleanup(void);

#endif /* MUDL_SCHANNEL_H */
