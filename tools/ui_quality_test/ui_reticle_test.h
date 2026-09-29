// Controlled geometry, with the saved 71DD/2D03 shader operations and
// 174605 DC state. This proves projection/seed/colour parity; it is not a
// replay of the flight's vertices (the existing snapshot did not retain them).
const char kReticleTestHlsl[] = R"HLSL(
cbuffer Model : register(b0) { float4 model[8]; };
cbuffer Light : register(b1) { float4 light[91]; };
struct Input { float3 p:POSITION; float3 tc:TEXCOORD0; float3 edge:TEXCOORD1; float3 rgb:TEXCOORD2; };
struct Output { float4 edge:TEXCOORD6; float3 rgb:TEXCOORD7; float4 p:SV_POSITION; };
Output vsMain(Input i) {
    Output o; o.edge=float4(i.edge,i.tc.z); o.rgb=i.rgb;
    o.p=float4(dot(model[4],float4(i.p,1)),dot(model[5],float4(i.p,1)),0,dot(model[7],float4(i.p,1)));
    return o;
}
float4 psMain(Output i):SV_TARGET {
    bool border=min(min(i.edge.x,i.edge.y),i.edge.z)<0.1 || i.edge.w>0.5;
    return float4(border ? i.rgb*light[84].z*light[61].y*light[90].y : float3(0,0,0),1);
}
)HLSL";

Tex reticleHdrTex(Gpu& g,unsigned w,unsigned h) {
    Tex t;t.w=w;t.h=h;D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;
    d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.SampleDesc.Count=1;
    d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    g.dev->CreateTexture2D(&d,nullptr,&t.tex);
    if(t.tex)g.dev->CreateRenderTargetView(t.tex.Get(),nullptr,&t.rtv);
    return t;
}
std::vector<uint16_t> readReticleHdr(Gpu& g,const Tex& t) {
    D3D11_TEXTURE2D_DESC d{};t.tex->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;
    d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> copy;
    check(SUCCEEDED(g.dev->CreateTexture2D(&d,nullptr,&copy)),"reticle HDR readback resource");
    if(!copy)return {};g.ctx->CopyResource(copy.Get(),t.tex.Get());D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(g.ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&m)),"reticle HDR readback maps");
    if(!m.pData)return {};std::vector<uint16_t> out(size_t(t.w)*t.h*4);
    for(unsigned y=0;y<t.h;++y)std::memcpy(out.data()+size_t(y)*t.w*4,
        static_cast<const uint8_t*>(m.pData)+size_t(y)*m.RowPitch,size_t(t.w)*8);
    g.ctx->Unmap(copy.Get(),0);return out;
}

