#ifndef URL_H
#define URL_H

#include "http.h"

bool url_host_matches_no_proxy(const char* host, const char* list);
bool url_is_oppo_download_check(const char* url);
const proxy_endpoint_t* url_select_proxy(const proxy_config_t* config,
                                         const char* scheme,
                                         const char* host);

#endif /* URL_H */
