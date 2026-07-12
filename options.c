#include "options.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void set_proxy_option(proxy_endpoint_t* dst, const char* value);
static void parse_embedded_args(options_t* opts, char* arg_tail);
static void split_embedded_args(options_t* opts, char* value);
static char* trim_token_quotes(char* s);

void options_parse(options_t* opts, int argc, char** argv) {
    memset(opts, 0, sizeof(options_t));
    opts->connections = DEFAULT_CONNECTIONS;
    opts->timeout_sec = DEFAULT_TIMEOUT;
    opts->max_retries = DEFAULT_RETRY;
    opts->progress_mode = PROGRESS_BAR;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            opts->help = true; return;
        }
        else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            opts->version = true; return;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts->quiet = true;
        }
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i+1 < argc) {
            strncpy(opts->output, argv[++i], sizeof(opts->output) - 1);
            split_embedded_args(opts, opts->output);
        }
        else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) && i+1 < argc) {
            strncpy(opts->dir, argv[++i], sizeof(opts->dir) - 1);
            split_embedded_args(opts, opts->dir);
        }
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--connections") == 0) && i+1 < argc) {
            opts->connections = atoi(argv[++i]);
            if (opts->connections < 1) opts->connections = 1;
            if (opts->connections > 32) opts->connections = 32;
        }
        else if ((strcmp(argv[i], "-ua") == 0 || strcmp(argv[i], "--user-agent") == 0) && i+1 < argc) {
            strncpy(opts->user_agent, argv[++i], sizeof(opts->user_agent) - 1);
        }
        else if (strcmp(argv[i], "--referer") == 0 && i+1 < argc) {
            strncpy(opts->referer, argv[++i], sizeof(opts->referer) - 1);
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--progress") == 0) && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "bar") == 0) opts->progress_mode = PROGRESS_BAR;
            else if (strcmp(argv[i], "line") == 0) opts->progress_mode = PROGRESS_LINE;
            else if (strcmp(argv[i], "json") == 0) opts->progress_mode = PROGRESS_JSON;
            else if (strcmp(argv[i], "none") == 0 || strcmp(argv[i], "quiet") == 0)
                opts->progress_mode = PROGRESS_SILENT;
            else die("Unknown progress mode: %s", argv[i]);
        }
        else if (strcmp(argv[i], "--header") == 0 && i+1 < argc) {
            if (opts->extra_count < 32)
                opts->extra_headers[opts->extra_count++] = _strdup(argv[++i]);
        }
        else if (strcmp(argv[i], "--timeout") == 0 && i+1 < argc) {
            opts->timeout_sec = atoi(argv[++i]);
            if (opts->timeout_sec < 1) opts->timeout_sec = 1;
        }
        else if (strcmp(argv[i], "--retries") == 0 && i+1 < argc) {
            opts->max_retries = atoi(argv[++i]);
            if (opts->max_retries < 0) opts->max_retries = 0;
        }
        else if (strcmp(argv[i], "--checksum") == 0 && i+1 < argc) {
            strncpy(opts->checksum, argv[++i], sizeof(opts->checksum) - 1);
        }
        else if (strncmp(argv[i], "--checksum=", 11) == 0) {
            strncpy(opts->checksum, argv[i] + 11, sizeof(opts->checksum) - 1);
        }
        else if ((strcmp(argv[i], "--proxy") == 0 || strcmp(argv[i], "--all-proxy") == 0) && i+1 < argc) {
            set_proxy_option(&opts->proxy.all, argv[++i]);
        }
        else if (strcmp(argv[i], "--http-proxy") == 0 && i+1 < argc) {
            set_proxy_option(&opts->proxy.http, argv[++i]);
        }
        else if (strcmp(argv[i], "--https-proxy") == 0 && i+1 < argc) {
            set_proxy_option(&opts->proxy.https, argv[++i]);
        }
        else if (strcmp(argv[i], "--no-proxy") == 0 && i+1 < argc) {
            strncpy(opts->proxy.no_proxy, argv[++i], sizeof(opts->proxy.no_proxy) - 1);
        }
        else if (argv[i][0] == '-') {
            die("Unknown option: %s. Use -h for help.", argv[i]);
        }
        else {
            if (opts->url[0] == 0)
                strncpy(opts->url, argv[i], sizeof(opts->url) - 1);
        }
    }
}
static void set_proxy_option(proxy_endpoint_t* dst, const char* value) {
    char err[256] = {0};
    if (http_proxy_parse(value, dst, err, sizeof(err)) != 0)
        die("%s", err);
}

