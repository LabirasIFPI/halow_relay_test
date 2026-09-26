// Adicione estes includes no topo do arquivo
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <cstdio>

// Adicione esta função antes do main
static void sigHandler(int sig) {
    void* frames[30];
    size_t n = backtrace(frames, 30);
    fprintf(stderr, "\n=== SIG %d capturado ===\n", sig);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(1);
}