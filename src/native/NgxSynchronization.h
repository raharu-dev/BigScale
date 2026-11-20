// Process-wide serialisation for NGX calls.
//
// NVIDIA's SDK keeps global state per graphics API inside its loaded module, and both the D3D11
// super-resolution backend and the D3D12 frame-generation worker enter it. Serialising every
// stateful entry behind one recursive mutex prevents an initialization or shutdown in one API from
// racing a feature evaluation in the other. The mutex is never held across a GPU wait or present.
#pragma once

#include <mutex>

namespace bigscale
{
std::recursive_mutex& NgxMutex();
}
