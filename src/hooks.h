#pragma once
#include "il2cpp_api.h"

namespace hooks {

bool init();

// Inline-hooks `target`. `original` receives a callable trampoline.
bool install(const char* label, void* target, void* detour, void** original);

// Hooks a managed method's compiled code. Refuses when the code is shared with other methods (identical-code
// folding), because the detour would then also run for unrelated methods.
bool install_method(const char* label, const MethodInfo* method, void* detour, void** original);

}  // namespace hooks
