#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdbool.h>
#include <windows.h>

#include "http.h"
#include "progress.h"

#define MUDL_VERSION "0.5.9"
#define DEFAULT_CONNECTIONS 8
#define DEFAULT_TIMEOUT 30
#define DEFAULT_RETRY 5

typedef struct {
    char        url[HTTP_MAX_URL];
    char        output[MAX_PATH * 2];
    char        dir[MAX_PATH * 2];
    int         connections;
    int         timeout_sec;
    int         max_retries;
    bool        quiet;
    progress_mode_t progress_mode;
    char        user_agent[256];
    char        referer[1024];
    char*       extra_headers[32];
    int         extra_count;
    proxy_config_t proxy;
    char        checksum[256];
    char        resource_validator[256];
    bool        help;
    bool        version;
} options_t;

void options_parse(options_t* opts, int argc, char** argv);
void options_print_help(void);
void options_print_version(void);

#endif /* OPTIONS_H */
