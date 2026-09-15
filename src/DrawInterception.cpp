#include "DrawInterception.h"
#include "D3DSlots.h"
#include <MinHook.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace ER {
namespace {
std::mutex installMutex;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> trackedContext;
DrawCallback beforeDraw{},afterDraw{};
thread_local unsigned depth=0;
std::array<void*,7> lastTargets{};
DrawHookResult lastResult;
std::atomic<unsigned> implementationCount{0};
bool initialized=false;

struct Scope {
    ID3D11DeviceContext* context;
    bool outer;
    explicit Scope(ID3D11DeviceContext* c):context(c),outer(depth++==0 && c==trackedContext.Get()) {
        if(outer) beforeDraw(c);
    }
    ~Scope() {if(outer) afterDraw(context);--depth;}
};

// Each code address gets its own ABI-exact thunk and original trampoline.
// A table switch therefore never overwrites the original for an older hook.
template<unsigned Kind,class... Args> struct Method {
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,Args...);
    struct Entry {void* target{};Fn original{};};
    static inline std::array<Entry,16> entries{};
    template<std::size_t I> static void STDMETHODCALLTYPE Thunk(ID3D11DeviceContext* c,Args... args) {
        Scope scope(c);entries[I].original(c,args...);
    }
    template<std::size_t... I> static constexpr auto Thunks(std::index_sequence<I...>) {
        return std::array<Fn,sizeof...(I)>{&Thunk<I>...};
    }
    static bool Queue(void* target,DrawHookResult& result) {
        for(const auto& e:entries) if(e.target==target) return true;
        constexpr auto thunks=Thunks(std::make_index_sequence<16>{});
        for(std::size_t i=0;i<entries.size();++i) if(!entries[i].target) {
            void* original=nullptr;
            auto status=MH_CreateHook(target,reinterpret_cast<void*>(thunks[i]),&original);
            if(status!=MH_OK) {result.error="Draw method "+std::to_string(Kind)+": "+MH_StatusToString(status);return false;}
            entries[i].original=reinterpret_cast<Fn>(original);
            entries[i].target=target;
            status=MH_QueueEnableHook(target);
            if(status!=MH_OK) {result.error=MH_StatusToString(status);return false;}
            ++result.added;++implementationCount;
            return true;
        }
        result.error="Too many distinct implementations for draw method "+std::to_string(Kind);return false;
    }
};
using Indexed=Method<0,UINT,UINT,INT>;
using Direct=Method<1,UINT,UINT>;
using IndexedInstanced=Method<2,UINT,UINT,UINT,INT,UINT>;
using Instanced=Method<3,UINT,UINT,UINT,UINT>;
using Auto=Method<4>;
using IndexedIndirect=Method<5,ID3D11Buffer*,UINT>;
using Indirect=Method<6,ID3D11Buffer*,UINT>;

DrawHookResult EnsureLocked(ID3D11DeviceContext* context) {
    if(!initialized || context!=trackedContext.Get()) return {false,0,"Unexpected D3D context"};
    auto** table=*reinterpret_cast<void***>(context);
    std::array<void*,7> targets;
    for(std::size_t i=0;i<targets.size();++i) targets[i]=table[SdkD3DSlots().draws[i]];
    if(targets==lastTargets) return {lastResult.ready,0,lastResult.error};
    DrawHookResult result{true};
    // Stage all modifications first, then enable together (one thread suspension).
    result.ready=Indexed::Queue(targets[0],result) && Direct::Queue(targets[1],result) &&
        IndexedInstanced::Queue(targets[2],result) && Instanced::Queue(targets[3],result) &&
        Auto::Queue(targets[4],result) && IndexedIndirect::Queue(targets[5],result) && Indirect::Queue(targets[6],result);
    if(result.ready && result.added) {
        // D3D has both clean-state and dirty-state draw entry points. Merely
        // observing a new clean entry misses its dirty partner on the next bind.
        // Rebind the EXACT current pixel shader (including all class instances)
        // to expose that partner without drawing or changing logical GPU state.
        Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
        std::array<ID3D11ClassInstance*,256> instances{};
        UINT count=static_cast<UINT>(instances.size());
        context->PSGetShader(&shader,instances.data(),&count);
        context->PSSetShader(shader.Get(),instances.data(),count);
        for(auto* instance:instances) if(instance) instance->Release();
        table=*reinterpret_cast<void***>(context);
        for(std::size_t i=0;i<targets.size();++i) targets[i]=table[SdkD3DSlots().draws[i]];
        result.ready=Indexed::Queue(targets[0],result) && Direct::Queue(targets[1],result) &&
            IndexedInstanced::Queue(targets[2],result) && Instanced::Queue(targets[3],result) &&
            Auto::Queue(targets[4],result) && IndexedIndirect::Queue(targets[5],result) && Indirect::Queue(targets[6],result);
    }
    if(result.added) {
        const auto status=MH_ApplyQueued();
        if(status!=MH_OK) {result.ready=false;result.error=MH_StatusToString(status);}
    }
    lastTargets=targets;lastResult=result;
    return result;
}
}
DrawHookResult InstallDrawInterception(ID3D11DeviceContext* context,DrawCallback before,DrawCallback after) {
    std::lock_guard lock(installMutex);
    if(!context || !before || !after) return {false,0,"Missing draw context/callback"};
    if(!initialized) {
        const auto status=MH_Initialize();
        if(status!=MH_OK) return {false,0,MH_StatusToString(status)};
        trackedContext=context;beforeDraw=before;afterDraw=after;initialized=true;
    }
    return EnsureLocked(context);
}
DrawHookResult EnsureDrawInterception(ID3D11DeviceContext* context) {
    std::lock_guard lock(installMutex);
    return EnsureLocked(context);
}
unsigned DrawImplementationCount() {return implementationCount.load();}
}
