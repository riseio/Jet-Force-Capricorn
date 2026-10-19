#pragma once
#ifdef __linux__
#include <sys/types.h>

namespace jfc::local_aot {

pid_t spawn_owned_process(char* const argv[], char* const environment[],
    const char* directory, int log_descriptor);
}
#endif
