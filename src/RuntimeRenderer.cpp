#include "RuntimeRenderer.h"
#include "ShaderPatch.h"
#include "D3DSlots.h"
#include "DrawInterception.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>

namespace ER {
namespace {
using Microsoft::WRL::ComPtr;
// Shader-owned private data follows the COM object's lifetime, avoiding a global
// pointer cache and pointer-reuse bugs. No textures are allocated or written.
const GUID bytecodeGuid{0x03d18924,0x72b0,0x4412,{0xb8,0x42,0x3e,0x91,0x64,0x87,0xe1,0x05}};
const GUID variantGuid{0x03d18925,0x72b0,0x4412,{0xb8,0x42,0x3e,0x91,0x64,0x87,0xe1,0x05}};
const GUID slotGuid{0x03d18926,0x72b0,0x4412,{0xb8,0x42,0x3e,0x91,0x64,0x87,0xe1,0x05}};
const GUID failedGuid{0x03d18927,0x72b0,0x4412,{0xb8,0x42,0x3e,0x91,0x64,0x87,0xe1,0x05}};
std::atomic<std::shared_ptr<const RenderBindings>> bindings;
std::atomic<bool> hasBindings{false}, installed{false}, captureReady{false};
std::atomic<unsigned> captured{0}, variants{0};
std::atomic<bool> drawHooksInvalid{false};
ComPtr<ID3D11DeviceContext> hookedContext;

std::mutex deviceHookMutex;
using CreatePS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
CreatePS originalCreatePS{};
void** hookedDeviceTable{};
bool HookContext(ID3D11DeviceContext* context);

HRESULT STDMETHODCALLTYPE CapturePixelShader(ID3D11Device* device,const void* code,SIZE_T size,
    ID3D11ClassLinkage* linkage,ID3D11PixelShader** output) {
    const auto result=originalCreatePS(device,code,size,linkage,output);
    if(SUCCEEDED(result) && output && *output && code && size>=32 && size<=1024*1024 && !linkage) {
        // The D3D11 object takes its own copy; no pointer into the game loader is retained.
        if(SUCCEEDED((*output)->SetPrivateData(bytecodeGuid,static_cast<UINT>(size),code))) ++captured;
    }
    return result;
}

bool Executable(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if(!address || !VirtualQuery(address,&info,sizeof(info)) || info.State!=MEM_COMMIT) return false;
    return !(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
void HookDevice(ID3D11Device* device) {
    if(!device) return;
    std::lock_guard lock(deviceHookMutex);
    auto** table=*reinterpret_cast<void***>(device);
    if(table==hookedDeviceTable) return;
    if(hookedDeviceTable) { logger::warn("A second D3D11 device implementation was ignored."); return; }
    const auto slot=SdkD3DSlots().createPixelShader;
    if(!Executable(table[slot])) { logger::error("Invalid CreatePixelShader vtable target."); return; }
    originalCreatePS=reinterpret_cast<CreatePS>(table[slot]);
    REL::safe_write(reinterpret_cast<std::uintptr_t>(&table[slot]),reinterpret_cast<std::uintptr_t>(&CapturePixelShader));
    hookedDeviceTable=table;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    captureReady=HookContext(context.Get());
    logger::info("D3D11 pixel shader capture installed.");
}
using CreateDeviceSwap=decltype(&D3D11CreateDeviceAndSwapChain);
using CreateDeviceOnly=decltype(&D3D11CreateDevice);
CreateDeviceSwap originalCreateDeviceSwap{};
CreateDeviceOnly originalCreateDeviceOnly{};
HRESULT WINAPI CreateDeviceSwapHook(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT levelCount,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swap,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context) {
    const auto result=originalCreateDeviceSwap(adapter,driver,software,flags,levels,levelCount,sdk,desc,swap,device,level,context);
    if(SUCCEEDED(result) && device) HookDevice(*device);
    return result;
}
HRESULT WINAPI CreateDeviceHook(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT levelCount,UINT sdk,ID3D11Device** device,
    D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context) {
    const auto result=originalCreateDeviceOnly(adapter,driver,software,flags,levels,levelCount,sdk,device,level,context);
    if(SUCCEEDED(result) && device) HookDevice(*device);
    return result;
}

ComPtr<ID3D11PixelShader> Variant(ID3D11Device* device,ID3D11PixelShader* original,unsigned& slot) {
    ComPtr<ID3D11PixelShader> replacement;
    UINT size=sizeof(ID3D11PixelShader*);
    if(SUCCEEDED(original->GetPrivateData(variantGuid,&size,replacement.GetAddressOf())) && replacement) {
        size=sizeof(slot); if(SUCCEEDED(original->GetPrivateData(slotGuid,&size,&slot))) return replacement;
        return {};
    }
    unsigned failed=0;size=sizeof(failed);
    if(SUCCEEDED(original->GetPrivateData(failedGuid,&size,&failed)) && failed) return {};
    auto fail=[&](const std::string& reason) {
        failed=1;original->SetPrivateData(failedGuid,sizeof(failed),&failed);
        logger::warn("Diffuse shader left unchanged: {}",reason);
    };
    size=0;original->GetPrivateData(bytecodeGuid,&size,nullptr);
    if(!size || size>1024*1024) { fail("source bytecode was not captured; restart Skyrim with this version enabled");return {}; }
    std::vector<std::uint8_t> code(size);
    if(FAILED(original->GetPrivateData(bytecodeGuid,&size,code.data()))) {fail("bytecode read failed");return {};}
    const auto patched=PatchDiffuseShader(code);
    if(patched.bytes.empty()) {fail(patched.error);return {};}
    // Bypass our capture hook for our own variant. One variant serves every color.
    const auto hr=originalCreatePS(device,patched.bytes.data(),patched.bytes.size(),nullptr,&replacement);
    if(FAILED(hr)) {fail(std::format("D3D11 rejected the variant (0x{:08X})",static_cast<unsigned>(hr)));return {};}
    slot=patched.constantSlot;
    if(FAILED(original->SetPrivateData(slotGuid,sizeof(slot),&slot)) ||
        FAILED(original->SetPrivateDataInterface(variantGuid,replacement.Get()))) {
        fail("could not retain shader variant");return {};
    }
    ++variants;
    logger::info("Diffuse shader variant ready: {} sample(s), constant slot {}.",patched.samples,slot);
    return replacement;
}

struct ActiveDraw {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11PixelShader> original;
    ComPtr<ID3D11Buffer> previousConstants, tintConstants;
    ComPtr<ID3D11Device> device;
    unsigned slot{};
    void Restore() {
        if(!original || !context) return;
        context->PSSetShader(original.Get(),nullptr,0);
        auto* buffer=previousConstants.Get();context->PSSetConstantBuffers(slot,1,&buffer);
        original.Reset();previousConstants.Reset();context.Reset();
    }
};
thread_local ActiveDraw draw;
thread_local std::shared_ptr<const RenderBindings> selectedBindings;
thread_local const RenderBinding* selected{};

using GeometryFn=void(*)(RE::BSLightingShader*,RE::BSRenderPass*,std::uint32_t);
GeometryFn originalSetup{},originalRestore{};
void SetupGeometry(RE::BSLightingShader* shader,RE::BSRenderPass* pass,std::uint32_t flags) {
    selected=nullptr;selectedBindings.reset();
    originalSetup(shader,pass,flags);
    if(!captureReady.load() || !hasBindings.load(std::memory_order_relaxed) || !pass) return;
    auto snapshot=bindings.load(); if(!snapshot) return;
    auto it=snapshot->find(pass->geometry);
    if(it==snapshot->end() || it->second.property.get()!=pass->shaderProperty || it->second.color.strength==0) return;
    const auto hooks=EnsureDrawInterception(hookedContext.Get());
    if(!hooks.ready) {
        if(!drawHooksInvalid.exchange(true)) logger::error("Cannot intercept active draw implementation: {}",hooks.error);
        return;
    }
    drawHooksInvalid=false;
    if(hooks.added) logger::info("Draw dispatch transition handled: {} new code detours ({} total); context table untouched.",hooks.added,DrawImplementationCount());
    selected=&it->second;selectedBindings=std::move(snapshot);
}
void BeginTint(ID3D11DeviceContext* context,const RenderBinding& target) {
    try {
        if(!context) return;
        ComPtr<ID3D11PixelShader> original;
        UINT classes=0;context->PSGetShader(&original,nullptr,&classes);
        if(!original || classes) {target.feedback->unsupported=true;return;}
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        unsigned slot=0;
        auto replacement=Variant(device.Get(),original.Get(),slot);
        if(!replacement) {target.feedback->unsupported=true;return;}
        if(draw.device.Get()!=device.Get()) {draw.tintConstants.Reset();draw.device=device;}
        if(!draw.tintConstants) {
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DYNAMIC;
            desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            if(FAILED(device->CreateBuffer(&desc,nullptr,&draw.tintConstants))) {target.feedback->unsupported=true;return;}
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(context->Map(draw.tintConstants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {target.feedback->unsupported=true;return;}
        const auto params=TintParameters(target.color);
        std::memcpy(mapped.pData,params.data(),sizeof(params));context->Unmap(draw.tintConstants.Get(),0);
        draw.context=context;draw.original=original;draw.slot=slot;
        context->PSGetConstantBuffers(slot,1,&draw.previousConstants);
        auto* buffer=draw.tintConstants.Get();context->PSSetConstantBuffers(slot,1,&buffer);
        context->PSSetShader(replacement.Get(),nullptr,0);
        if(target.feedback->passes.fetch_add(1,std::memory_order_relaxed)==0)
            logger::info("Equipment diffuse tint submitted: RGB ({:.3f}, {:.3f}, {:.3f}), strength {:.3f}, brightness {:.3f}.",
                target.color.rgb[0],target.color.rgb[1],target.color.rgb[2],target.color.strength,target.color.brightness);
    } catch(const std::exception& e) {
        draw.Restore();
        if(!target.feedback->unsupported.exchange(true)) logger::error("Runtime tint: {}",e.what());
    }
}
void RestoreGeometry(RE::BSLightingShader* shader,RE::BSRenderPass* pass,std::uint32_t flags) {
    selected=nullptr;selectedBindings.reset();originalRestore(shader,pass,flags);
}
// Code-entry detours surround the actual draw, including skinned partitions.
// The shared depth guard in DrawInterception prevents nested implementation
// wrappers from applying the tint twice.
void BeforeDraw(ID3D11DeviceContext* context) {
    if(selected) BeginTint(context,*selected);
}
void AfterDraw(ID3D11DeviceContext*) {draw.Restore();}
bool HookContext(ID3D11DeviceContext* context) {
    if(!context) return false;
    hookedContext=context;
    const auto result=InstallDrawInterception(context,&BeforeDraw,&AfterDraw);
    if(!result.ready) {logger::error("D3D draw interception: {}",result.error);return false;}
    logger::info("D3D code-entry interception ready: {} implementations; context vtable is not modified.",DrawImplementationCount());
    return true;
}
}

bool InstallRenderer() {
    if(installed.load()) return true;
    static REL::Relocation<std::uintptr_t> table{RE::VTABLE_BSLightingShader[0]};
    const auto* slots=reinterpret_cast<std::uintptr_t*>(table.address());
    if(!Executable(reinterpret_cast<void*>(slots[6])) || !Executable(reinterpret_cast<void*>(slots[7]))) {
        logger::error("Lighting shader virtual functions failed validation.");return false;
    }
    // Hook device creation before the game's renderer builds its shaders. Both
    // documented D3D11 entry points are handled, chaining the existing import.
    // Explicitly select Skyrim's import table.
    const auto game=reinterpret_cast<REX::W32::HMODULE>(GetModuleHandleW(nullptr));
    const auto swapIAT=SKSE::GetIATAddr(game,"d3d11.dll","D3D11CreateDeviceAndSwapChain");
    // This runtime imports CreateDeviceAndSwapChain. The alternative is optional.
    const auto deviceIAT=swapIAT?0:SKSE::GetIATAddr(game,"d3d11.dll","D3D11CreateDevice");
    if(!swapIAT && !deviceIAT) {logger::error("No D3D11 creation import found.");return false;}
    if(swapIAT) {
        originalCreateDeviceSwap=reinterpret_cast<CreateDeviceSwap>(*reinterpret_cast<std::uintptr_t*>(swapIAT));
        REL::safe_write(swapIAT,reinterpret_cast<std::uintptr_t>(&CreateDeviceSwapHook));
    }
    if(deviceIAT) {
        originalCreateDeviceOnly=reinterpret_cast<CreateDeviceOnly>(*reinterpret_cast<std::uintptr_t*>(deviceIAT));
        REL::safe_write(deviceIAT,reinterpret_cast<std::uintptr_t>(&CreateDeviceHook));
    }
    originalSetup=reinterpret_cast<GeometryFn>(table.write_vfunc(6,&SetupGeometry));
    originalRestore=reinterpret_cast<GeometryFn>(table.write_vfunc(7,&RestoreGeometry));
    installed=true;
    logger::info("Per-geometry diffuse rendering hooks installed (Skyrim 1.6.1170).");
    return true;
}
void PublishBindings(std::shared_ptr<const RenderBindings> value) {
    const bool any=value && !value->empty();bindings.store(std::move(value));hasBindings.store(any);
}
#ifdef ER_RENDER_TEST
void TestHookDevice(ID3D11Device* device) {HookDevice(device);}
void TestSelect(const RenderBinding* binding) {
    const auto result=EnsureDrawInterception(hookedContext.Get());
    if(!result.ready) throw std::runtime_error(result.error);
    selected=binding;
}
#endif
bool RendererReady() {return installed.load() && captureReady.load();}
std::string RendererStatus() {
    if(!installed.load()) return "Renderer hook unavailable; see EquipmentColorRuntime.log.";
    if(drawHooksInvalid.load()) return "Active draw implementation could not be intercepted. See EquipmentColorRuntime.log.";
    if(!captureReady.load()) return "Waiting for D3D11 shader capture; restart Skyrim if this persists.";
    return std::format("Renderer ready | {} captured shaders | {} recolor shader variants | {} draw implementations | no generated textures",
        captured.load(),variants.load(),DrawImplementationCount());
}
}
