#include "platform_paths.h"

#include <cstdlib>

#if defined(_WIN32)
#include <cstdlib>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace psb {

std::string homeDir() {
#if defined(_WIN32)
    const char* hd = std::getenv("USERPROFILE");
    return hd ? hd : ".";
#else
    const char* hd = std::getenv("HOME");
    if (hd) return hd;
    struct passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
#endif
}

} // namespace psb
