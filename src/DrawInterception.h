#pragma once
#include <d3d11.h>
#include <string>
namespace ER {
using DrawCallback=void(*)(ID3D11DeviceContext*);
struct DrawHookResult { bool ready{}; unsigned added{}; std::string error; };
// Installs code-entry detours, NEVER writes the context's mutable vtable.
DrawHookResult InstallDrawInterception(ID3D11DeviceContext*,DrawCallback before,DrawCallback after);
// Called at the geometry boundary to cover newly selected runtime implementations.
DrawHookResult EnsureDrawInterception(ID3D11DeviceContext*);
unsigned DrawImplementationCount();
}
