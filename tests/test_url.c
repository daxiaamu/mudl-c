#include "url.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_url_parse(void) {
    char scheme[16], host[256], path[HTTP_MAX_PATH];
    int port = 0;
    assert(http_parse_url("https://example.com:8443/a/b?q=1", scheme,
                          sizeof(scheme), host, sizeof(host), &port,
                          path, sizeof(path)) == 0);
    assert(strcmp(scheme, "https") == 0);
    assert(strcmp(host, "example.com") == 0);
    assert(port == 8443);
    assert(strcmp(path, "/a/b?q=1") == 0);
    assert(http_parse_url("not-a-url", scheme, sizeof(scheme), host,
                          sizeof(host), &port, path, sizeof(path)) != 0);
}

static void test_proxy_parse(void) {
    proxy_endpoint_t proxy;
    char err[256];
    assert(http_proxy_parse("http://user:pass@127.0.0.1:7890", &proxy,
                            err, sizeof(err)) == 0);
    assert(proxy.enabled);
    assert(strcmp(proxy.host, "127.0.0.1") == 0);
    assert(proxy.port == 7890);
    assert(strcmp(proxy.auth, "Basic dXNlcjpwYXNz") == 0);
}

static void test_no_proxy(void) {
    assert(url_host_matches_no_proxy("localhost", "localhost,.example.com"));
    assert(url_host_matches_no_proxy("api.example.com", ".example.com"));
    assert(url_host_matches_no_proxy("example.com", ".example.com"));
    assert(url_host_matches_no_proxy("192.168.2.3", "192.168.0.0/16"));
    assert(!url_host_matches_no_proxy("example.net", ".example.com"));
}

static void test_proxy_selection(void) {
    proxy_config_t config = {0};
    config.all.enabled = true;
    config.all.port = 8000;
    config.https.enabled = true;
    config.https.port = 8443;
    strcpy(config.no_proxy, ".internal.example");
    assert(url_select_proxy(&config, "http", "public.example")->port == 8000);
    assert(url_select_proxy(&config, "https", "public.example")->port == 8443);
    assert(url_select_proxy(&config, "https", "api.internal.example") == NULL);
}

static void test_oppo_download_check(void) {
    assert(url_is_oppo_download_check(
        "https://component-ota-cn.allawntech.com/downloadCheck?c=1"));
    assert(url_is_oppo_download_check(
        "https://component-ota-cn.allawnos.com/downloadCheck?c=1"));
    assert(url_is_oppo_download_check(
        "HTTPS://A.ALLAWNOS.COM/downloadcheck?x=1"));
    assert(!url_is_oppo_download_check(
        "http://component-ota-cn.allawnos.com/downloadCheck?c=1"));
    assert(!url_is_oppo_download_check(
        "https://allawnos.com/downloadCheck?c=1"));
    assert(!url_is_oppo_download_check(
        "https://evilallawnos.com/downloadCheck?c=1"));
    assert(!url_is_oppo_download_check(
        "https://evilallawntech.com/downloadCheck?c=1"));
    assert(!url_is_oppo_download_check(
        "https://evilallawnos.com/downloadCheck?c=1"));
    assert(!url_is_oppo_download_check(
        "https://component-ota-cn.allawnos.com/downloadCheck"));
    assert(!url_is_oppo_download_check(
        "https://component-ota-cn.allawnos.com/other?c=1"));
}

int main(void) {
    test_url_parse();
    test_proxy_parse();
    test_no_proxy();
    test_proxy_selection();
    test_oppo_download_check();
    puts("URL and proxy unit tests passed");
    return 0;
}
