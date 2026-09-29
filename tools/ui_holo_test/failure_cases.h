#pragma once
// Actual production cache/binding functions, injected COM failure boundaries.
// Real WARP context commands execute before the one-shot setter/query faults.
namespace holoFailure {
using namespace edvr::ui_holo_remap;
enum class Site {Good,GetDevice,CreateFail,CreatePartialFail,CreateFault,CreateNull,
    BufferFail,BufferPartialFail,BufferFault,BufferNull,GetShader,GetBuffer,Update,SetBuffer,SetShader,Classes,
    BeforeShader,BeforeBuffer,PersistentBeforeShader};
struct Child {void** table;ULONG refs=0;};
inline ULONG STDMETHODCALLTYPE childRelease(Child* p){return --p->refs;}
struct Device {void** table;ULONG refs=1;Site site=Site::Good;Child ps{},cb{};unsigned creates=0,buffers=0;};
inline ULONG STDMETHODCALLTYPE add(Device* d){return ++d->refs;}
inline ULONG STDMETHODCALLTYPE drop(Device* d){return --d->refs;}
inline void fault(){RaiseException(0xe0421313,0,0,nullptr);}
inline HRESULT STDMETHODCALLTYPE createPs(Device* d,const void* bytes,SIZE_T n,ID3D11ClassLinkage* linkage,ID3D11PixelShader** out){
    ++d->creates;check(bytes&&n>64&&!linkage,"production creation consumes verified DXBC without linkage");*out=nullptr;
    if(d->site==Site::CreateFail)return E_FAIL;if(d->site==Site::CreateNull)return S_OK;
    d->ps.refs=1;*out=reinterpret_cast<ID3D11PixelShader*>(&d->ps);
    if(d->site==Site::CreateFault)fault();return d->site==Site::CreatePartialFail?E_FAIL:S_OK;
}
inline HRESULT STDMETHODCALLTYPE createCb(Device* d,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer** out){
    ++d->buffers;check(desc->ByteWidth==16&&desc->BindFlags==D3D11_BIND_CONSTANT_BUFFER,"bounded per-draw float4 constants");*out=nullptr;
    if(d->site==Site::BufferFail)return E_FAIL;if(d->site==Site::BufferNull)return S_OK;
    d->cb.refs=1;*out=reinterpret_cast<ID3D11Buffer*>(&d->cb);
    if(d->site==Site::BufferFault)fault();return d->site==Site::BufferPartialFail?E_FAIL:S_OK;
}
struct Context {void** table;Device* d=nullptr;ID3D11DeviceContext* real=nullptr;Site site=Site::Good;Child classes[2]{};unsigned setters=0;};
inline void STDMETHODCALLTYPE getDevice(Context* c,ID3D11Device** out){*out=reinterpret_cast<ID3D11Device*>(c->d);add(c->d);if(c->d->site==Site::GetDevice)fault();}
inline void once(Context* c,Site s){if(c->site==s){c->site=Site::Good;fault();}}
inline void STDMETHODCALLTYPE getShader(Context* c,ID3D11PixelShader** ps,ID3D11ClassInstance** a,UINT* n){
    c->real->PSGetShader(ps,a,n);
    if(c->site==Site::Classes&&a&&n&&*n==0){for(unsigned i=0;i<2;++i){++c->classes[i].refs;a[i]=reinterpret_cast<ID3D11ClassInstance*>(&c->classes[i]);}*n=2;}
    once(c,Site::GetShader);
}
inline void STDMETHODCALLTYPE getCb(Context* c,UINT s,UINT n,ID3D11Buffer** b){c->real->PSGetConstantBuffers(s,n,b);once(c,Site::GetBuffer);}
inline void STDMETHODCALLTYPE update(Context* c,ID3D11Resource* r,UINT s,const D3D11_BOX* b,const void* data,UINT row,UINT slice){c->real->UpdateSubresource(r,s,b,data,row,slice);once(c,Site::Update);}
inline void STDMETHODCALLTYPE setCb(Context* c,UINT s,UINT n,ID3D11Buffer*const* b){++c->setters;once(c,Site::BeforeBuffer);c->real->PSSetConstantBuffers(s,n,b);once(c,Site::SetBuffer);}
inline void STDMETHODCALLTYPE setPs(Context* c,ID3D11PixelShader* ps,ID3D11ClassInstance*const* a,UINT n){++c->setters;once(c,Site::BeforeShader);if(c->site==Site::PersistentBeforeShader)fault();c->real->PSSetShader(ps,a,n);once(c,Site::SetShader);}

inline void run(ID3D11DeviceContext* real,ID3D11PixelShader* original,ID3D11PixelShader* patched,
                ID3D11Buffer* constants,const std::vector<BYTE>& bytes){
    void* childV[3]{};childV[2]=reinterpret_cast<void*>(&childRelease);
    void* deviceV[43]{};deviceV[1]=reinterpret_cast<void*>(&add);deviceV[2]=reinterpret_cast<void*>(&drop);
    deviceV[3]=reinterpret_cast<void*>(&createCb);deviceV[15]=reinterpret_cast<void*>(&createPs);
    void* ctxV[128]{};ctxV[3]=reinterpret_cast<void*>(&getDevice);ctxV[9]=reinterpret_cast<void*>(&setPs);
    ctxV[16]=reinterpret_cast<void*>(&setCb);ctxV[48]=reinterpret_cast<void*>(&update);
    ctxV[74]=reinterpret_cast<void*>(&getShader);ctxV[77]=reinterpret_cast<void*>(&getCb);
    const uint64_t ps=hash(bytes.data(),bytes.size());Identity old{};UINT length=sizeof(old);
    ck(original->GetPrivateData(identityGuid(),&length,&old));
    for(Site site:{Site::Good,Site::GetDevice,Site::CreateFail,Site::CreatePartialFail,Site::CreateFault,
            Site::CreateNull,Site::BufferFail,Site::BufferPartialFail,Site::BufferFault,Site::BufferNull}){
        Device device{deviceV,1,site,{childV,0},{childV,0}};Context context{ctxV,&device};Cache cache;
        check(cache.remember(original,ps,bytes.data(),bytes.size(),false),"failure cache retains exact source");
        const Identity marker{ps,reinterpret_cast<uintptr_t>(&device)};ck(original->SetPrivateData(identityGuid(),sizeof(marker),&marker));
        auto made=cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps);
        check((made!=nullptr)==(site==Site::Good),"failed/partial/null creation never admits a shader");
        if(site==Site::Good){check(cache.constants()!=nullptr,"successful creation includes constants");
            check(cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps)==made&&device.creates==1&&device.buffers==1,"warm cache creates nothing again");}
        else if(site!=Site::GetDevice){check(!cache.prepare(reinterpret_cast<ID3D11DeviceContext*>(&context),original,ps)&&device.creates==1,"failed device slot is bounded, no repeated preparation");}
        cache.reset();check(device.refs==1&&device.ps.refs==0&&device.cb.refs==0,"partial COM publication and device lease fully reclaimed");
        cache.reset();check(device.refs==1,"reset is idempotent");
    }
    ck(original->SetPrivateData(identityGuid(),sizeof(old),&old));
    real->PSSetShader(original,nullptr,0);ComPtr<ID3D11Buffer> oldCb;real->PSGetConstantBuffers(13,1,&oldCb);
    for(Site site:{Site::Good,Site::GetShader,Site::GetBuffer,Site::Update,Site::SetBuffer,Site::SetShader,Site::Classes}){
        Context context{ctxV,nullptr,real,site,{{childV,1},{childV,1}}};Binding binding;bool ok=false;
        const bool ran=edvr::guarded("ui_holo_test.partial",[&]{ok=binding.begin(reinterpret_cast<ID3D11DeviceContext*>(&context),patched,constants,{.4f,.4f,.125f,-.25f},directSetPs,original);});
        check(ran==(site==Site::Good||site==Site::Classes)&&ok==(site==Site::Good),"production partial begin reports failure");
        check(binding.restore(reinterpret_cast<ID3D11DeviceContext*>(&context)),"all saved shader/constant state restores after partial begin");binding.clear();
        ComPtr<ID3D11PixelShader> psNow;ComPtr<ID3D11Buffer> cbNow;UINT classes=0;
        real->PSGetShader(&psNow,nullptr,&classes);real->PSGetConstantBuffers(13,1,&cbNow);
        check(psNow.Get()==original&&cbNow.Get()==oldCb.Get()&&classes==0,"complete stock PS/b13 state preserved after normal/failure/SEH");
        check(context.classes[0].refs==1&&context.classes[1].refs==1,"all unsupported class getter leases reclaimed");
        if(site==Site::Classes)check(context.setters==0,"dynamic linkage declines before any state mutation");
    }
    Binding empty;
    for(Site site:{Site::BeforeShader,Site::BeforeBuffer,Site::PersistentBeforeShader}){
        Context context{ctxV,nullptr,real};Binding binding;
        check(binding.begin(reinterpret_cast<ID3D11DeviceContext*>(&context),patched,constants,{.4f,.4f,0,0}),"restore failure starts with actually moved rendering state");
        context.site=site;const auto result=binding.finish(reinterpret_cast<ID3D11DeviceContext*>(&context));
        check(result.retried&&result.restored==(site!=Site::PersistentBeforeShader),"before-real restore fault gets exactly one bounded retry, persistent failure remains untrusted");
        check(binding.needsRestore()==!result.restored,"persistent failure retains original references and blocks fallback and all replay issues");
        ComPtr<ID3D11PixelShader> restoredPs;ComPtr<ID3D11Buffer> restoredCb;real->PSGetShader(&restoredPs,nullptr,nullptr);real->PSGetConstantBuffers(13,1,&restoredCb);
        check(restoredCb.Get()==oldCb.Get(),"CB restore still runs after a shader restore fault");
        check(restoredPs.Get()==(result.restored?original:patched),"failed restoration never reports complete stock shader state");
        // The production WriteBackBegin gate uses result.restored retained in
        // Draw even after saved refs clear. No replay is safe on false.
        binding.clear();real->PSSetShader(original,nullptr,0);
    }
    check(!empty.begin(real,patched,constants,{NAN,1,0,0})&&!empty.begin(real,patched,constants,{1,0,0,0}),"invalid constant map refuses before state capture");
    Params p{};check(!params(0,72,160,160,0,0,p)&&!params(96,72,0,160,0,0,p)&&!params(96,72,160,160,INFINITY,0,p),"invalid dimensions/jitter refuse");
    check(params(96,72,240,180,.125f,-.25f,p)&&p.x==.4f&&p.y==.4f&&p.bx==.125f&&p.by==-.25f,"production inverse viewport and jitter float4");
    auto chunks=parseContainer(bytes.data(),bytes.size(),0x50);
    for(const auto& c:chunks)if(c.tag==0x58454853||c.tag==0x52444853){
        std::vector<uint32_t> t(c.bytes.size()/4);std::memcpy(t.data(),c.bytes.data(),c.bytes.size());
        for(size_t at=2;at<t.size();at+=instructionLength(t,at))if((t[at]&2047)==89){t[at+2]=13;break;}
        bool rejected=false;try{patchProgram(t);}catch(const std::exception&){rejected=true;}check(rejected,"occupied shader b13 refuses, never rewritten");
    }
}
} // namespace holoFailure
