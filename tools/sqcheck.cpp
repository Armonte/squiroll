// sqcheck — Squirrel 3.0.6 syntax pre-flight.
//
// Reads each .nut file passed on the command line and runs it through
// sq_compilebuffer. Reports compile errors to stderr and exits non-zero
// if any file fails. Build-only; never shipped with the DLL.
//
// Build:  g++ -O2 -DSQUSEDOUBLE -I deps/squirrel/include \
//             tools/sqcheck.cpp deps/squirrel/squirrel/*.cpp \
//             -o tools/sqcheck
//
// Usage:  tools/sqcheck file1.nut file2.nut ...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <squirrel.h>

static int g_failed = 0;
static const char* g_current_file = "<none>";

static void compile_error_handler(HSQUIRRELVM /*v*/, const SQChar* desc,
                                  const SQChar* source, SQInteger line, SQInteger column)
{
    fprintf(stderr, "sqcheck: %s(%lld,%lld): %s\n",
            source ? source : g_current_file,
            (long long)line, (long long)column,
            desc ? desc : "(no description)");
    g_failed = 1;
}

static int check_file(HSQUIRRELVM v, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sqcheck: cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    g_current_file = path;
    SQRESULT r = sq_compilebuffer(v, buf, (SQInteger)len, path, /*raiseerror=*/SQTrue);
    free(buf);

    if (SQ_FAILED(r)) {
        // The compile_error_handler already printed details.
        if (!g_failed) {
            // Fallback if no handler invocation happened.
            fprintf(stderr, "sqcheck: %s: compile failed (no error detail)\n", path);
            g_failed = 1;
        }
        return 1;
    }
    // Pop the compiled closure off the stack so the next file starts clean.
    sq_pop(v, 1);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.nut> [...]\n", argv[0]);
        return 2;
    }

    HSQUIRRELVM v = sq_open(/*initialstacksize=*/1024);
    if (!v) {
        fprintf(stderr, "sqcheck: sq_open failed\n");
        return 2;
    }
    sq_setcompilererrorhandler(v, compile_error_handler);

    int any_fail = 0;
    for (int i = 1; i < argc; ++i) {
        if (check_file(v, argv[i]) != 0) any_fail = 1;
    }

    sq_close(v);
    return any_fail ? 1 : 0;
}