static char* trim_token_quotes(char* s) {
    if (!s) return s;

    while (*s == '"')
        s++;

    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '"') {
        s[len - 1] = 0;
        len--;
    }

    return s;
}

static void parse_embedded_args(options_t* opts, char* arg_tail) {
    char* tokens[64];
    int count = 0;

    char* p = arg_tail;
    while (*p && count < 64) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            tokens[count++] = p;
            while (*p && *p != '"') p++;
        } else {
            tokens[count++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }

        if (*p) *p++ = 0;
    }

    for (int i = 0; i < count; i++) {
        tokens[i] = trim_token_quotes(tokens[i]);
    }

    for (int i = 0; i < count; i++) {
        char* tok = tokens[i];
        if (!tok || !tok[0])
            continue;

        if ((strcmp(tok, "-o") == 0 || strcmp(tok, "--output") == 0) && i + 1 < count) {
            char* value = trim_token_quotes(tokens[++i]);
            strncpy(opts->output, value, sizeof(opts->output) - 1);
            opts->output[sizeof(opts->output) - 1] = 0;
        } else if (strncmp(tok, "--output=", 9) == 0) {
            strncpy(opts->output, trim_token_quotes(tok + 9), sizeof(opts->output) - 1);
            opts->output[sizeof(opts->output) - 1] = 0;
        } else if ((strcmp(tok, "-d") == 0 || strcmp(tok, "--dir") == 0) && i + 1 < count) {
            char* value = trim_token_quotes(tokens[++i]);
            strncpy(opts->dir, value, sizeof(opts->dir) - 1);
            opts->dir[sizeof(opts->dir) - 1] = 0;
        } else if (strncmp(tok, "--dir=", 6) == 0) {
            strncpy(opts->dir, trim_token_quotes(tok + 6), sizeof(opts->dir) - 1);
            opts->dir[sizeof(opts->dir) - 1] = 0;
        } else if ((strcmp(tok, "-c") == 0 || strcmp(tok, "--connections") == 0) && i + 1 < count) {
            opts->connections = atoi(trim_token_quotes(tokens[++i]));
            if (opts->connections < 1) opts->connections = 1;
            if (opts->connections > 32) opts->connections = 32;
        } else if (strncmp(tok, "--connections=", 14) == 0) {
            opts->connections = atoi(trim_token_quotes(tok + 14));
            if (opts->connections < 1) opts->connections = 1;
            if (opts->connections > 32) opts->connections = 32;
        } else if ((strcmp(tok, "-p") == 0 || strcmp(tok, "--progress") == 0) && i + 1 < count) {
            i++;
            if (strcmp(tokens[i], "bar") == 0) opts->progress_mode = PROGRESS_BAR;
            else if (strcmp(tokens[i], "line") == 0) opts->progress_mode = PROGRESS_LINE;
            else if (strcmp(tokens[i], "json") == 0) opts->progress_mode = PROGRESS_JSON;
            else if (strcmp(tokens[i], "none") == 0 || strcmp(tokens[i], "quiet") == 0)
                opts->progress_mode = PROGRESS_SILENT;
        } else if (strncmp(tok, "--progress=", 11) == 0) {
            char* value = trim_token_quotes(tok + 11);
            if (strcmp(value, "bar") == 0) opts->progress_mode = PROGRESS_BAR;
            else if (strcmp(value, "line") == 0) opts->progress_mode = PROGRESS_LINE;
            else if (strcmp(value, "json") == 0) opts->progress_mode = PROGRESS_JSON;
            else if (strcmp(value, "none") == 0 || strcmp(value, "quiet") == 0)
                opts->progress_mode = PROGRESS_SILENT;
        } else if ((strcmp(tok, "-ua") == 0 || strcmp(tok, "--user-agent") == 0) && i + 1 < count) {
            char* value = trim_token_quotes(tokens[++i]);
            strncpy(opts->user_agent, value, sizeof(opts->user_agent) - 1);
            opts->user_agent[sizeof(opts->user_agent) - 1] = 0;
        } else if (strncmp(tok, "--user-agent=", 13) == 0) {
            strncpy(opts->user_agent, trim_token_quotes(tok + 13), sizeof(opts->user_agent) - 1);
            opts->user_agent[sizeof(opts->user_agent) - 1] = 0;
        } else if (strcmp(tok, "--referer") == 0 && i + 1 < count) {
            char* value = trim_token_quotes(tokens[++i]);
            strncpy(opts->referer, value, sizeof(opts->referer) - 1);
            opts->referer[sizeof(opts->referer) - 1] = 0;
        } else if (strncmp(tok, "--referer=", 10) == 0) {
            strncpy(opts->referer, trim_token_quotes(tok + 10), sizeof(opts->referer) - 1);
            opts->referer[sizeof(opts->referer) - 1] = 0;
        } else if (strcmp(tok, "--header") == 0 && i + 1 < count) {
            if (opts->extra_count < 32)
                opts->extra_headers[opts->extra_count++] = _strdup(trim_token_quotes(tokens[++i]));
        } else if (strncmp(tok, "--header=", 9) == 0) {
            if (opts->extra_count < 32)
                opts->extra_headers[opts->extra_count++] = _strdup(trim_token_quotes(tok + 9));
        } else if (strcmp(tok, "--timeout") == 0 && i + 1 < count) {
            opts->timeout_sec = atoi(trim_token_quotes(tokens[++i]));
            if (opts->timeout_sec < 1) opts->timeout_sec = 1;
        } else if (strncmp(tok, "--timeout=", 10) == 0) {
            opts->timeout_sec = atoi(trim_token_quotes(tok + 10));
            if (opts->timeout_sec < 1) opts->timeout_sec = 1;
        } else if (strcmp(tok, "--retries") == 0 && i + 1 < count) {
            opts->max_retries = atoi(trim_token_quotes(tokens[++i]));
            if (opts->max_retries < 0) opts->max_retries = 0;
        } else if (strncmp(tok, "--retries=", 10) == 0) {
            opts->max_retries = atoi(trim_token_quotes(tok + 10));
            if (opts->max_retries < 0) opts->max_retries = 0;
        } else if (strcmp(tok, "--checksum") == 0 && i + 1 < count) {
            strncpy(opts->checksum, trim_token_quotes(tokens[++i]), sizeof(opts->checksum) - 1);
            opts->checksum[sizeof(opts->checksum) - 1] = 0;
        } else if (strncmp(tok, "--checksum=", 11) == 0) {
            strncpy(opts->checksum, trim_token_quotes(tok + 11), sizeof(opts->checksum) - 1);
            opts->checksum[sizeof(opts->checksum) - 1] = 0;
        } else if ((strcmp(tok, "--proxy") == 0 || strcmp(tok, "--all-proxy") == 0) && i + 1 < count) {
            set_proxy_option(&opts->proxy.all, trim_token_quotes(tokens[++i]));
        } else if (strncmp(tok, "--proxy=", 8) == 0) {
            set_proxy_option(&opts->proxy.all, trim_token_quotes(tok + 8));
        } else if (strncmp(tok, "--all-proxy=", 12) == 0) {
            set_proxy_option(&opts->proxy.all, trim_token_quotes(tok + 12));
        } else if (strcmp(tok, "--http-proxy") == 0 && i + 1 < count) {
            set_proxy_option(&opts->proxy.http, trim_token_quotes(tokens[++i]));
        } else if (strncmp(tok, "--http-proxy=", 13) == 0) {
            set_proxy_option(&opts->proxy.http, trim_token_quotes(tok + 13));
        } else if (strcmp(tok, "--https-proxy") == 0 && i + 1 < count) {
            set_proxy_option(&opts->proxy.https, trim_token_quotes(tokens[++i]));
        } else if (strncmp(tok, "--https-proxy=", 14) == 0) {
            set_proxy_option(&opts->proxy.https, trim_token_quotes(tok + 14));
        } else if (strcmp(tok, "--no-proxy") == 0 && i + 1 < count) {
            strncpy(opts->proxy.no_proxy, trim_token_quotes(tokens[++i]), sizeof(opts->proxy.no_proxy) - 1);
            opts->proxy.no_proxy[sizeof(opts->proxy.no_proxy) - 1] = 0;
        } else if (strncmp(tok, "--no-proxy=", 11) == 0) {
            strncpy(opts->proxy.no_proxy, trim_token_quotes(tok + 11), sizeof(opts->proxy.no_proxy) - 1);
            opts->proxy.no_proxy[sizeof(opts->proxy.no_proxy) - 1] = 0;
        } else if (strcmp(tok, "-q") == 0 || strcmp(tok, "--quiet") == 0) {
            opts->quiet = true;
        } else if (tok[0] != '-' && opts->url[0] == 0) {
            strncpy(opts->url, tok, sizeof(opts->url) - 1);
            opts->url[sizeof(opts->url) - 1] = 0;
        }
    }
}

