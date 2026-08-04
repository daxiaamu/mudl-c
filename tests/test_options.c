#include "options.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_url_after_options(void) {
    options_t opts;
    char* argv[] = {
        "mudl", "-d", "D:\\Downloads", "-o", "file.bin", "-c", "16",
        "--checksum=md5=0123456789abcdef0123456789abcdef",
        "https://example.com/file.bin"
    };
    options_parse(&opts, (int)(sizeof(argv) / sizeof(argv[0])), argv);
    assert(strcmp(opts.url, "https://example.com/file.bin") == 0);
    assert(strcmp(opts.dir, "D:\\Downloads") == 0);
    assert(strcmp(opts.output, "file.bin") == 0);
    assert(opts.connections == 16);
    assert(strcmp(opts.checksum, "md5=0123456789abcdef0123456789abcdef") == 0);
}

static void test_merged_trailing_backslash_arguments(void) {
    options_t opts;
    char merged[] = "D:\\下载目录\\\" -c 32 --progress line";
    char* argv[] = {"mudl", "-d", merged, "https://example.com/file.bin"};
    options_parse(&opts, 4, argv);
    assert(strcmp(opts.dir, "D:\\下载目录\\") == 0);
    assert(opts.connections == 32);
    assert(opts.progress_mode == PROGRESS_LINE);
    assert(strcmp(opts.url, "https://example.com/file.bin") == 0);
}

static void test_connection_limits(void) {
    options_t low, high;
    char* low_argv[] = {"mudl", "-c", "0", "https://example.com/a"};
    char* high_argv[] = {"mudl", "-c", "99", "https://example.com/b"};
    options_parse(&low, 4, low_argv);
    options_parse(&high, 4, high_argv);
    assert(low.connections == 1);
    assert(high.connections == 32);
}

static void test_multiple_source_urls(void) {
    options_t opts;
    char* argv[] = {
        "mudl", "-c", "8",
        "https://host.example/file.bin",
        "https://mirror.example/file.bin"
    };
    options_parse(&opts, (int)(sizeof(argv) / sizeof(argv[0])), argv);
    assert(opts.url_count == 2);
    assert(strcmp(opts.url, "https://host.example/file.bin") == 0);
    assert(strcmp(opts.urls[0], opts.url) == 0);
    assert(strcmp(opts.urls[1], "https://mirror.example/file.bin") == 0);
}

static void test_retry_limits(void) {
    options_t zero, maximum;
    char* zero_argv[] = {
        "mudl", "--retries", "0", "https://example.com/a"
    };
    char* maximum_argv[] = {
        "mudl", "--retries=100", "https://example.com/b"
    };
    options_parse(&zero, (int)(sizeof(zero_argv) / sizeof(zero_argv[0])),
                  zero_argv);
    options_parse(&maximum,
                  (int)(sizeof(maximum_argv) / sizeof(maximum_argv[0])),
                  maximum_argv);
    assert(zero.max_retries == 0);
    assert(maximum.max_retries == MAX_RETRIES);
}

int main(void) {
    test_url_after_options();
    test_merged_trailing_backslash_arguments();
    test_connection_limits();
    test_multiple_source_urls();
    test_retry_limits();
    puts("option unit tests passed");
    return 0;
}
