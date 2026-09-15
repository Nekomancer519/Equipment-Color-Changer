#include "ShaderPatch.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
void ComputeHashRetail(const BYTE*, UINT, BYTE*);

namespace ER {
namespace {
using Words = std::vector<std::uint32_t>;
using Microsoft::WRL::ComPtr;
// SM4/5 token encoding from Microsoft's d3d12TokenizedProgramFormat.hpp.
constexpr unsigned Sample=69, SampleL=72, SampleD=73, SampleB=74;
constexpr unsigned DclCB=89, DclTemps=104, Custom=53;
constexpr unsigned Dxbc=0x43425844, Shdr=0x52444853, Shex=0x58454853;
unsigned Op(unsigned opcode, unsigned length) { return opcode | length<<24; }
unsigned Dst(unsigned mask) { return 2 | mask<<4 | 1<<20; }
unsigned Src(unsigned swizzle=0xE4) { return 2 | 1<<2 | swizzle<<4 | 1<<20; }
unsigned CB(unsigned swizzle=0xE4) { return 2 | 1<<2 | swizzle<<4 | 8<<12 | 2<<20; }
unsigned Read(std::span<const std::uint8_t> b, std::size_t offset) {
    if (offset > b.size() || b.size()-offset < 4) throw std::runtime_error("Truncated DXBC");
    unsigned value; std::memcpy(&value,b.data()+offset,4); return value;
}
// Skip a tokenized operand, including relative indexing and operand extensions.
std::size_t OperandEnd(const Words& w, std::size_t p, std::size_t end, unsigned depth=0) {
    if(p>=end || depth>8) throw std::runtime_error("Invalid operand");
    const auto token=w[p++]; auto ext=token;
    while(ext>>31) { if(p>=end) throw std::runtime_error("Invalid extended operand"); ext=w[p++]; }
    unsigned type=(token>>12)&255, dimensions=(token>>20)&3;
    if(type==4 || type==5) {
        unsigned count=(token&3)==1?1:4; p+=count*(type==5?2:1);
    }
    for(unsigned i=0;i<dimensions;++i) {
        unsigned mode=(token>>(22+3*i))&7;
        if(mode==0 || mode==3) ++p;
        else if(mode==1 || mode==4) p+=2;
        else if(mode!=2) throw std::runtime_error("Unsupported operand index");
        if(mode>=2) p=OperandEnd(w,p,end,depth+1);
    }
    if(p>end) throw std::runtime_error("Operand exceeds instruction");
    return p;
}
void AppendTint(Words& out, unsigned reg, unsigned scratch, unsigned cb) {
    // value=max(r,g,b); rgb=saturate(rgb*(1-strength)+value*color*brightness*strength).
    const Words code={
        Op(52,7),Dst(1),scratch,Src(0),reg,Src(0x55),reg,
        Op(52,7),Dst(1),scratch,Src(0),scratch,Src(0xAA),reg,
        Op(56,8),Dst(7),scratch,Src(0),scratch,CB(),cb,0,
        Op(50,10)|(1<<13),Dst(7),reg,Src(),reg,CB(0xFF),cb,0,Src(),scratch
    };
    out.insert(out.end(),code.begin(),code.end());
}
}

PatchedShader PatchDiffuseShader(std::span<const std::uint8_t> input) {
    PatchedShader result;
    try {
        if(input.size()>1024*1024 || Read(input,0)!=Dxbc || Read(input,24)!=input.size())
            throw std::runtime_error("Invalid DXBC container");
        const auto count=Read(input,28);
        if(!count || count>64 || 32ull+count*4>input.size()) throw std::runtime_error("Invalid chunk table");
        std::vector<Words> chunks;
        bool found=false;
        for(unsigned ci=0;ci<count;++ci) {
            auto offset=Read(input,32+ci*4), tag=Read(input,offset), size=Read(input,offset+4);
            if(size%4 || std::uint64_t(offset)+8+size>input.size()) throw std::runtime_error("Invalid chunk bounds");
            Words chunk(2+size/4); std::memcpy(chunk.data(),input.data()+offset,size+8);
            if(tag!=Shdr && tag!=Shex) {
                // Reflection/statistics/debug metadata would describe the original code.
                if(tag==0x4E475349 || tag==0x4E47534F || tag==0x3547534F || tag==0x31475349 || tag==0x3147534F || tag==0x30494653)
                    chunks.push_back(std::move(chunk));
                continue;
            }
            if(found) throw std::runtime_error("Multiple shader programs");
            found=true;
            Words w(chunk.begin()+2,chunk.end());
            if(w.size()<2 || w[1]!=w.size() || (w[0]>>16)!=0 || (w[0]&255)>0x50)
                throw std::runtime_error("Expected SM4/SM5.0 pixel shader");
            std::array<bool,14> used{}; unsigned temps=0; std::size_t tempAt=0;
            struct SampleSite { std::size_t end; unsigned reg; };
            std::vector<SampleSite> sites;
            for(std::size_t p=2;p<w.size();) {
                auto opcode=w[p]&2047, len=(w[p]>>24)&127;
                if(opcode==Custom) { if(p+1>=w.size()) throw std::runtime_error("Truncated custom data"); len=w[p+1]; }
                if(!len || p+len>w.size()) throw std::runtime_error("Invalid instruction length");
                const auto end=p+len;
                if(opcode==DclTemps) { if(len!=2) throw std::runtime_error("Invalid temp declaration"); temps=w[p+1]; tempAt=p; }
                if(opcode==DclCB) {
                    if(len!=4 || ((w[p+1]>>20)&3)!=2 || w[p+2]>=used.size()) throw std::runtime_error("Unsupported constant declaration");
                    used[w[p+2]]=true;
                }
                if(opcode==Sample || opcode==SampleL || opcode==SampleD || opcode==SampleB) {
                    auto q=p+1; auto extended=w[p];
                    while(extended>>31) { if(q>=end) throw std::runtime_error("Invalid sample extension"); extended=w[q++]; }
                    auto dest=q; q=OperandEnd(w,q,end); q=OperandEnd(w,q,end); auto resource=q;
                    auto resourceEnd=OperandEnd(w,q,end);
                    if(resourceEnd-resource==2 && ((w[resource]>>12)&255)==7 && w[resource+1]==0) {
                        // Accept RGB(A) destination masks and identity resource swizzle only.
                        if(((w[dest]>>4)&7)==0) { p=end; continue; }
                        if(w[dest]!=Dst(15) && w[dest]!=Dst(7)) throw std::runtime_error("Diffuse sample uses unsupported destination mask");
                        if(w[resource]!=(Src() | 7<<12)) throw std::runtime_error("Diffuse sample uses nonidentity resource swizzle");
                        sites.push_back({end,w[dest+1]});
                    }
                }
                p=end;
            }
            if(!temps || temps>=4096 || !tempAt || sites.empty()) throw std::runtime_error("No supported diffuse RGB sample");
            unsigned slot=7;
            while(slot>0 && used[slot]) --slot;
            if(used[slot]) throw std::runtime_error("No available constant buffer slot");
            result.constantSlot=slot; result.samples=static_cast<unsigned>(sites.size());
            Words out{w[0],0}; std::size_t site=0;
            for(std::size_t p=2;p<w.size();) {
                auto len=(w[p]>>24)&127; if((w[p]&2047)==Custom) len=w[p+1];
                out.insert(out.end(),w.begin()+p,w.begin()+p+len);
                if(p==tempAt) {
                    out.back()=temps+1;
                    out.insert(out.end(),{Op(DclCB,4),CB(),slot,1});
                }
                p+=len;
                if(site<sites.size() && sites[site].end==p) { AppendTint(out,sites[site].reg,temps,slot); ++site; }
            }
            out[1]=static_cast<unsigned>(out.size());
            chunk={tag,static_cast<unsigned>(out.size()*4)}; chunk.insert(chunk.end(),out.begin(),out.end());
            chunks.push_back(std::move(chunk));
        }
        if(!found) throw std::runtime_error("No shader program chunk");
        Words container{Dxbc,0,0,0,0,1,0,static_cast<unsigned>(chunks.size())};
        container.resize(8+chunks.size());
        for(unsigned i=0;i<chunks.size();++i) { container[8+i]=static_cast<unsigned>(container.size()*4); container.insert(container.end(),chunks[i].begin(),chunks[i].end()); }
        container[6]=static_cast<unsigned>(container.size()*4);
        // DXBC hashes everything starting at byte 20 with Microsoft's container hash.
        auto* data=reinterpret_cast<std::uint8_t*>(container.data());
        ComputeHashRetail(data+20,static_cast<unsigned>(container.size()*4-20),data+4);
        result.bytes.assign(data,data+container.size()*4);
    } catch(const std::exception& e) { result.bytes.clear(); result.error=e.what(); }
    return result;
}
}