void testReticleGpu(Gpu& g) {
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
    if(SUCCEEDED(g.dev.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&adapterDesc)))
        std::printf("  reticle adapter: %ls\n",adapterDesc.Description);
    ComPtr<ID3DBlob> v,p;
    if (!compile(kReticleTestHlsl,sizeof(kReticleTestHlsl)-1,"vsMain","vs_5_0",&v) ||
        !compile(kReticleTestHlsl,sizeof(kReticleTestHlsl)-1,"psMain","ps_5_0",&p)) {
        check(false,"reticle shader operations compile"); return;
    }
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    g.dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs);
    g.dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps);
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32B32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;
    g.dev->CreateInputLayout(elements,4,v->GetBufferPointer(),v->GetBufferSize(),&layout);
    struct Vertex { float p[3],tc[3],edge[3],rgb[3]; } vertices[72]{};
    for(unsigned q=0;q<12;++q) {
        const float x0=-0.8f+float(q%4)*0.4f,y0=-0.7f+float(q/4)*0.5f;
        const unsigned corner[6]={0,1,2,2,1,3};
        for(unsigned k=0;k<6;++k) {
            auto& a=vertices[q*6+k]; const unsigned c=corner[k];
            a.p[0]=x0+((c&1)?0.3f:0); a.p[1]=y0+((c&2)?0.3f:0); a.p[2]=1;
            // Barycentric edge values exercise both PS branches.
            a.edge[k%3]=1; a.rgb[0]=0.125f; a.rgb[1]=0.0625f; a.rgb[2]=0.03125f;
        }
    }
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{vertices,0,0}; ComPtr<ID3D11Buffer> vb,model,light;
    g.dev->CreateBuffer(&bd,&initial,&vb);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.ByteWidth=8*16;
    g.dev->CreateBuffer(&bd,nullptr,&model);bd.ByteWidth=91*16;g.dev->CreateBuffer(&bd,nullptr,&light);
    float lights[91][4]{}; lights[84][2]=lights[61][1]=lights[90][1]=1;
    g.ctx->UpdateSubresource(light.Get(),0,nullptr,lights,0,0);
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc=D3D11_COMPARISON_LESS;dd.StencilEnable=TRUE;dd.StencilReadMask=0x81;dd.StencilWriteMask=1;
    dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_EQUAL};
    dd.BackFace=dd.FrontFace;ComPtr<ID3D11DepthStencilState> ds;g.dev->CreateDepthStencilState(&dd,&ds);
    const UiDsEffect effect=uiLayerDsEffect(uiLayerDsStateFrom(&dd,0),true);
    check(effect.stencilTest && !effect.stencilWrite && !effect.depthWrite && !effect.depthTest,
          "captured reticle state tests stencil81, keeps all planes, depthOFF ignores ALL");
    UiBlendRt original;original.enable=true;original.src=uiblend::kSrcAlpha;original.dst=uiblend::kOne;
    original.op=uiblend::kOpAdd;original.mask=15;UiBlendRt mapped;
    check(uiLayerConvertBlend(original,&mapped),"captured scaled additive reticle blend converts");
    auto blend=[&](const UiBlendRt& b) { D3D11_BLEND_DESC d{};auto& r=d.RenderTarget[0];r.BlendEnable=TRUE;
        r.SrcBlend=static_cast<D3D11_BLEND>(b.src);r.DestBlend=static_cast<D3D11_BLEND>(b.dst);r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=static_cast<D3D11_BLEND>(b.srcA);r.DestBlendAlpha=static_cast<D3D11_BLEND>(b.dstA);
        r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;ComPtr<ID3D11BlendState> out;
        g.dev->CreateBlendState(&d,&out);return out; };
    // The stock alpha equation is SRC_ALPHA/ONE. Layer conversion owns alpha.
    original.srcA=uiblend::kSrcAlpha;original.dstA=uiblend::kOne;
    auto stockBlend=blend(original),layerBlend=blend(mapped);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;
    rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;
    g.dev->CreateRasterizerState(&rd,&raster);
    edvr_layer_seed::Seeder seeder;
    try { seeder.init(g.dev.Get()); } catch(...) { check(false,"reticle seeder initializes");return; }
    check(vs&&ps&&layout&&vb&&model&&light&&ds&&stockBlend&&layerBlend&&raster,"reticle resources initialize");
    if(!vs||!ps||!layout||!vb||!model||!light||!ds||!stockBlend||!layerBlend||!raster)return;
    const float clear[4]={0,0,0,1}; const float jitters[3][2]={{0,0},{.375f,-.25f},{-.4375f,.3125f}};
    for(unsigned eye=0;eye<2;++eye)for(float input:{.5f,.75f})for(float quality:{1.f,1.25f})
    for(const auto& jitter:jitters)for(bool partial:{false,true})for(unsigned stencil:{1u,129u}) {
        const unsigned gameW=static_cast<unsigned>(64*input),gameH=static_cast<unsigned>(48*input);
        const unsigned w=static_cast<unsigned>(64*quality),h=static_cast<unsigned>(48*quality);
        Tex reference=reticleHdrTex(g,w,h),layer=reticleHdrTex(g,w,h);
        check(reference.rtv&&layer.rtv,"reticle real RGBA16_FLOAT HDR targets");
        if(!reference.rtv||!layer.rtv)return;
        Ds gameDs=makeDs(g,gameW,gameH,true),layerDs=makeDs(g,w,h,true),referenceDs=makeDs(g,w,h,true);
        g.ctx->ClearDepthStencilView(gameDs.dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.5f,static_cast<UINT8>(stencil));
        g.ctx->ClearDepthStencilView(referenceDs.dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.5f,static_cast<UINT8>(stencil));
        g.ctx->ClearRenderTargetView(reference.rtv.Get(),clear);g.ctx->ClearRenderTargetView(layer.rtv.Get(),clear);
        seeder.seed(g.ctx.Get(),gameDs.depth.Get(),gameDs.stencil.Get(),layerDs.dsv.Get(),gameW,gameH,w,h,jitter[0],jitter[1],0x81,false);
        const auto map=uiLayerMapFromRegion(0,0,float(gameW),float(gameH),w,h);
        float cx=0,cy=0;uiLayerJitterCancel(jitter[0],jitter[1],map,&cx,&cy);
        UiViewport gameVp;gameVp.x=partial?float(gameW)/8:0;gameVp.y=partial?float(gameH)/8:0;
        gameVp.w=partial?float(gameW)*.75f:float(gameW);gameVp.h=partial?float(gameH)*.75f:float(gameH);
        const auto refVp=uiLayerMapViewport(map,gameVp,0,0),mappedVp=uiLayerMapViewport(map,gameVp,cx,cy);
        const UiRect gameSc{partial?static_cast<int32_t>(gameW/4):0,0,
                           partial?static_cast<int32_t>(gameW*3/4):static_cast<int32_t>(gameW),static_cast<int32_t>(gameH)};
        const auto mappedSc=uiLayerMapScissor(map,gameSc,cx,cy,w,h);
        // Compare at the same mapped scissor; the arithmetic's independent
        // outward-rounding tests own its subpixel edge policy.
        const auto refSc=mappedSc;
        auto draw=[&](const Tex& t,const Ds& d,const UiViewport& vp,const UiRect& sc,bool cancel) {
            float rows[8][4]{};const float clipW=eye?3.f:2.f;
            rows[4][0]=clipW;rows[5][1]=clipW;rows[7][3]=clipW;
            rows[4][3]=(eye?.05f:-.05f)*clipW; // different eye projection
            if(cancel) {rows[4][3]+=2*jitter[0]/gameVp.w*clipW;rows[5][3]-=2*jitter[1]/gameVp.h*clipW;}
            g.ctx->UpdateSubresource(model.Get(),0,nullptr,rows,0,0);
            ID3D11RenderTargetView* rt=t.rtv.Get();g.ctx->OMSetRenderTargets(1,&rt,d.dsv.Get());g.ctx->OMSetDepthStencilState(ds.Get(),1);
            g.ctx->OMSetBlendState(cancel?layerBlend.Get():stockBlend.Get(),nullptr,0xffffffff);
            D3D11_VIEWPORT viewport{vp.x,vp.y,vp.w,vp.h,vp.minZ,vp.maxZ};g.ctx->RSSetViewports(1,&viewport);
            D3D11_RECT rect{static_cast<LONG>(sc.l),static_cast<LONG>(sc.t),static_cast<LONG>(sc.r),static_cast<LONG>(sc.b)};
            g.ctx->RSSetScissorRects(1,&rect);g.ctx->RSSetState(raster.Get());g.ctx->IASetInputLayout(layout.Get());
            UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer* vertex=vb.Get();g.ctx->IASetVertexBuffers(0,1,&vertex,&stride,&offset);
            g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);g.ctx->VSSetShader(vs.Get(),nullptr,0);
            g.ctx->VSSetConstantBuffers(0,1,model.GetAddressOf());g.ctx->PSSetShader(ps.Get(),nullptr,0);
            g.ctx->PSSetConstantBuffers(1,1,light.GetAddressOf());g.ctx->Draw(72,0);g.ctx->OMSetRenderTargets(0,nullptr,nullptr);
        };
        const auto stencilBefore=readBackDs(g,gameDs);
        draw(reference,referenceDs,refVp,refSc,false);draw(layer,layerDs,mappedVp,mappedSc,true);
        const auto a=readReticleHdr(g,reference),b=readReticleHdr(g,layer);
        check(a.size()==size_t(w)*h*4&&b.size()==a.size(),"reticle complete HDR planes read back");
        if(a.size()!=size_t(w)*h*4||b.size()!=a.size())return;
        unsigned lit=0;bool same=true,clipped=true,uncovered=true;
        for(size_t i=0;i<a.size();i+=4) {lit+=a[i]!=0;for(unsigned c=0;c<3;++c)if(a[i+c]!=b[i+c])same=false;
            const int x=static_cast<int>((i/4)%w),y=static_cast<int>((i/4)/w);
            if(b[i] && (x<mappedSc.l||x>=mappedSc.r||y<mappedSc.t||y>=mappedSc.b))clipped=false;
            if(b[i+3]!=0x3c00)uncovered=false; // half-float1, scalar transmittance
        }
        check(same,"reticle exact RGB parity: row4/5/7 realw,z0,seed81,eyes,input/UI sizes,jitter,partial viewport/scissor");
        check(stencil==1?lit>0:lit==0,"reticle stencil81 compares both bits (ref1 passes1, rejects129)");
        check(clipped,"reticle partial scissor actually clips the marker geometry");
        check(uncovered,"additive reticle changes no HDR transmittance, including black interiors");
        check(stencilBefore==readBackDs(g,gameDs),"reticle take leaves original depth/stencil unchanged");
    }
    bool failed=false;try {seeder.seed(g.ctx.Get(),nullptr,nullptr,nullptr,1,1,1,1,0,0,0x81,false);}catch(...){failed=true;}
    check(failed,"reticle unavailable seed fails before rendering");
    // The production decision keeps a failed seed in the original frame.
    UiLayerDrawFacts facts;facts.family=UiLayerFamily::kHoloGeneric;facts.eyeTarget=true;facts.ldrView=false;facts.crispHdr=true;
    facts.eye=0;facts.targetMatchesEye=true;facts.armed=true;facts.blend=UiBlendShape::kScaledAdditive;facts.ds=effect;
    facts.dsReproducible=false;
    check(uiLayerDecide(facts)==UiLayerDecision::kDepthStencilTest,"unavailable reticle stencil seed refuses to stock");
    // Measure the real fallback commands for old depth-only HUD -> game
    // stencil writer -> newly admitted reticle. No eager expanded seed.
    FreshWorld route(g,true);
    g.ctx->ClearDepthStencilView(route.games[0].dsv.Get(),3,.75f,1);
    check(route.seed(0,0,0,true),"reticle route starts with existing depth-only HUD seed");
    D3D11_DEPTH_STENCIL_DESC writer{};writer.DepthEnable=FALSE;writer.StencilEnable=TRUE;
    writer.StencilWriteMask=1;writer.StencilReadMask=255;
    writer.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
    writer.BackFace=writer.FrontFace;const float all[4]={-1,-1,1,1};
    route.gameWriter(0,writer,all);
    auto& cache=route.caches[0];const bool current=cache.seq==route.seq;
    check(route.seeder.usesSpecifiedStencilRef()?!current:current,
          "game stencil writer preserves only old depth-only fallback; specified full seed invalidates");
    const unsigned copies=route.seedCopies,draws=route.seedDraws,clears=route.seedClears;
    const bool depth=current && cache.depth;
    check(route.seed(0,0,0x81,depth),"reticle mask growth samples source after the intervening game writer");
    const unsigned wantDraws=route.seeder.usesSpecifiedStencilRef()?1u:(depth?3u:2u);
    check(route.seedCopies==copies+1 && route.seedDraws==draws+wantDraws && route.seedClears==clears+1,
          "reticle growth adds one source copy, one stencil clear, and exactly required depth/bit passes");
    const auto grown=readBackDs(g,cache.layer);bool newStencil=true;
    for(size_t i=3;i<grown.size();i+=4)if(grown[i]&0x81)newStencil=false;
    check(newStencil,"reticle imported stencil81 reflects actual late writer, not the old seed");
    route.gameWriter(0,writer,all);
    check(cache.seq==0,"later game writer invalidates nonzero reticle stencil claims");
    route.failNextSeed=true;
    check(!route.seed(0,0,0x81,false)&&cache.seq==0,"failed reticle refresh cannot publish a fresh seed");
    std::printf("  reticle mask growth: specified=%u added copy=1 clear=1 SeedDraw=%u (depth=%u); later writers invalidate\n",
                unsigned(route.seeder.usesSpecifiedStencilRef()),wantDraws,unsigned(depth));
    // Captured order19940: sprite531 -> marker533 -> depth-tested panel534
    // (right eye621/623/624). Compare the exact production seed predicate,
    // including mask/depth inheritance only when current, on both routes.
    // This models a sprite source fallback/other actual source writer, not
    // automatic invalidation by a redirected sprite's raw write-back.
    FreshWorld before(g,true),after(g,true);
    auto prepare=[](FreshWorld& world,uint8_t mask,bool depthTest) {
        auto& c=world.caches[0];
        const bool now=c.seq==world.seq && c.source==world.games[0].tex.Get() &&
            c.seededW==c.layer.w && c.seededH==c.layer.h;
        if(!now || (mask&~c.mask) || (depthTest&&!c.depth))
            return world.seed(0,0,uint8_t(mask|(now?c.mask:0)),depthTest||(now&&c.depth));
        return true;
    };
    D3D11_DEPTH_STENCIL_DESC sprite{};sprite.DepthEnable=FALSE;sprite.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    sprite.DepthFunc=D3D11_COMPARISON_LESS;sprite.StencilEnable=TRUE;sprite.StencilReadMask=1;sprite.StencilWriteMask=5;
    sprite.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_EQUAL};
    sprite.BackFace=sprite.FrontFace;
    for(auto* world:{&before,&after}) {
        g.ctx->ClearDepthStencilView(world->games[0].dsv.Get(),3,.75f,1);
        check(prepare(*world,0,true),"captured sequence initial depth-only HUD seed");
        check(prepare(*world,1,false),"captured sprite gets its current stencil01 seed");
        world->gameWriter(0,sprite,all,0,false,5); // actual read01/write05/ref5 raw replay
        check(world->caches[0].seq==0,"source-fallback sprite stencil writer invalidates old seeded copy");
    }
    check(prepare(after,0x81,false),"admitted reticle seeds stencil81 after captured sprite raw writer");
    const auto reticleStencil=readBackDs(g,after.caches[0].layer);
    bool hasCurrentSprite=true;
    for(size_t i=3;i<reticleStencil.size();i+=4)if((reticleStencil[i]&0x81)!=1)hasCurrentSprite=false;
    check(hasCurrentSprite,"reticle sees latest sprite's bit1 (plus current bit80), not stale prior stencil");
    check(prepare(before,0,true)&&prepare(after,0,true),"following captured holo depth test gets current depth on both routes");
    const bool specified=after.seeder.usesSpecifiedStencilRef();
    check(before.seeds==(specified?2u:3u) && after.seeds==(specified?2u:4u),
          "captured sequence reticle adds one fallback seed and zero specified-reference seeds");
    check(after.seedCopies-before.seedCopies==(specified?0u:1u) &&
          after.seedDraws-before.seedDraws==(specified?0u:4u) &&
          after.seedClears-before.seedClears==(specified?0u:1u),
          "captured sequence exact delta: fallback one copy/four SeedDraw/one clear; specified zero");
    check(readBackDs(g,before.games[0])==readBackDs(g,after.games[0]),"captured sequence game planes identical across admission");
    std::printf("  reticle source-stale sprite->marker->panel: specified=%u seeds %u->%u copies %u->%u SeedDraw %u->%u clears %u->%u\n",
        unsigned(specified),before.seeds,after.seeds,before.seedCopies,after.seedCopies,
        before.seedDraws,after.seedDraws,before.seedClears,after.seedClears);
    FreshWorld stableBefore(g,true),stableAfter(g,true);
    for(auto* world:{&stableBefore,&stableAfter}) {
        g.ctx->ClearDepthStencilView(world->games[0].dsv.Get(),3,.75f,5);
        check(prepare(*world,0,true)&&prepare(*world,1,false),"current-depth marker proof includes preceding sprite mask01 growth");
    }
    check(prepare(stableAfter,0x81,false),"current-depth marker imports additional bit80 only when it arrives");
    check(prepare(stableBefore,0,true)&&prepare(stableAfter,0,true),"current-depth later panel needs no extra refresh");
    check(stableAfter.seedCopies-stableBefore.seedCopies==(specified?0u:1u) &&
          stableAfter.seedDraws-stableBefore.seedDraws==(specified?0u:3u) &&
          stableAfter.seedClears-stableBefore.seedClears==(specified?0u:1u),
          "current-depth marker exact delta: fallback one copy/three SeedDraw/one clear; specified zero");
    std::printf("  reticle current-depth sprite->marker->panel: specified=%u seeds %u->%u copies %u->%u SeedDraw %u->%u clears %u->%u\n",
        unsigned(specified),stableBefore.seeds,stableAfter.seeds,stableBefore.seedCopies,stableAfter.seedCopies,
        stableBefore.seedDraws,stableAfter.seedDraws,stableBefore.seedClears,stableAfter.seedClears);
    // Disprove an attractive shortcut: preserving old depth capability on
    // a stale stencil-only marker refresh can change a later consumer after
    // an admitted writable-private draw on an original read-only depth view.
    // The existing private guard protects game-writer invalidation; it does
    // not make an early optional depth import equivalent to a later one.
    FreshWorld lateDepth(g,true),earlyDepth(g,true);
    for(auto* world:{&lateDepth,&earlyDepth}) {
        check(world->seed(0,0,0,true),"stale-depth shortcut proof primes old depth capability");
        world->caches[0].seq=0;
    }
    check(lateDepth.seed(0,0,0x81,false)&&earlyDepth.seed(0,0,0x81,true),
          "stale-depth shortcut compares ordinary marker seed versus eager depth retention");
    lateDepth.privateWriter(0,0,true,true);earlyDepth.privateWriter(0,0,true,true);
    check(lateDepth.consume(0,0)&&earlyDepth.consume(0,0),"later depth consumers complete after private read-only-original draw");
    if(!earlyDepth.seeder.usesSpecifiedStencilRef())
        check(readBack(g,lateDepth.caches[0].colour)!=readBack(g,earlyDepth.caches[0].colour),
              "eager stale depth retention changes actual later colour: keep legacy depth growth");
    check(readBackDs(g,lateDepth.games[0])==readBackDs(g,earlyDepth.games[0]),
          "stale-depth counterexample keeps all original game mutation bytes equal");
    g.ctx->ClearState();
}
