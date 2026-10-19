#include "local_aot_process.hpp"
#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace jfc::local_aot {
namespace {
void parent_died(int) {
    kill(-getpgrp(), SIGKILL);
    _exit(126);
}
}

pid_t spawn_owned_process(char* const argv[], char* const environment[],
    const char* directory, int log_descriptor) {
    const auto owner = getpid();
    const auto supervisor = fork();
    if (supervisor < 0) return -1;
    if (supervisor == 0) {

        if (setpgid(0, 0) != 0) _exit(126);
        struct sigaction action{};
        action.sa_handler = parent_died;
        sigemptyset(&action.sa_mask);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, SIGTERM);
        if (sigaction(SIGTERM, &action, nullptr) != 0 ||
            sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) != 0 ||
            prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
        if (getppid() != owner) parent_died(SIGTERM);
        const auto tool = fork();
        if (tool < 0) _exit(126);
        if (tool == 0) {
            action.sa_handler = SIG_DFL;
            if (sigaction(SIGTERM, &action, nullptr) != 0 || chdir(directory) != 0 ||
                dup2(log_descriptor, STDOUT_FILENO) < 0 || dup2(log_descriptor, STDERR_FILENO) < 0) _exit(126);
            if (log_descriptor > STDERR_FILENO) close(log_descriptor);
            execve(argv[0], argv, environment);
            _exit(127);
        }
        int status = 0;
        while (waitpid(tool, &status, 0) < 0) {
            if (errno != EINTR) _exit(126);
        }
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    }

    if (setpgid(supervisor, supervisor) != 0 && errno != ESRCH) {
        const auto error = errno;
        kill(-supervisor, SIGKILL);
        kill(supervisor, SIGKILL);
        while (waitpid(supervisor, nullptr, 0) < 0 && errno == EINTR) {}
        errno = error;
        return -1;
    }
    return supervisor;
}
}
#endif
