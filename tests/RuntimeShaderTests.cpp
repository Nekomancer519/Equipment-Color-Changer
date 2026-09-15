#include "ShaderPatch.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <map>
#include <cstdlib>
#ifdef ER_RENDER_TEST
#include "RuntimeRenderer.h"
#include "D3DSlots.h"
#include "DrawInterception.h"
namespace ER {void TestHookDevice(ID3D11Device*);void TestSelect(const RenderBinding*);}
using TestDrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
TestDrawFn chainedOriginal{};
std::atomic<unsigned> chainedCalls{0};
__declspec(noinline) void STDMETHODCALLTYPE ChainedTestDraw(ID3D11DeviceContext* context,UINT count,UINT start) {
    ++chainedCalls;chainedOriginal(context,count,start);
}
#endif
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr,const char* text) { if(FAILED(hr)) { std::cerr<<text<<" HRESULT "<<std::hex<<hr<<"\n"; throw std::runtime_error(text); } }
ComPtr<ID3DBlob> Compile(const char* source,const char* profile) {
    ComPtr<ID3DBlob> code,errors;
    auto hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(errors) std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
    Check(hr,"Compile"); return code;
}
int main(int argc,char** argv) { try {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const bool hardware=std::getenv("ECR_TEST_HARDWARE")!=nullptr;
    Check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device");
    std::cout<<(hardware?"Hardware":"WARP")<<" D3D11 test device.\n";
#ifdef ER_RENDER_TEST
    auto** contextTable=*reinterpret_cast<void***>(context.Get());
    std::array<void*,115> before;std::copy_n(contextTable,before.size(),before.begin());
    ER::TestHookDevice(device.Get());
    for(std::size_t i=0;i<before.size();++i) {
        if(before[i]!=contextTable[i]) throw std::runtime_error("Context vtable must remain untouched");
    }
#endif
    auto vs=Compile("float4 main(uint v:SV_VertexID):SV_Position { return float4(v==2?3:-1,v==1?3:-1,0,1); }","vs_5_0");
    auto ps=Compile("Texture2D<float4> tex:register(t0); SamplerState smp:register(s0); cbuffer Existing:register(b7){float4 factor;} float4 main():SV_Target { return tex.Sample(smp,float2(.5,.5))*factor; }","ps_5_0");
    ER::PatchedShader patched=ER::PatchDiffuseShader({static_cast<const std::uint8_t*>(ps->GetBufferPointer()),ps->GetBufferSize()});
    if(!patched.error.empty()) throw std::runtime_error(patched.error);
    if(patched.samples!=1 || patched.constantSlot==7) throw std::runtime_error("Sample count/constant conflict");
    ComPtr<ID3D11PixelShader> tinted,original; ComPtr<ID3D11VertexShader> vertex;
    Check(device->CreatePixelShader(patched.bytes.data(),patched.bytes.size(),nullptr,&tinted),"Create instrumented pixel shader");
    Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&original),"Create original pixel shader");
    Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Create VS");
    D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=td.MipLevels=td.ArraySize=1; td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count=1; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const std::array<float,4> input{.2f,.4f,.8f,.35f}; D3D11_SUBRESOURCE_DATA initial{input.data(),16,0};
    ComPtr<ID3D11Texture2D> source,target,staging; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
    Check(device->CreateTexture2D(&td,&initial,&source),"Source"); Check(device->CreateShaderResourceView(source.Get(),nullptr,&srv),"SRV");
    td.BindFlags=D3D11_BIND_RENDER_TARGET; Check(device->CreateTexture2D(&td,nullptr,&target),"Target"); Check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"RTV");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(device->CreateTexture2D(&td,nullptr,&staging),"Readback");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const std::array<float,4> one{1,1,1,1};D3D11_SUBRESOURCE_DATA ones{one.data(),0,0};
    ComPtr<ID3D11Buffer> cb,existing;Check(device->CreateBuffer(&bd,nullptr,&cb),"Tint CB");Check(device->CreateBuffer(&bd,&ones,&existing),"Existing CB");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&sd,&sampler),"Sampler");
    D3D11_VIEWPORT vp{0,0,1,1,0,1}; context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);
    auto* r=rtv.Get();context->OMSetRenderTargets(1,&r,nullptr);auto* s=srv.Get();context->PSSetShaderResources(0,1,&s);auto* sm=sampler.Get();context->PSSetSamplers(0,1,&sm);
    auto* e=existing.Get();context->PSSetConstantBuffers(7,1,&e);auto* b=cb.Get();context->PSSetConstantBuffers(patched.constantSlot,1,&b);
    unsigned drawMode=0;
#ifdef ER_RENDER_TEST
    TestDrawFn testDispatch=nullptr;
