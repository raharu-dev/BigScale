// Runtime patch transactions for the shipped NVIDIA Streamline stack.
//
// The MFGAdaUnlock profiles are byte-exact: every site is validated against the module's timestamp,
// image size and executable hash before a single byte is written, and a module without an exact
// profile is left untouched. Nothing is restored: Streamline and the NGX provider are process
// resident, so the patched image must stay consistent until process exit.
#pragma once

#include <windows.h>

#include <string>

namespace bigscale::streamlinepatch
{
// Installs the module-load detours. Must run before Run_Loads sl.interposer.dll and before any
// feature plugin is mapped. Idempotent.
bool Install(std::string& reason);

// Applies every enabled transaction to a module that just mapped, when it is one of the two the
// profiles cover. Safe to call for unrelated modules. `midpoint` also redirects the interpolation
// kernel so generated frames land at i/(N+1) instead of the midpoint.
bool ApplyToModule(HMODULE module, bool midpoint, std::string& reason);

// True once at least one provider copy received the architecture gates.
bool ProviderGated();

// Last failure recorded by a detour, for the provider's refusal reason.
std::string LastFailure();
}