static void split_embedded_args(options_t* opts, char* value) {
    char* q = strchr(value, '"');
    if (!q) return;

    char* tail = q + 1;
    while (*tail == ' ' || *tail == '\t') tail++;
    if (*tail != '-') return;

    *q = 0;
    parse_embedded_args(opts, tail);
}
void options_print_help(void) {
    printf("大侠阿木：daxiaamu.com\n");
    printf("MUDL - Multi-threaded Universal Downloader\n");
    printf("Usage: mudl [options] <URL>\n\n");
    printf("Options:\n");
    printf("  -o,  --output <FILE>      Output filename\n");
    printf("  -d,  --dir <DIR>          Output directory\n");
    printf("  -c,  --connections <N>    Connections (default %d, 1-32)\n", DEFAULT_CONNECTIONS);
    printf("  -q,  --quiet              Hide detail logs, keep progress\n");
    printf("  -p,  --progress <FORMAT>  Progress: bar|line|json|none\n");
    printf("  -ua, --user-agent <UA>    Custom User-Agent\n");
    printf("       --referer <URL>      Referer\n");
    printf("       --header <K:V>       Custom HTTP header (repeatable)\n");
    printf("       --timeout <SEC>      Timeout (default %d)\n", DEFAULT_TIMEOUT);
    printf("       --retries <N>        Retries (default %d)\n", DEFAULT_RETRY);
    printf("       --checksum <TYPE=DIGEST> Verify file checksum after download\n");
    printf("       --proxy <PROXY>      Alias for --all-proxy\n");
    printf("       --all-proxy <PROXY>  Proxy for all HTTP(S) downloads\n");
    printf("       --http-proxy <PROXY> Proxy for HTTP downloads\n");
    printf("       --https-proxy <PROXY> Proxy for HTTPS downloads\n");
    printf("       --no-proxy <LIST>    Comma-separated hosts/domains/IP ranges\n");
    printf("  -h,  --help               Show help\n");
    printf("  -V,  --version            Show version\n\n");
    printf("Examples:\n");
    printf("  mudl https://example.com/file.zip\n");
    printf("  mudl -c 8 https://example.com/large.iso\n");
    printf("  mudl -q -p json https://example.com/file.bin\n");
}
void options_print_version(void) {
    printf("MUDL v%s\n", MUDL_VERSION);
    printf("Multi-threaded Universal Downloader (MUDL)\n");
}

