// WARP exercises the shipped mono shaders/renderer. Backend stubs inspect their
// real GPU inputs and deliberately clobber state; SDK image quality is separate.
#include "../../src/d3d11/flat_mono_resolve.h"
#include "../../src/d3d11/dlaa.h"
#include "../../src/d3d11/fsr3_engine.h"
#include "../../src/d3d11/engine_velocity_emit.h"
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "../../src/common/config.h"
#include "../../src/common/log.h"
using Microsoft::WRL::ComPtr;
namespace {
int failures=0,backendCalls=0;
bool backendFail=false,backendReset=false,infiniteSeen=false;
std::vector<std::string> resetEvents;
float expectedJx=0,expectedJy=0;
float observedMotion=0,observedMotionY=0,observedDepth=0;unsigned observedReject=0;
uint32_t observedInW=0,observedInH=0,observedOutW=0,observedOutH=0;
void check(bool ok,const char* text){if(!ok){std::printf("FAIL: %s\n",text);++failures;}}
bool readPixel(ID3D11DeviceContext* context,ID3D11Texture2D* texture,void* out,size_t bytes,UINT x=8,UINT y=8) {
    ComPtr<ID3D11Device> device;context->GetDevice(device.GetAddressOf());
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,staging.GetAddressOf())))return false;
    context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return false;
    std::memcpy(out,static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch+size_t(x)*bytes,bytes);
    context->Unmap(staging.Get(),0);return true;
}
float half(uint16_t value) {
    const unsigned exponent=(value>>10)&31,mantissa=value&1023;
    const float result=exponent?std::ldexp(1.0f+mantissa/1024.0f,int(exponent)-15):std::ldexp(float(mantissa),-24);
    return value&0x8000?-result:result;
}
bool backend(ID3D11DeviceContext* c,ID3D11Texture2D* depth,ID3D11Texture2D* mv,ID3D11Texture2D* mask,
             ID3D11Texture2D* out,float jx,float jy,bool reset,const char** reason) {
    ++backendCalls;backendReset=reset;
    check(jx==expectedJx && jy==expectedJy,"backend receives actual rendered phase");
    uint16_t motion[2]{};unsigned char reject=0;
    check(readPixel(c,mv,motion,sizeof(motion)) && readPixel(c,depth,&observedDepth,sizeof(float)) &&
          readPixel(c,mask,&reject,1),"backend inputs readable");
    observedMotion=half(motion[0]);observedMotionY=half(motion[1]);observedReject=reject;
    c->ClearState(); // Both successful and refused backends may clobber all stages.
    if(backendFail){if(reason)*reason="injected-backend-refusal";return false;}
    ComPtr<ID3D11Device> d;c->GetDevice(d.GetAddressOf());ComPtr<ID3D11UnorderedAccessView> uav;
    if(FAILED(d->CreateUnorderedAccessView(out,nullptr,uav.GetAddressOf())))return false;
    const float green[4]={0,1,0,1};c->ClearUnorderedAccessViewFloat(uav.Get(),green);
    return true;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT fmt,UINT binds,const void* bytes=nullptr,UINT pitch=0) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.ArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Format=fmt;desc.BindFlags=binds;desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=bytes;data.SysMemPitch=pitch;ComPtr<ID3D11Texture2D> out;
    check(SUCCEEDED(d->CreateTexture2D(&desc,bytes?&data:nullptr,out.GetAddressOf())),"fixture texture creation");return out;
}
ComPtr<ID3D11ShaderResourceView> view(ID3D11Device* d,ID3D11Resource* r) {
    ComPtr<ID3D11ShaderResourceView> out;check(SUCCEEDED(d->CreateShaderResourceView(r,nullptr,out.GetAddressOf())),"fixture view creation");return out;
}
void camera(float (&rows)[6][4]) {
    std::memset(rows,0,sizeof(rows));rows[0][0]=rows[1][1]=rows[2][3]=rows[4][2]=1;rows[3][2]=.025f;
}
uint32_t bits(float f){uint32_t v;std::memcpy(&v,&f,4);return v;}
} // namespace
namespace edvr {
Config& Config::get() {
    static Config* config=[] {auto* c=new Config;wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr,exe,32768);std::wstring path=exe;
        c->m_logDir=path.substr(0,path.find_last_of(L"\\/"))+L"\\flat-pixel-fixture";return c;}();
    return *config;
}
Log& Log::get() {static auto* log=new Log;return *log;}
void Log::note(const char* fmt,...) {
    constexpr char prefix[]="flat resolve reset event:";
    if(std::strncmp(fmt,prefix,sizeof(prefix)-1)!=0)return;
    char line[1024]{};
    va_list args;va_start(args,fmt);std::vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    resetEvents.emplace_back(line);
}
bool ensureDirectory(const std::wstring& path) {return CreateDirectoryW(path.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS;}
thread_local bool g_flatComputeInternal = false;
bool dlaaAvailable(ID3D11Device*,const char**){return true;}
bool fsr3Available(ID3D11Device*,const char**){return true;}
bool dlaaEvaluate(ID3D11DeviceContext* c,int,ID3D11Texture2D*,ID3D11Texture2D* depth,ID3D11Texture2D* mv,
    ID3D11Texture2D* out,ID3D11Texture2D* mask,uint32_t w,uint32_t h,uint32_t outW,uint32_t outH,float jx,float jy,bool reset,float,const char** why) {
    observedInW=w;observedInH=h;observedOutW=outW;observedOutH=outH;
    return backend(c,depth,mv,mask,out,jx,jy,reset,why);
}
bool fsr3Evaluate(ID3D11DeviceContext* c,unsigned,ID3D11Texture2D*,ID3D11Texture2D* depth,ID3D11Texture2D* mv,
    ID3D11Texture2D* mask,ID3D11Texture2D* out,uint32_t w,uint32_t h,uint32_t outW,uint32_t outH,float jx,float jy,bool reset,float,
    float nearZ,float,float fov,const char** why,bool infinite) {
    observedInW=w;observedInH=h;observedOutW=outW;observedOutH=outH;
    infiniteSeen=infinite;check(nearZ==.025f && std::abs(fov-1.5707963f)<1e-5f,"FSR actual near and FOV");
    return backend(c,depth,mv,mask,out,jx,jy,reset,why);
}
} // namespace edvr
#include "flat_projection_scope_tests.h"
#include "flat_projection_runtime_tests.h"
#include "flat_pixel_capture_gpu_tests.h"
#include "flat_draw_capture_gpu_tests.h"
int main(int argc,char** argv) {
    if(argc!=2 || (std::strcmp(argv[1],"--self-test") && std::strcmp(argv[1],"--dry-run"))){std::puts("usage: flat_mono_resolve_test --self-test|--dry-run");return 2;}
    if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would exercise mono resolve WARP shaders, backend inputs and state restoration; writes no files.");return 0;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,
        device.GetAddressOf(),&level,context.GetAddressOf());
    if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        device.GetAddressOf(),&level,context.GetAddressOf());
    check(SUCCEEDED(hr),"WARP device");if(FAILED(hr))return 1;
    ComPtr<ID3D11InfoQueue> messages;device.As(&messages);
    projectionScopeTests(device.Get(), context.Get());
    projectionRuntimeTests(device.Get(), context.Get());
    const UINT w=16,h=16;std::vector<uint32_t> red(w*h,0xff0000ff);std::vector<float> z(w*h,.01f),slots(w*h*2);
    for(size_t i=0;i<slots.size();i+=2){slots[i]=-1;slots[i+1]=.01f;}
    auto color=texture(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,red.data(),w*4);
    auto depth=texture(device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,z.data(),w*4);
    auto slotTexture=texture(device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,slots.data(),w*8);
    auto colorView=view(device.Get(),color.Get()),depthView=view(device.Get(),depth.Get()),slotView=view(device.Get(),slotTexture.Get());
    uint32_t record[84]{};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(record);bd.StructureByteStride=336;
    bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=record;ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(device->CreateBuffer(&bd,&initial,pool.GetAddressOf())),"pool buffer");auto poolView=view(device.Get(),pool.Get());
    // The freshness stamp the prep shader's EN[276].x reads (the emit's
    // present-frame clock in production): the fixture's markers fold it in.
    constexpr uint32_t kFixtureStamp = 77;
    float scene[277][4]{};float cam[6][4];camera(cam);std::memcpy(scene+270,cam,sizeof(cam));
    {const uint32_t stamp=kFixtureStamp;std::memcpy(&scene[276][0],&stamp,4);}
    bd={};bd.ByteWidth=sizeof(scene);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    initial.pSysMem=scene;ComPtr<ID3D11Buffer> now,old;
    check(SUCCEEDED(device->CreateBuffer(&bd,&initial,now.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&bd,&initial,old.GetAddressOf())),"engine camera buffers");
    auto target=texture(device.Get(),32,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> targetView;check(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,targetView.GetAddressOf())),"original RTV");
    D3D11_VIEWPORT viewport{3,4,19,21,.2f,.8f};
    auto bindOriginal=[&]{ID3D11RenderTargetView* rt=targetView.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        ID3D11ShaderResourceView* srv=colorView.Get();context->PSSetShaderResources(0,1,&srv);context->VSSetShaderResources(3,1,&srv);
        ID3D11Buffer* cb=old.Get();context->CSSetConstantBuffers(4,1,&cb);context->RSSetViewports(1,&viewport);};
    auto restored=[&]{ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11ShaderResourceView> ps,vs;ComPtr<ID3D11Buffer> cb;
        context->OMGetRenderTargets(1,rt.GetAddressOf(),nullptr);context->PSGetShaderResources(0,1,ps.GetAddressOf());
        context->VSGetShaderResources(3,1,vs.GetAddressOf());context->CSGetConstantBuffers(4,1,cb.GetAddressOf());
        UINT count=1;D3D11_VIEWPORT current{};context->RSGetViewports(&count,&current);
        return rt.Get()==targetView.Get() && ps.Get()==colorView.Get() && vs.Get()==colorView.Get() && cb.Get()==old.Get() &&
            count==1 && !std::memcmp(&current,&viewport,sizeof(viewport));};
    edvr::FlatMonoResolveFrame f{};f.color=colorView.Get();f.depth=depthView.Get();f.renderWidth=w;f.renderHeight=h;
    f.outputWidth=f.outputHeight=32;f.deltaMs=16;camera(f.camera);camera(f.previousCamera);
    f.engine={slotView.Get(),poolView.Get(),now.Get(),old.Get()};f.mode=edvr::FlatMonoResolveMode::Dlss;f.frame=1;
    edvr::FlatMonoResolvePreflight planned{};planned.renderWidth=w;planned.renderHeight=h;
    planned.outputWidth=f.outputWidth;planned.outputHeight=f.outputHeight;planned.mode=f.mode;
    planned.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;planned.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
    const auto beforeInvalidPreflight=edvr::flatMonoResolveStats();
    auto invalidPreflight=planned;invalidPreflight.outputWidth=0;
    auto preflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),invalidPreflight);
    check(preflight.status==edvr::FlatMonoResolvePreflightStatus::InvalidMetadata &&
          !preflight.readyForRasterJitter() && backendCalls==0 &&
          edvr::flatMonoResolveStats().initializations==beforeInvalidPreflight.initializations &&
          edvr::flatMonoResolveStats().allocations==beforeInvalidPreflight.allocations,
          "bad extent preflight refuses before renderer allocation or backend work");
    preflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),planned);
    check(preflight.readyForRasterJitter() && preflight.spatialFallbackReady &&
          preflight.backendAvailable && preflight.backendFeatureCreationDeferred &&
          preflight.reason && !std::strcmp(preflight.reason,"ready-backend-feature-creation-deferred") &&
          backendCalls==0,"valid metadata preallocates spatial output without evaluating a backend feature");
    const auto preflightAllocations=edvr::flatMonoResolveStats().allocations;
    auto run=[&](bool wanted){bindOriginal();ComPtr<ID3D11ShaderResourceView> out;const char* reason=nullptr;
        bool ok=edvr::flatMonoResolve(device.Get(),context.Get(),f,out.GetAddressOf(),&reason);
        if(ok!=wanted)std::printf("info: resolver reason %s\n",reason?reason:"none");
        check(ok==wanted,"resolver result");check(restored(),"complete original pipeline restored");
        check(ok?out!=nullptr:out==nullptr,"owned output only on success");return out;};
    auto pixel=[&](ID3D11ShaderResourceView* srv,UINT x=16,UINT y=16){uint32_t value=0;
        if(!srv)return value;ComPtr<ID3D11Resource> resource;srv->GetResource(resource.GetAddressOf());ComPtr<ID3D11Texture2D> tex;resource.As(&tex);
        check(tex && readPixel(context.Get(),tex.Get(),&value,4,x,y),"resolved pixel readback");return value;};
    if(messages)messages->ClearStoredMessages();
    auto first=run(true);check(backendReset && observedReject==255 && pixel(first.Get())==0xff0000ff,"reset seeds backend but displays current color");
    auto stats=edvr::flatMonoResolveStats();
    check(stats.calls==1 && stats.initializations==1 && stats.allocations==1 && stats.acceptedResets==1 &&
          stats.acceptedContinues==0 && stats.lostHistory==1 && stats.currentContinueRun==0,
          "first frame reports one state build, one texture allocation and one accepted reset");
    check(resetEvents.size()==1 && resetEvents.back().find("frame=1 mode=dlss")!=std::string::npos &&
          resetEvents.back().find("requested=1 lost=1")!=std::string::npos &&
          resetEvents.back().find("camera-cut=0")!=std::string::npos &&
          resetEvents.back().find("delta-ms=16")!=std::string::npos,
          "successful backend reset logs frame, mode, reasons and elapsed time");
    f.reset=false;f.frame=2;f.camera[5][0]=.3125f;
    auto second=run(true);check(!backendReset && std::abs(observedMotion-1)<.001 && observedDepth==.01f,
        "camera translation gives positive one-render-pixel current-to-previous motion and unchanged raw depth");
    stats=edvr::flatMonoResolveStats();
    check(stats.initializations==1 && stats.allocations==1 && stats.acceptedContinues==1 &&
          stats.currentContinueRun==1 && stats.longestContinueRun==1 && stats.contextPointerMismatches==0,
          "continuous frame reuses state and textures without a reset");
    check(resetEvents.size()==1,"continuous backend frame emits no reset event");
    check(pixel(second.Get())==0xff00ff00,"valid pixel uses trained output");
    check(pixel(second.Get(),31,16)==0xff0000ff,"offscreen reprojection displays current spatial color");
    f.jitterX=expectedJx=.25f;f.jitterY=expectedJy=-.375f;
    f.previousJitterX=-.25f;f.previousJitterY=.375f;++f.frame;
    run(true);
    check(!backendReset && std::abs(observedMotion-1)<.001f && std::abs(observedMotionY)<.001f,
          "unjittered camera reconstructs depth at the current raster phase; SDK vector excludes both phases");
    // A matched pixel's own rigid record moves oppositely: exact engine vector
    // must replace the positive camera vector, not estimate it from a heuristic.
    record[1]=record[77]=bits(1);record[2]=record[78]=0x7fff7fff;record[3]=record[79]=0xfffe7fff;
    record[4]=bits(0);record[5]=bits(0);record[6]=bits(2.5f);
    record[73]=bits(-.3125f);record[74]=bits(0);record[75]=bits(2.5f);
    edvr::engine_velocity_emit::Pose np{{record[4],record[5],record[6],record[2],record[3]}};
    edvr::engine_velocity_emit::Pose pp{{record[73],record[74],record[75],record[78],record[79]}};
    record[72]=0x7FC0ED01u^edvr::engine_velocity_emit::markerHash(np,pp,kFixtureStamp);
    context->UpdateSubresource(pool.Get(),0,nullptr,record,0,0);
    slots[(8*w+8)*2]=1;context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);
    ++f.frame;run(true);
    if(std::abs(observedMotion+1)>=.001 || observedReject!=0)std::printf("info: joined motion=%g reject=%u\n",observedMotion,observedReject);
    check(std::abs(observedMotion+1)<.001 && std::abs(observedMotionY)<.001 && observedReject==0,
          "joined engine pose uses shared exact reprojection at unjittered UV despite raster phase");
    for(unsigned kind=0;kind<3;++kind){
        if(kind==0)slots[(8*w+8)*2]=2; // corrupt even code
        if(kind==1){slots[(8*w+8)*2]=1;slots[(8*w+8)*2+1]=.02f;} // stale depth
        if(kind==2){slots[(8*w+8)*2+1]=.01f;record[72]=0x7FC0ED02u^edvr::engine_velocity_emit::markerHash(np,pp,kFixtureStamp);context->UpdateSubresource(pool.Get(),0,nullptr,record,0,0);}
        context->UpdateSubresource(slotTexture.Get(),0,nullptr,slots.data(),w*8,0);++f.frame;
        auto rejected=run(true);check(observedReject==255 && pixel(rejected.Get())==0xff0000ff,"corrupt/stale/masked pixel displays current color");
    }
    backendFail=true;++f.frame;run(false);backendFail=false;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> recovered;const char* fallbackReason=nullptr;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,recovered.GetAddressOf(),&fallbackReason) &&
          recovered && restored() && pixel(recovered.Get())==0xff0000ff &&
          edvr::flatMonoResolveStats().allocations==preflightAllocations,
          "preflighted spatial fallback restores state and displays current jittered frame after SDK refusal without allocation");
    ++f.frame;run(true);check(backendReset,"backend failure and fallback invalidate history");
    stats=edvr::flatMonoResolveStats();check(stats.backendFailures==1 && stats.currentContinueRun==0,
        "backend failure and following accepted reset are visible in cumulative statistics");
    f.mode=edvr::FlatMonoResolveMode::Taa;f.reset=true;++f.frame;auto taa=run(true);
    check(pixel(taa.Get())==0xff0000ff,"TAA reset spatial upsample");f.reset=false;++f.frame;taa=run(true);
    check(pixel(taa.Get())==0xff0000ff,"TAA static color remains stable");
    f.mode=edvr::FlatMonoResolveMode::Fsr;++f.frame;run(true);check(infiniteSeen,"FSR receives explicit infinite-depth mode");
    f.mode=edvr::FlatMonoResolveMode::Dlss;++f.frame;auto retained=run(true);
    auto beforeInvalidate=edvr::flatMonoResolveStats();
    edvr::flatMonoResolveInvalidateHistory();++f.frame;auto invalidated=run(true);
    check(backendReset && retained.Get()==invalidated.Get(),"history-only invalidation preserves allocated output");
    stats=edvr::flatMonoResolveStats();
    check(stats.invalidations==beforeInvalidate.invalidations+1 && stats.allocations==beforeInvalidate.allocations &&
          stats.initializations==beforeInvalidate.initializations && stats.currentContinueRun==0,
          "history-only invalidation resets accumulation without rebuilding resources");
    // Original copy may decode SRGB. Keep encoded bits during reconstruction and
    // return that same view format, rather than silently changing its transfer.
    std::vector<uint32_t> encoded(w*h,0xff4080a0);
    auto srgbTexture=texture(device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_TYPELESS,D3D11_BIND_SHADER_RESOURCE,encoded.data(),w*4);
    D3D11_SHADER_RESOURCE_VIEW_DESC srgbDesc{};srgbDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srgbDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srgbDesc.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> srgbView;
    check(SUCCEEDED(device->CreateShaderResourceView(srgbTexture.Get(),&srgbDesc,srgbView.GetAddressOf())),"SRGB copy input fixture");
    f.color=srgbView.Get();f.reset=true;++f.frame;auto srgbResult=run(true);
    D3D11_SHADER_RESOURCE_VIEW_DESC resultDesc{};if(srgbResult)srgbResult->GetDesc(&resultDesc);
    check(resultDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && pixel(srgbResult.Get())==encoded[0],
          "DLSS reset preserves encoded bytes and original SRGB view transfer");
    stats=edvr::flatMonoResolveStats();check(stats.formatChanges==1,"input view format change is counted");
    f.mode=edvr::FlatMonoResolveMode::Taa;
    for(unsigned i=0;i<2;++i){++f.frame;srgbResult=run(true);if(srgbResult)srgbResult->GetDesc(&resultDesc);
        check(resultDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && pixel(srgbResult.Get())==encoded[0],
              "both TAA history surfaces preserve SRGB copy contract");f.reset=false;}
    const int callsBefore=backendCalls;f.mode=edvr::FlatMonoResolveMode::Dlaa;run(false);
    check(backendCalls==callsBefore,"DLAA scale mismatch declines before backend");
    f.mode=edvr::FlatMonoResolveMode::Dlss;f.color=colorView.Get();++f.frame;run(true);
    f.frame+=2;run(true);stats=edvr::flatMonoResolveStats();
    check(backendReset && stats.frameGaps>=1 && stats.currentContinueRun==0,"frame gap produces a counted reset");
    ++f.frame;f.camera[5][0]=51;run(true);stats=edvr::flatMonoResolveStats();
    check(backendReset && stats.cameraCuts==1,"camera cut produces a counted reset");
    check(!resetEvents.empty() && resetEvents.back().find("camera-cut=1")!=std::string::npos &&
          resetEvents.back().find("requested=0")!=std::string::npos &&
          resetEvents.back().find("now=(51,0,0) previous=(0,0,0) origin-delta=(51,0,0)")!=std::string::npos &&
          resetEvents.back().find("max-matrix-delta=0")!=std::string::npos,
          "accepted camera cut logs raw origins and separates it from a view-matrix change");
    f.camera[0][0]=0;run(false);check(backendCalls==callsBefore+3,"singular camera declines before backend");
    f.camera[0][0]=1;
    std::vector<uint32_t> step(w*h,0xff000000);
    for(UINT y=0;y<h;++y)for(UINT x=9;x<w;++x)step[y*w+x]=0xff0000ff;
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.jitterX=0;f.jitterY=0;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> spatialZero;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,spatialZero.GetAddressOf(),&fallbackReason) &&
          restored() && (pixel(spatialZero.Get())&255)==0,"zero-phase fallback samples the output grid directly");
    f.jitterX=.5f;
    bindOriginal();ComPtr<ID3D11ShaderResourceView> spatialJitter;
    check(edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,spatialJitter.GetAddressOf(),&fallbackReason) &&
          restored() && (pixel(spatialJitter.Get())&255)>0 && (pixel(spatialJitter.Get())&255)<255,
          "nonzero-phase fallback shifts the spatial sample to undo raster displacement");
    // At output pixel 17, the previous unjittered coordinate is render x=8.75.
    // Its old raster phase +.5 selects depth texel 9. A lookup without that
    // phase would select texel 8 and reject valid history in this fixture.
    f.mode=edvr::FlatMonoResolveMode::Taa;f.reset=true;++f.frame;
    camera(f.camera);camera(f.previousCamera);f.jitterY=f.previousJitterY=0;
    for(UINT y=0;y<h;++y)z[y*w+8]=.02f;
    context->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
    std::fill(step.begin(),step.end(),0xff0000ff);
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.jitterX=.5f;f.previousJitterX=0;
    auto phaseHistory=run(true);
    check((pixel(phaseHistory.Get(),17,16)&255)==255,"TAA reset places current color on the unjittered output grid");
    std::fill(step.begin(),step.end(),0xffff0000);
    for(UINT y=0;y<h;++y)step[y*w+8]=0xff0000ff; // keep red within the 3x3 history clamp
    context->UpdateSubresource(color.Get(),0,nullptr,step.data(),w*4,0);
    f.reset=false;++f.frame;f.jitterX=.25f;f.previousJitterX=.5f;
    phaseHistory=run(true);
    check((pixel(phaseHistory.Get(),17,16)&255)>200,
          "TAA compares expected depth at the previous frame's raster phase and retains aligned history");
    // Event output is bounded across the session, with a separate camera-cut
    // allowance even after requested resets exhaust the ordinary budget.
    for(unsigned i=0;i<40;++i){f.reset=true;++f.frame;run(true);}
    unsigned ordinaryLogs=0,cameraLogs=0;
    for(const auto& event:resetEvents)
        if(event.find("camera-cut=1")!=std::string::npos)++cameraLogs;else ++ordinaryLogs;
    check(ordinaryLogs==32 && cameraLogs==1,"ordinary reset events stop at their 32-line session cap");
    f.reset=false;++f.frame;f.camera[5][0]=51;run(true);
    unsigned cameraLogsAfter=0;
    for(const auto& event:resetEvents)if(event.find("camera-cut=1")!=std::string::npos)++cameraLogsAfter;
    check(ordinaryLogs==32 && cameraLogsAfter==2 && resetEvents.size()==34,
          "camera cut still logs after ordinary reset event cap is exhausted");
    f.jitterX=std::nanf("");
    bindOriginal();ComPtr<ID3D11ShaderResourceView> badJitter;
    check(!edvr::flatMonoResolveSpatialFallback(device.Get(),context.Get(),f,badJitter.GetAddressOf(),&fallbackReason) &&
          !badJitter && restored(),"nonfinite jitter cannot silently reach spatial fallback");
    // Gate 2 step 2 (design doc section 72): a supersampled render (R > D) on
    // the NVIDIA route evaluates DLAA at E = R on both axes, and the resolved
    // output view is R-sized so the game's own copy downsamples it to D.
    {
        const UINT w2=w*2,h2=h*2;
        std::vector<uint32_t> red2(w2*h2,0xff0000ff);std::vector<float> z2(w2*h2,.01f);
        std::vector<float> slots2(w2*h2*2);for(size_t i=0;i<slots2.size();i+=2){slots2[i]=-1;slots2[i+1]=.01f;}
        auto color2=texture(device.Get(),w2,h2,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,red2.data(),w2*4);
        auto depth2=texture(device.Get(),w2,h2,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,z2.data(),w2*4);
        auto slotTexture2=texture(device.Get(),w2,h2,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,slots2.data(),w2*8);
        auto colorView2=view(device.Get(),color2.Get()),depthView2=view(device.Get(),depth2.Get()),slotView2=view(device.Get(),slotTexture2.Get());
        edvr::FlatMonoResolveFrame f2{};f2.color=colorView2.Get();f2.depth=depthView2.Get();
        f2.renderWidth=w2;f2.renderHeight=h2;f2.outputWidth=w;f2.outputHeight=h;f2.deltaMs=16;
        camera(f2.camera);camera(f2.previousCamera);
        f2.engine={slotView2.Get(),poolView.Get(),now.Get(),old.Get()};
        f2.mode=edvr::FlatMonoResolveMode::Dlss;f2.frame=1;
        expectedJx=expectedJy=0;  // this block runs unjittered
        edvr::FlatMonoResolvePreflight planned2{};planned2.renderWidth=w2;planned2.renderHeight=h2;
        planned2.outputWidth=w;planned2.outputHeight=h;planned2.mode=f2.mode;
        planned2.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;planned2.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
        auto preflight2=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),planned2);
        check(preflight2.readyForRasterJitter() && preflight2.backendAvailable,
              "supersampled preflight is ready with backend creation deferred");
        bindOriginal();ComPtr<ID3D11ShaderResourceView> out2;const char* reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,out2.GetAddressOf(),&reason2) && out2,
              "supersampled DLSS frame resolves via DLAA at render size");
        if(reason2)std::printf("info: supersample resolver reason %s\n",reason2);
        check(restored(),"supersampled resolve restores the complete original pipeline");
        check(observedInW==w2 && observedInH==h2 && observedOutW==w2 && observedOutH==h2,
              "backend evaluates the supersample route at render size on both axes");
        ComPtr<ID3D11Resource> outResource2;if(out2)out2->GetResource(outResource2.GetAddressOf());
        ComPtr<ID3D11Texture2D> outTexture2;if(outResource2)outResource2.As(&outTexture2);
        D3D11_TEXTURE2D_DESC outDesc2{};if(outTexture2)outTexture2->GetDesc(&outDesc2);
        check(outDesc2.Width==w2 && outDesc2.Height==h2,
              "the supersampled output view is render-sized for the game's downsample");
        check(pixel(out2.Get(),16,16)==0xff0000ff,"supersampled reset displays current render-size color");
        // FSR mirrors NVIDIA here: Native AA is the 1.0x case of the same
        // upscaler, evaluating at render size for the game's downsample.
        f2.mode=edvr::FlatMonoResolveMode::Fsr;f2.frame=2;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outFsr;reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,outFsr.GetAddressOf(),&reason2) && outFsr,
              "supersampled FSR frame resolves via Native AA at render size");
        if(reason2)std::printf("info: supersample FSR resolver reason %s\n",reason2);
        check(restored(),"supersampled FSR resolve restores the complete original pipeline");
        check(observedInW==w2 && observedInH==h2 && observedOutW==w2 && observedOutH==h2,
              "FSR evaluates the supersample route at render size on both axes");
        // TAA at R > D evaluates on the display grid today (the route's
        // honest report): the resolved view stays D-sized.
        f2.mode=edvr::FlatMonoResolveMode::Taa;f2.frame=3;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outTaa;reason2=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),f2,outTaa.GetAddressOf(),&reason2) && outTaa,
              "supersampled TAA frame resolves on the display grid");
        if(reason2)std::printf("info: supersample TAA resolver reason %s\n",reason2);
        ComPtr<ID3D11Resource> outResTaa;if(outTaa)outTaa->GetResource(outResTaa.GetAddressOf());
        ComPtr<ID3D11Texture2D> outTexTaa;if(outResTaa)outResTaa.As(&outTexTaa);
        D3D11_TEXTURE2D_DESC outDescTaa{};if(outTexTaa)outTexTaa->GetDesc(&outDescTaa);
        check(outDescTaa.Width==w && outDescTaa.Height==h,
              "the display-grid TAA output stays display-sized at supersampling");
    }
    // Gate-2 review F1: the negotiated evaluation size is part of the resolve's
    // resource cache key, and the preflight carries it. A cut E must reallocate
    // at the cut size (never reuse the route-default cache), restoring the
    // default reallocates back, and a preflight carrying the same cut lets the
    // first treated frame hit that cache instead of reallocating.
    {
        expectedJx=expectedJy=0;  // this block runs unjittered
        edvr::FlatMonoResolveFrame fc{};fc.color=colorView.Get();fc.depth=depthView.Get();
        fc.renderWidth=w;fc.renderHeight=h;fc.outputWidth=32;fc.outputHeight=32;fc.deltaMs=16;
        camera(fc.camera);camera(fc.previousCamera);
        fc.engine={slotView.Get(),poolView.Get(),now.Get(),old.Get()};
        fc.mode=edvr::FlatMonoResolveMode::Dlss;fc.frame=1;
        const auto cutBase=edvr::flatMonoResolveStats();
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outDefault;const char* cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outDefault.GetAddressOf(),&cutReason) && outDefault,
              "default-E frame resolves on the route's evaluation grid");
        if(cutReason)std::printf("info: cut-probe default reason %s\n",cutReason);
        check(restored(),"default-E resolve restores the complete original pipeline");
        check(observedInW==w && observedInH==h && observedOutW==32 && observedOutH==32,
              "backend evaluates the default route at display size");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+1,
              "default-E frame allocates the route-default resource set once");
        ++fc.frame;fc.evalWidth=fc.evalHeight=24;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outCut;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outCut.GetAddressOf(),&cutReason) && outCut,
              "cut-E frame resolves on the negotiated evaluation grid");
        if(cutReason)std::printf("info: cut-probe cut reason %s\n",cutReason);
        check(restored(),"cut-E resolve restores the complete original pipeline");
        check(observedInW==w && observedInH==h && observedOutW==24 && observedOutH==24,
              "backend evaluates the negotiated cut at its own size");
        ComPtr<ID3D11Resource> outCutResource;if(outCut)outCut->GetResource(outCutResource.GetAddressOf());
        ComPtr<ID3D11Texture2D> outCutTexture;if(outCutResource)outCutResource.As(&outCutTexture);
        D3D11_TEXTURE2D_DESC outCutDesc{};if(outCutTexture)outCutTexture->GetDesc(&outCutDesc);
        check(outCutDesc.Width==24 && outCutDesc.Height==24,
              "the cut-E output view is cut-sized for the game's upsample");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+2,
              "a changed E reallocates rather than reusing the route-default cache");
        ++fc.frame;fc.evalWidth=fc.evalHeight=0;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outBack;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outBack.GetAddressOf(),&cutReason) && outBack &&
              observedOutW==32 && observedOutH==32,
              "dropping the override returns to the route's evaluation grid");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+3,
              "returning to the default E reallocates back");
        edvr::FlatMonoResolvePreflight cutPlan{};cutPlan.renderWidth=w;cutPlan.renderHeight=h;
        cutPlan.outputWidth=32;cutPlan.outputHeight=32;cutPlan.mode=fc.mode;
        cutPlan.evalWidth=cutPlan.evalHeight=24;
        cutPlan.colorViewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;cutPlan.depthViewFormat=DXGI_FORMAT_R32_FLOAT;
        auto cutPreflight=edvr::flatMonoResolvePreflight(device.Get(),context.Get(),cutPlan);
        check(cutPreflight.readyForRasterJitter() && cutPreflight.spatialFallbackReady,
              "preflight carrying the negotiated E allocates at the cut size");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+4,
              "the cut-E preflight reallocates from the default-sized cache");
        ++fc.frame;fc.evalWidth=fc.evalHeight=24;
        bindOriginal();ComPtr<ID3D11ShaderResourceView> outPreflighted;cutReason=nullptr;
        check(edvr::flatMonoResolve(device.Get(),context.Get(),fc,outPreflighted.GetAddressOf(),&cutReason) && outPreflighted &&
              observedOutW==24 && observedOutH==24,
              "the preflighted cut-E frame resolves on the negotiated grid");
        check(edvr::flatMonoResolveStats().allocations==cutBase.allocations+4,
              "the first treated frame hits the preflighted cache instead of reallocating");
    }
    context->ClearState();
    failures+=flatPixelCaptureGpuTests(device.Get(),context.Get());
    failures+=flatDrawCaptureGpuTests(device.Get(),context.Get());
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T n=0;messages->GetMessage(i,nullptr,&n);std::vector<unsigned char> bytes(n);
        auto* msg=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());messages->GetMessage(i,msg,&n);
        if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::printf("D3D: %s\n",msg->pDescription);check(false,"no D3D resource hazards/errors/warnings");}}
    stats=edvr::flatMonoResolveStats();edvr::flatMonoResolveReset();
    auto afterReset=edvr::flatMonoResolveStats();
    check(afterReset.fullResets==stats.fullResets+1 && afterReset.acceptedResets==stats.acceptedResets &&
          afterReset.acceptedContinues==stats.acceptedContinues && afterReset.currentContinueRun==0,
          "full renderer reset preserves session diagnostics");
    context->ClearState();
    if(!failures)std::puts("flat mono resolve: PASS");return failures?1:0;
}
