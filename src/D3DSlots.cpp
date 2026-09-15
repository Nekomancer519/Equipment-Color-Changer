// Separate translation unit: the SDK's C COM layout is the source of truth.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11.h>
#include "D3DSlots.h"
namespace ER {
const D3DSlots& SdkD3DSlots() {
    static constexpr D3DSlots slots{
        offsetof(ID3D11DeviceVtbl,CreatePixelShader)/sizeof(void*), {
        offsetof(ID3D11DeviceContextVtbl,DrawIndexed)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,Draw)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,DrawIndexedInstanced)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,DrawInstanced)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,DrawAuto)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,DrawIndexedInstancedIndirect)/sizeof(void*),
        offsetof(ID3D11DeviceContextVtbl,DrawInstancedIndirect)/sizeof(void*)}};
    static_assert(slots.createPixelShader==15);
    static_assert(slots.draws==std::array<std::size_t,7>{12,13,20,21,38,39,40});
    return slots;
}
}