#endif
    ComPtr<ID3D11Buffer> indices,indirect,indexedIndirect;
    bd.BindFlags=D3D11_BIND_INDEX_BUFFER;bd.ByteWidth=12;
    const std::array<UINT,3> indexData{1,2,3};D3D11_SUBRESOURCE_DATA indexInit{indexData.data(),0,0};
    Check(device->CreateBuffer(&bd,&indexInit,&indices),"Index buffer");
    context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R32_UINT,0);
    bd.BindFlags=0;bd.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;bd.ByteWidth=16;
    const std::array<UINT,4> args{3,1,0,0};D3D11_SUBRESOURCE_DATA argInit{args.data(),0,0};
    Check(device->CreateBuffer(&bd,&argInit,&indirect),"Indirect args");
    bd.ByteWidth=20;const std::array<UINT,5> indexedArgs{3,1,0,UINT(-1),0};argInit.pSysMem=indexedArgs.data();
    Check(device->CreateBuffer(&bd,&argInit,&indexedIndirect),"Indexed indirect args");
    auto draw=[&](ID3D11PixelShader* shader,std::array<float,4> params,std::array<float,4> expected) {
        const float clear[4]{-1,-1,-1,-1};context->ClearRenderTargetView(rtv.Get(),clear);
        context->UpdateSubresource(cb.Get(),0,nullptr,params.data(),0,0);context->PSSetShader(shader,nullptr,0);
        switch(drawMode) {
        case 0:
#ifdef ER_RENDER_TEST
            if(testDispatch) {testDispatch(context.Get(),3,0);break;}
#endif
            context->Draw(3,0);break;
        case 1:context->DrawIndexed(3,0,-1);break;
        case 2:context->DrawInstanced(3,1,0,0);break;
        case 3:context->DrawIndexedInstanced(3,1,0,-1,0);break;
        case 4:context->DrawInstancedIndirect(indirect.Get(),0);break;
        case 5:context->DrawIndexedInstancedIndirect(indexedIndirect.Get(),0);break;
        }
        context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Readback map");
        std::array<float,4> actual;std::memcpy(actual.data(),mapped.pData,16);context->Unmap(staging.Get(),0);
        for(unsigned i=0;i<4;++i) if(std::abs(actual[i]-expected[i])>.0001f) {std::cerr<<"channel "<<i<<" expected "<<expected[i]<<" actual "<<actual[i]<<"\n";throw std::runtime_error("GPU color mismatch");}
    };
    draw(tinted.Get(),{1,0,0,0},{.8f,0,0,.35f});
    draw(tinted.Get(),{0,.5f,0,.5f},{.1f,.6f,.4f,.35f});
    draw(tinted.Get(),{0,0,0,1},input);
    draw(tinted.Get(),{2,0,0,0},{1,0,0,.35f});
    draw(original.Get(),{1,0,0,0},input);
#ifdef ER_RENDER_TEST
    ER::RenderBinding selection;selection.color.rgb={1,0,0};selection.color.strength=1;selection.color.brightness=1;
    for(drawMode=0;drawMode<6;++drawMode) {
        ER::TestSelect(&selection);
        draw(original.Get(),{0,0,0,1},{.8f,0,0,.35f});
        ComPtr<ID3D11PixelShader> restored;context->PSGetShader(&restored,nullptr,nullptr);
        ComPtr<ID3D11Buffer> restoredCB;context->PSGetConstantBuffers(patched.constantSlot,1,&restoredCB);
        if(restored.Get()!=original.Get() || restoredCB.Get()!=cb.Get()) throw std::runtime_error("Draw hook leaked shader/constant state");
        ER::TestSelect(nullptr);draw(original.Get(),{0,0,0,1},input);
    }
    drawMode=0;
    ER::TestSelect(&selection);
    for(unsigned i=0;i<1000;++i) {
        ER::TestSelect(&selection);
        selection.color.rgb=i%2?std::array<float,3>{0,1,0}:std::array<float,3>{1,0,0};
        draw(original.Get(),{0,0,0,1},i%2?std::array<float,4>{0,.8f,0,.35f}:std::array<float,4>{.8f,0,0,.35f});
    }
    ER::TestSelect(nullptr);
    if(selection.feedback->unsupported || selection.feedback->passes!=1006) throw std::runtime_error("Hook feedback mismatch");
    ComPtr<ID3D11Multithread> threading;Check(context.As(&threading),"Multithread interface");
    const auto wasProtected=threading->GetMultithreadProtected();
    for(unsigned i=0;i<20;++i) {
        threading->SetMultithreadProtected(i%2);
        ER::TestSelect(&selection);
        draw(original.Get(),{0,0,0,1},{0,.8f,0,.35f});
        ER::TestSelect(nullptr);draw(original.Get(),{0,0,0,1},input);
    }
    threading->SetMultithreadProtected(wasProtected);
    if(selection.feedback->passes!=1026) throw std::runtime_error("Thread-mode transition lost interception");
    const auto drawSlot=ER::SdkD3DSlots().draws[1];
    auto** liveTable=*reinterpret_cast<void***>(context.Get());
    chainedOriginal=reinterpret_cast<TestDrawFn>(liveTable[drawSlot]);
    REL::safe_write(reinterpret_cast<std::uintptr_t>(&liveTable[drawSlot]),reinterpret_cast<std::uintptr_t>(&ChainedTestDraw));
    // A wrapper may retain its own call target even when D3D replaces a table.
    // Call this captured wrapper explicitly so driver table rewrites cannot
    // invalidate the test fixture itself.
    testDispatch=&ChainedTestDraw;
    selection.color.rgb={1,0,0};selection.color.strength=.5f;
    ER::TestSelect(&selection);draw(original.Get(),{0,0,0,1},{.5f,.2f,.4f,.35f});
    ER::TestSelect(nullptr);draw(original.Get(),{0,0,0,1},input);
    if(chainedCalls!=2 || selection.feedback->passes!=1027) throw std::runtime_error("Nested hook chain lost or double-tinted a draw");
    testDispatch=nullptr;
    if(liveTable[drawSlot]==reinterpret_cast<void*>(&ChainedTestDraw))
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&liveTable[drawSlot]),reinterpret_cast<std::uintptr_t>(chainedOriginal));
    ER::TestSelect(&selection);draw(original.Get(),{0,0,0,1},{.5f,.2f,.4f,.35f});ER::TestSelect(nullptr);
    std::cout<<"PASS: later chained implementation and cached call target, nested hooks tint once, return to runtime dispatch.\n";
    std::cout<<"PASS: production code-entry hooks, untouched context table, six draw paths, state restoration, 1000 color changes and 20 threading-mode switches; "<<ER::DrawImplementationCount()<<" draw implementations.\n";
