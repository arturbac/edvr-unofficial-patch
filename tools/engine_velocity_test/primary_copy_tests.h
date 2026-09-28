#pragma once
#include "../../src/d3d11/engine_velocity_primary_copy.h"
#include <cstring>

namespace primary_copy_tests {
namespace copy=edvr::engine_velocity_primary_copy;
namespace emit=edvr::engine_velocity_emit;
using Microsoft::WRL::ComPtr;
using Record=std::array<uint32_t,84>;
inline Record record(float x) {
    Record r{};r.fill(0x12345678u);r[0]=0;r[1]=r[77]=0x3F800000u;
    r[2]=r[78]=0x7FFF7FFFu;r[3]=r[79]=0xFFFE7FFFu;r[72]=0;
    const float p[3]={x,2,3};std::memcpy(&r[4],p,12);std::memcpy(&r[73],p,12);return r;
}
inline emit::Pose previous(const Record& r) {auto p=copy::current(r.data());float x=0;std::memcpy(&x,&p.w[0],4);x-=1;std::memcpy(&p.w[0],&x,4);return p;}
inline bool claim(Record& r,uint32_t frame) {const auto p=previous(r);return copy::recordEmission(reinterpret_cast<uintptr_t>(r.data()),r.data(),p,emit::kJoined^emit::markerHash(copy::current(r.data()),p,frame),frame);}
struct Node { alignas(16) unsigned char data[32+8*336+64]{};
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(data);}
    Record& item(unsigned i){return *reinterpret_cast<Record*>(data+32+i*336);}
    void links(uintptr_t anchor,uint64_t n){uint64_t h[4]={anchor,anchor,0,n};std::memcpy(data,h,32);}
};
struct Dictionary {
    alignas(16) unsigned char data[48]{},entry[56]{};uintptr_t bucket=0;
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(data);}
    uintptr_t anchor(){return reinterpret_cast<uintptr_t>(entry)+0x28;}
    void set(Node* node){
        const uintptr_t bp=reinterpret_cast<uintptr_t>(&bucket);const uint64_t count=1;
        std::memcpy(data+0x10,&bp,8);std::memcpy(data+0x18,&count,8);
        bucket=reinterpret_cast<uintptr_t>(entry);std::memcpy(entry,&bp,8);
        const uintptr_t p=node?node->ptr():anchor();std::memcpy(entry+0x28,&p,8);std::memcpy(entry+0x30,&p,8);
        if(node)node->links(anchor(),1);
    }
};
inline void run(ID3D11Device* device,ID3D11DeviceContext* ctx,void (*check)(bool,const char*)) {
    copy::reset();const uint32_t frame=42;
    Record src[2]={record(10),record(20)},mapped[2]={src[0],src[1]};
    D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(src);d.StructureByteStride=336;
    d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{src,0,0};ComPtr<ID3D11Buffer> native,privatePool;
    check(SUCCEEDED(device->CreateBuffer(&d,&initial,&native)),"primary copy: native pool fixture");
    d.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
    check(SUCCEEDED(device->CreateBuffer(&d,&initial,&privatePool)),"primary copy: private pool fixture");
    if(!native || !privatePool)return;
    auto begin=[&](uint64_t seq,D3D11_MAP type=D3D11_MAP_WRITE_DISCARD,uint32_t tick=42){
        return copy::beginMap(native.Get(),mapped,sizeof(mapped),336,type,seq,tick);
    };
    auto forward=[&](Record* source,uint32_t first,uint32_t count,uint32_t tick=42){
        copy::copier(reinterpret_cast<uintptr_t>(mapped),336,reinterpret_cast<uintptr_t>(source),first,count,tick);
    };
    check(claim(src[0],frame) && claim(src[1],frame),"primary copy: two independently certified source addresses");
    check(begin(1),"primary copy: discard opens exact mapped resource lease");forward(src,0,2);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: active mapped GPU source never scattered");
    copy::endMap(native.Get(),1);check(copy::patches(native.Get(),frame).size()==2,"primary copy: authoritative source/destination copy joins both slots");
    ctx->CopyResource(privatePool.Get(),native.Get());
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame),"primary copy: one batch scatter on private clone");
    auto read=[&](ID3D11Buffer* buffer){
        D3D11_BUFFER_DESC bd{};buffer->GetDesc(&bd);bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.MiscFlags=0;bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;device->CreateBuffer(&bd,nullptr,&staging);ctx->CopyResource(staging.Get(),buffer);
        D3D11_MAPPED_SUBRESOURCE m{};Record out[2]{};if(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m))){std::memcpy(out,m.pData,sizeof(out));ctx->Unmap(staging.Get(),0);}return std::array<Record,2>{out[0],out[1]};
    };
    const auto patched=read(privatePool.Get()),unchanged=read(native.Get());
    check(unchanged[0]==src[0] && unchanged[1]==src[1],"primary copy: native GPU bytes remain bit-identical");
    for(unsigned n=0;n<2;++n){auto expected=src[n];const auto p=previous(src[n]);expected[72]=emit::kJoined^emit::markerHash(copy::current(src[n].data()),p,frame);expected[73]=p.w[0];expected[74]=p.w[1];expected[75]=p.w[2];expected[78]=p.w[3];expected[79]=p.w[4];check(patched[n]==expected,"primary copy: only private marker and previous pose bytes change");}
    // GPU guard sees the post-CopyResource bytes, not the earlier CPU lease.
    auto mismatched=src[0];mismatched[30]^=1;Record guarded[2]={mismatched,src[1]};
    ctx->UpdateSubresource(privatePool.Get(),0,nullptr,guarded,0,0);
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame),"primary copy: guarded batch dispatched");
    check(read(privatePool.Get())[0]==mismatched,"primary copy: GPU full-record guard rejects late material overwrite");
    ComPtr<ID3D11ShaderResourceView> cloneSrv;device->CreateShaderResourceView(privatePool.Get(),nullptr,&cloneSrv);
    ctx->PSSetShaderResources(7,1,cloneSrv.GetAddressOf());
    check(!copy::apply(ctx,privatePool.Get(),native.Get(),frame) && copy::stats().lastScatterFailure==copy::ScatterFailure::Bound,
          "primary copy: active clone SRV refuses UAV scatter with counted failure");
    ID3D11ShaderResourceView* nullSrv=nullptr;ctx->PSSetShaderResources(7,1,&nullSrv);
    ComPtr<ID3D11UnorderedAccessView> cloneUav;device->CreateUnorderedAccessView(privatePool.Get(),nullptr,&cloneUav);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,cloneUav.GetAddressOf(),nullptr);
    check(!copy::apply(ctx,privatePool.Get(),native.Get(),frame),"primary copy: OM UAV alias refuses scatter");
    ID3D11UnorderedAccessView* nullUav=nullptr;ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,&nullUav,nullptr);
    ComPtr<ID3D11Buffer> other,oldCount;device->CreateBuffer(&d,&initial,&other);
    ComPtr<ID3D11ShaderResourceView> oldSrv;device->CreateShaderResourceView(native.Get(),nullptr,&oldSrv);
    ComPtr<ID3D11UnorderedAccessView> oldUav;device->CreateUnorderedAccessView(other.Get(),nullptr,&oldUav);
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;device->CreateBuffer(&cb,nullptr,&oldCount);
    ctx->CSSetShader(copy::g_gpu.shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,oldSrv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0,1,oldUav.GetAddressOf(),nullptr);ctx->CSSetConstantBuffers(0,1,oldCount.GetAddressOf());
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame),"primary copy: scatter with prior compute bindings");
    ComPtr<ID3D11ComputeShader> restoredShader;ComPtr<ID3D11ShaderResourceView> restoredSrv;
    ComPtr<ID3D11UnorderedAccessView> restoredUav;ComPtr<ID3D11Buffer> restoredCount;
    ctx->CSGetShader(&restoredShader,nullptr,nullptr);ctx->CSGetShaderResources(0,1,&restoredSrv);
    ctx->CSGetUnorderedAccessViews(0,1,&restoredUav);ctx->CSGetConstantBuffers(0,1,&restoredCount);
    check(restoredShader.Get()==copy::g_gpu.shader.Get() && restoredSrv.Get()==oldSrv.Get() && restoredUav.Get()==oldUav.Get() && restoredCount.Get()==oldCount.Get(),
          "primary copy: compute shader/SRV/UAV/constants restored exactly");
    ctx->CSSetShader(nullptr,nullptr,0);ctx->CSSetShaderResources(0,1,&nullSrv);ctx->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);
    ID3D11Buffer* nullBuffer=nullptr;ctx->CSSetConstantBuffers(0,1,&nullBuffer);
    check(begin(2,D3D11_MAP_WRITE_NO_OVERWRITE),"primary copy: append lease opens");forward(src,0,1);copy::endMap(native.Get(),2);
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: consumed source cannot recertify overwritten slot; disjoint append retained");
    ctx->CopyResource(privatePool.Get(),native.Get());copy::apply(ctx,privatePool.Get(),native.Get(),frame);
    check(read(privatePool.Get())[0]==src[0] && read(privatePool.Get())[1]==patched[1],
          "primary copy: partial dispatch uses current row count; cached extra rows cannot repatch revoked slots");
    check(begin(3),"primary copy: discard resets all certificates");copy::endMap(native.Get(),3);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: discard does not carry prior slot ownership");
    claim(src[0],frame);mapped[0][8]^=1;begin(4);forward(src,0,1);copy::endMap(native.Get(),4);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: full destination mismatch refuses same-pose material aliases");mapped[0]=src[0];
    claim(src[0],frame);begin(5);src[0][9]^=1;forward(src,0,1);copy::endMap(native.Get(),5);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: source mutation/reuse invalidates pending exact emission");src[0]=mapped[0];
    claim(src[0],frame);begin(6,D3D11_MAP_WRITE_NO_OVERWRITE,frame+1);forward(src,0,1,frame+1);copy::endMap(native.Get(),6);
    check(copy::patches(native.Get(),frame+1).empty(),"primary copy: older emission cannot become current map history");
    claim(src[0],frame);begin(7);forward(src,0,1);copy::invalidateMapped(reinterpret_cast<uintptr_t>(mapped));copy::endMap(native.Get(),7);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: uncertain copier metadata invalidates entire mapped lease");
    claim(src[0],frame);check(!begin(8,D3D11_MAP_WRITE),"primary copy: arbitrary whole-buffer writes refuse ownership");forward(src,0,1);copy::endMap(native.Get(),8);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: unknown map writes cannot leak previous certificates");
    check(!begin(8),"primary copy: replayed map sequence refused");
    claim(src[0],frame);begin(9);forward(src,0,1);forward(src,0,0);
    check(copy::g_pools.at(native.Get()).patches.size()==1,"primary copy: zero-count copy does not revoke a valid range");
    copy::invalidateMapped(0);copy::endMap(native.Get(),9);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: unknown mapped-base fault revokes every active lease");
    claim(src[0],frame);begin(10);forward(src,0,1);
    copy::invalidateMapped(reinterpret_cast<uintptr_t>(src));copy::endMap(native.Get(),10);
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: foreign non336 mapped resource preserves disjoint pool lease");
    ComPtr<ID3D11Buffer> secondNative;D3D11_BUFFER_DESC nd{};native->GetDesc(&nd);device->CreateBuffer(&nd,&initial,&secondNative);
    claim(src[0],frame);begin(11);copy::beginMap(secondNative.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,frame);
    forward(src,0,1);copy::endMap(native.Get(),11);copy::endMap(secondNative.Get(),1);
    check(copy::patches(native.Get(),frame).empty() && copy::patches(secondNative.Get(),frame).empty(),"primary copy: ambiguous active mapped-base aliases refuse and invalidate both pools");
    copy::forget(secondNative.Get());
    // Destination clear must not erase the queued source of the next merge.
    copy::reset();Node cleared;Dictionary dictionary;dictionary.set(&cleared);cleared.item(0)=record(80);
    claim(cleared.item(0),frame);claim(src[0],frame);begin(1);forward(src,0,1);copy::endMap(native.Get(),1);
    claim(src[1],frame);const auto clearEpoch=copy::g_emissionEpoch;copy::invalidateDictionary(dictionary.ptr());
    check(!copy::g_emissions.count(reinterpret_cast<uintptr_t>(&cleared.item(0))) &&
          copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])) && copy::g_emissionEpoch==clearEpoch,
          "primary copy: destination dictionary clear preserves disjoint queued source owners");
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: CPU node free preserves already-copied GPU certificates");
    mapped[0]=cleared.item(0);begin(2);forward(&cleared.item(0),0,1);copy::endMap(native.Get(),2);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: same-address identical-payload reuse after free cannot inherit source identity");
    dictionary.set(nullptr);const auto calls=copy::stats().clearCalls;copy::invalidateDictionary(dictionary.ptr());
    check(copy::stats().clearCalls==calls+1 && copy::stats().clearFailed==0 && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])),
          "primary copy: normal empty dictionary clear is witnessed without poisoning unrelated claims");
    dictionary.set(&cleared);uintptr_t cycle=cleared.ptr();std::memcpy(cleared.data,&cycle,8);copy::invalidateDictionary(dictionary.ptr());
    check(copy::g_emissions.empty() && copy::stats().clearFailed==1,"primary copy: malformed dictionary cycle invalidates whole pending source epoch");

    // Native partial-node merge: one prefix goes to the destination's last
    // free slot; two items shift left. Identical payloads cannot merge owners.
    copy::reset();Node destination,source;uintptr_t da[2]={destination.ptr(),destination.ptr()},sa[2]={source.ptr(),source.ptr()};
    destination.links(reinterpret_cast<uintptr_t>(da),7);source.links(reinterpret_cast<uintptr_t>(sa),3);
    for(unsigned i=0;i<3;++i){source.item(i)=record(float(30+i));claim(source.item(i),frame);}
    const auto s0=source.item(0),s1=source.item(1),s2=source.item(2);
    void* plan=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(da),reinterpret_cast<uintptr_t>(sa),frame);
    check(plan!=nullptr && copy::g_emissions.empty(),"primary copy: pre-merge stages claims then clears every original source address");
    destination.item(7)=s0;std::memmove(&source.item(0),&source.item(1),2*336);
    copy::endMergeOpaque(plan,true);
    check(copy::g_emissions.size()==3 && copy::g_emissions.at(reinterpret_cast<uintptr_t>(&source.item(0))).patch.native==s1 &&
          copy::g_emissions.at(reinterpret_cast<uintptr_t>(&source.item(1))).patch.native==s2,
          "primary copy: authoritative partial merge preserves distinct shifted identities");
    mapped[0]=source.item(0);mapped[1]=source.item(1);begin(1);forward(&source.item(0),0,2);copy::endMap(native.Get(),1);
    check(copy::patches(native.Get(),frame).size()==2,"primary copy: relocated merge claims join native copier destination");
    check(copy::g_emissions.count(reinterpret_cast<uintptr_t>(&source.item(2)))==0,"primary copy: shifted old source address cannot alias later reuse");
    source.links(reinterpret_cast<uintptr_t>(sa),9);
    check(copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(da),reinterpret_cast<uintptr_t>(sa),frame)==nullptr && copy::g_emissions.empty(),
          "primary copy: invalid merge count invalidates whole pending-emission epoch");
    // Full nodes insert at HEAD. A later partial node must still fill the
    // preexisting partial TAIL, including the exhausted/freed-node case.
    copy::reset();Node tail,full,partial;uintptr_t d2[2]={tail.ptr(),tail.ptr()},s2a[2]={full.ptr(),partial.ptr()};
    tail.links(reinterpret_cast<uintptr_t>(d2),3);full.links(reinterpret_cast<uintptr_t>(s2a),8);partial.links(reinterpret_cast<uintptr_t>(s2a),2);
    uintptr_t next=partial.ptr(),prev=full.ptr();std::memcpy(full.data,&next,8);std::memcpy(partial.data+8,&prev,8);
    for(unsigned i=0;i<8;++i){full.item(i)=record(float(100+i));claim(full.item(i),frame);}
    for(unsigned i=0;i<2;++i){partial.item(i)=record(float(200+i));claim(partial.item(i),frame);}
    void* mixed=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(d2),reinterpret_cast<uintptr_t>(s2a),frame);
    check(mixed!=nullptr,"primary copy: partial-tail/full-head/partial-source merge planned");
    tail.item(3)=partial.item(0);tail.item(4)=partial.item(1);copy::endMergeOpaque(mixed,true);
    check(copy::g_emissions.size()==10 && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&tail.item(3))) &&
          copy::g_emissions.count(reinterpret_cast<uintptr_t>(&tail.item(4))) && !copy::g_emissions.count(reinterpret_cast<uintptr_t>(&partial.item(0))),
          "primary copy: full head preserves old tail and exhausted partial claims relocate exactly");
    copy::reset();Node planned;Dictionary plannedDictionary;plannedDictionary.set(&planned);planned.item(0)=record(400);claim(planned.item(0),frame);
    uintptr_t emptyDestination[2];emptyDestination[0]=emptyDestination[1]=reinterpret_cast<uintptr_t>(emptyDestination);
    void* pending=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(emptyDestination),plannedDictionary.anchor(),frame);
    claim(src[1],frame);copy::invalidateDictionary(plannedDictionary.ptr());copy::endMergeOpaque(pending,true);
    check(!copy::g_emissions.count(reinterpret_cast<uintptr_t>(&planned.item(0))) && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])),
          "primary copy: node free revokes detached overlapping merge plan without erasing unrelated source owners");
    claim(src[0],frame);const auto epoch=copy::g_emissionEpoch;copy::invalidateEmission(0);
    check(copy::g_emissions.empty() && copy::g_emissionEpoch!=epoch,"primary copy: unknown append invalidates source epoch before allocator reuse");
    copy::reset();std::vector<Record> many(copy::kMaxEmissions+1,record(300));
    for(auto& r:many)claim(r,frame);
    check(copy::g_emissions.empty() && copy::stats().overflows==1 && !claim(src[0],frame),"primary copy: capacity overflow poisons all same-frame claims");
    copy::forget(native.Get());copy::reset();
}
}
