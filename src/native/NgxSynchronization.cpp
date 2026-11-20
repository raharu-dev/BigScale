#include "NgxSynchronization.h"

namespace bigscale
{
std::recursive_mutex& NgxMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}
}
