#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <windows.h>
#include <shellapi.h>

#include "options.h"
#include "engine.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

typedef struct {
    UINT input_cp;
    UINT output_cp;
    HANDLE output_handle;
    DWORD output_mode;
    bool has_output_mode;
    bool initialized;
} console_state_t;

static console_state_t g_console_state = {0};

static void sig_handler(int sig);
static char** command_line_to_utf8_argv(int* out_argc);
static void free_utf8_argv(char** argv, int argc);
static void restore_console(void);

/* Fix Chinese output on Windows console */
static void init_console(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    g_console_state.input_cp = GetConsoleCP();
    g_console_state.output_cp = GetConsoleOutputCP();
    g_console_state.output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_console_state.output_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(g_console_state.output_handle, &g_console_state.output_mode)) {
        g_console_state.has_output_mode = true;
    }
    g_console_state.initialized = true;
    atexit(restore_console);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    /* Enable VT escape sequences */
    DWORD mode;
    if (g_console_state.has_output_mode) {
        mode = g_console_state.output_mode;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(g_console_state.output_handle, mode);
    }
}

static void restore_console(void) {
    if (!g_console_state.initialized)
        return;
    if (g_console_state.output_cp)
        SetConsoleOutputCP(g_console_state.output_cp);
    if (g_console_state.input_cp)
        SetConsoleCP(g_console_state.input_cp);
    if (g_console_state.has_output_mode)
        SetConsoleMode(g_console_state.output_handle, g_console_state.output_mode);
}

static char** command_line_to_utf8_argv(int* out_argc) {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0)
        return NULL;

    char** argv = (char**)calloc((size_t)wargc + 1, sizeof(char*));
    if (!argv) {
        LocalFree(wargv);
        return NULL;
    }

    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) {
            free_utf8_argv(argv, i);
            LocalFree(wargv);
            return NULL;
        }
        argv[i] = (char*)calloc((size_t)n, 1);
        if (!argv[i]) {
            free_utf8_argv(argv, i);
            LocalFree(wargv);
            return NULL;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }

    argv[wargc] = NULL;
    *out_argc = wargc;
    LocalFree(wargv);
    return argv;
}

static void free_utf8_argv(char** argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

int main(int argc, char** argv) {
    init_console();

    int parse_argc = argc;
    char** parse_argv = argv;
    bool argv_owned = false;
    char** utf8_argv = command_line_to_utf8_argv(&parse_argc);
    if (utf8_argv) {
        parse_argv = utf8_argv;
        argv_owned = true;
    }

    options_t opts;
    options_parse(&opts, parse_argc, parse_argv);
    if (argv_owned) {
        free_utf8_argv(parse_argv, parse_argc);
    }
    if (opts.help) { options_print_help(); return 0; }
    if (opts.version) { options_print_version(); return 0; }
    if (opts.url[0] == 0) {
        fprintf(stderr, "Error: URL required. Use -h for help.\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    return engine_run(&opts);
}

static void sig_handler(int sig) {
    (void)sig;
    engine_interrupt();
}