#endif
    if(ER::PatchDiffuseShader({}).error.empty()) throw std::runtime_error("Accepted empty shader");
    auto noDiffuse=Compile("float4 main():SV_Target {return float4(1,1,1,1);}","ps_5_0");
    if(ER::PatchDiffuseShader({static_cast<const std::uint8_t*>(noDiffuse->GetBufferPointer()),noDiffuse->GetBufferSize()}).error.empty()) throw std::runtime_error("Accepted shader without diffuse sample");
    std::cout<<"PASS: actual D3D11 render/readback; red recolor, strength blending, zero strength, brightness clamp, unchanged alpha, restored original shader, constant-slot conflict, invalid input.\n";
    if(argc>1) {
        // Scan the user's uncompressed shader archive directly in memory. No
        // copyrighted game shaders are extracted or shipped with the mod.
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        if(!file) throw std::runtime_error("Cannot open shader corpus");
        const auto length=file.tellg();if(length<=0 || length>256*1024*1024) throw std::runtime_error("Shader corpus size");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),length);
        auto u32=[&](std::size_t p){std::uint32_t v;std::memcpy(&v,bytes.data()+p,4);return v;};
        std::map<std::string,unsigned> rejected;unsigned pixel=0,accepted=0,validated=0;
        for(std::size_t p=0;p+32<bytes.size();++p) {
            if(u32(p)!=0x43425844) continue;
            const auto len=u32(p+24),count=u32(p+28);
            if(len<32 || len>1024*1024 || p+len>bytes.size() || count>64 || 32ull+4*count>len) continue;
            bool isPixel=false;
            for(unsigned i=0;i<count;++i) {
                const auto off=u32(p+32+i*4);if(std::uint64_t(off)+12>len) continue;
                auto tag=u32(p+off);if((tag==0x52444853 || tag==0x58454853) && (u32(p+off+8)>>16)==0) isPixel=true;
            }
            if(!isPixel) {p+=len-1;continue;}
            ++pixel;
            auto patchedGame=ER::PatchDiffuseShader({bytes.data()+p,len});
            if(patchedGame.bytes.empty()) ++rejected[patchedGame.error];
            else {
                ++accepted;
                ComPtr<ID3D11PixelShader> checked;
                Check(device->CreatePixelShader(patchedGame.bytes.data(),patchedGame.bytes.size(),nullptr,&checked),"Game shader variant validation");
                ++validated;
            }
            p+=len-1;
        }
        std::cout<<"Game shader corpus: "<<pixel<<" pixel shaders; "<<accepted<<" patched; "<<validated<<" accepted by D3D11.\n";
        for(const auto& [why,n]:rejected) std::cout<<"Unchanged ("<<n<<"): "<<why<<"\n";
        if(!accepted) throw std::runtime_error("No game shader variants validated");
    }
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;} }
