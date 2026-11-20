// mfgunlock/midpoint.hpp includes <include/reshade.hpp> but never references a ReShade symbol, so
// this spike supplies an empty header instead of vendoring the ReShade SDK. Nothing else from the
// addon is included, and any future ReShade use in that header surfaces as a compile error here
// rather than silently linking the addon runtime.
#pragma once
