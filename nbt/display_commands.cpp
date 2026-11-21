#include "display_commands.hpp"
#include "lifecycle.hpp"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <sys/slog.h>
#endif

extern char** environ;

namespace carlife {
namespace {

pid_t startCommand(const char* executable, char* const args[], int output) {
#ifdef __QNXNTO__
    const int fds[] = {STDIN_FILENO, output, STDERR_FILENO};
    struct inheritance inherit = {};
    inherit.flags = SPAWN_SETGROUP | SPAWN_SETSIGDEF | SPAWN_SETSIGMASK;
    inherit.pgroup = SPAWN_NEWPGROUP;
    sigemptyset(&inherit.sigdefault);
    sigaddset(&inherit.sigdefault, SIGTERM);
    sigemptyset(&inherit.sigmask);
    return spawnp(executable, 3, fds, &inherit, args, environ);
#else
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions)) return -1;
    pid_t pid = -1;
    if (!posix_spawnattr_init(&attr)) {
        if ((output == STDOUT_FILENO || !posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO)) &&
            !posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) &&
            !posix_spawnattr_setpgroup(&attr, 0) &&
            posix_spawnp(&pid, executable, &actions, &attr, args, environ)) pid = -1;
        posix_spawnattr_destroy(&attr);
    }
    posix_spawn_file_actions_destroy(&actions);
    return pid;
#endif
}

int waitFor(pid_t pid, double deadline, int& status) {
    do {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return 1;
        if (done < 0 && errno != EINTR) return -1;
        usleep(10000);
    } while (now() < deadline);
    return 0;
}

int runCommand(const char* executable, char* const args[], const char* description, int output) {
    const pid_t child = startCommand(executable, args, output);
    if (child < 0) return -1;
    int status;
    const int done = waitFor(child, now() + 1.2, status);
    if (done > 0)
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (done < 0) return -1;
    // The shell and its LayerManagerControl children belong to this command only.
    fputs("CarLife display command timed out\n", stderr);
#ifdef __QNXNTO__
    slogf(0, _SLOG_WARNING, "CarLife display command timed out: %.120s", description);
#else
    (void)description;
#endif
    kill(-child, SIGKILL);
    waitFor(child, now() + 0.1, status);
    return -1;
}

FILE* capture(const char* executable, char* const args[], const char* description) {
#ifdef __QNXNTO__
    char name[] = "/dev/shmem/nbt-carlife-lm-XXXXXX";
#else
    char name[] = "/tmp/nbt-carlife-lm-XXXXXX";
#endif
    const int fd = mkstemp(name);
    if (fd < 0) return NULL;
    if (unlink(name) == 0 && runCommand(executable, args, description, fd) == 0 &&
        lseek(fd, 0, SEEK_SET) == 0) {
        FILE* result = fdopen(fd, "r");
        if (result) return result;
    }
    close(fd);
    return NULL;
}

}

int displayRun(const char* command) {
    char* args[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command), NULL};
    return runCommand("/bin/sh", args, command, STDOUT_FILENO);
}

FILE* displayCapture(const char* command) {
    char* args[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command), NULL};
    return capture("/bin/sh", args, command);
}

FILE* displayCaptureProgram(const char* executable, const char* option) {
    char* args[] = {const_cast<char*>(executable), const_cast<char*>(option), NULL};
    return capture(executable, args, option);
}

}
