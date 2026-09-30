// Included after ui_world_route_test.h (check, the pure pieces) and ui_after_ui_test.h (squeeze, functionBody).
//
// THE VR WORLD ROUTE'S WIRING, by source scan (the rig runs from the repo root; docs/design-flat-temporal-aa-2026-09-23.md,
// section 82). The pure half decides what the layer does for the route; these scans keep the places that call it, and
// the places that must NOT, where the design put them -- the same way ui_after_ui_test.h scans uiLayerNoteOther:
//   * ui_layer.cpp: the gatherer asks the route for the 2D screen family alone; in the route's mode the decision is never a
//     take; Begin re-checks the bindings before it binds the layer and the mipped screen under VrWorldInternalScope; End
//     puts everything back and only then tells the route which eye it took (and nothing else does);
//   * vscreen.cpp: the pending re-issue is scoped to the draw, issued after the game's own draw and before the verdict's
//     undo, and the tail of the game's draw skips the per-eye screen-motion reissues for a draw the route re-issues while
//     the recognition (the naming of the world's source) still runs;
//   * native_temporal.cpp: the eye shift is advertised only while the route does not own the next frame; the layer-only
//     branch sits after the output sizing, answers S_OK and never null, and leaves the eye's history and continuity alone;
//   * native_sharpen.cpp: a layer-only eye is composited first and sharpened after; every other eye keeps today's order.
namespace worldroute {
// ------------------------------------------------------------ 6: the wiring, by source scan

std::string readText(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The squeezed text of one top-level function, or "" (after a failed check).
std::string bodyOf(const std::string& text, const char* signature, const char* what) {
    std::string body;
    char msg[240];
    std::snprintf(msg, sizeof(msg), "%s is found", what);
    if (!afterui::functionBody(text, signature, &body)) {
        check(false, msg);
        return std::string();
    }
    check(true, msg);
    return afterui::squeeze(body);
}

// True when every piece is present in order (each after the one before).
bool inOrder(const std::string& s, std::initializer_list<const char*> pieces, std::string* missing = nullptr) {
    size_t prev = 0;
    for (const char* piece : pieces) {
        const size_t at = s.find(piece, prev);
        if (at == std::string::npos) {
            if (missing) *missing = piece;
            return false;
        }
        prev = at;
    }
    return true;
}

bool has(const std::string& s, const char* piece) { return s.find(piece) != std::string::npos; }
size_t countOf(const std::string& s, const char* piece) {
    size_t n = 0;
    for (size_t at = s.find(piece); at != std::string::npos; at = s.find(piece, at + 1)) ++n;
    return n;
}

void reportOrder(bool ok, const std::string& missing, const char* what) {
    char msg[360];
    std::snprintf(msg, sizeof(msg), "%s%s%s", what, ok ? "" : " -- missing or out of order: ", ok ? "" : missing.c_str());
    check(ok, msg);
}

void testWiringLayer() {
    const std::string text = readText("src/d3d11/ui_layer.cpp");
    check(!text.empty(), "src/d3d11/ui_layer.cpp is readable from the working directory");
    const std::string all = afterui::squeeze(text);
    const std::string decide = bodyOf(text, "bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt", "the layer's gatherer (uiLayerDecide)");
    // The route is asked for the screen family alone, once, in the gatherer.
    check(has(decide, "f.worldRoute=family==UiLayerFamily::kScreen&&vrWorldRouteLayerMayTake();") &&
              countOf(all, "vrWorldRouteLayerMayTake()") == 1,
          "the gatherer asks the route (vrWorldRouteLayerMayTake) for the 2D screen family alone, and nothing else in the layer does");
    std::string missing;
    reportOrder(inOrder(decide, {"uiLayerWorldRouteMode(family,f.worldScreen,f.worldRoute)", "uiLayerWorldCount(d,true)==UiWorldCount::kReissue",
                                 "worldReissuePlan(ctx,f,substituted,seq,jx,jy);", "returnfalse;}", "++g_win.decided[static_cast<size_t>(family)]"},
                        &missing),
                missing, "in the route's mode uiLayerDecide plans the re-issue, returns false (never a take), and counts decisions only after");
    // A refusal in the route's mode is counted as a refusal and named; only a passed draw is planned.
    check(has(decide, "++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];noteFamily(family,d,detail);worldRefuse(f.eye,uiWorldReasonId(d));"),
          "a refusal in the route's mode is counted as that decision, named, and logged as the route's reason");
    check(has(decide, "if(familyInt!=static_cast<int>(UiLayerFamily::kAfterUi)){g_reissue.pending=false;detail::g_uiLayerWorldReissue=false;}"),
          "a real family's decision forgets a pending re-issue; the after-UI retry's decisions leave it alone");
    // Begin: under the internal scope, validating before binding, the layer through the ordinary machinery.
    const std::string begin = bodyOf(text, "bool uiLayerWorldReissueBegin(ID3D11DeviceContext* ctx)", "uiLayerWorldReissueBegin");
    reportOrder(inOrder(begin, {"g_reissue.pending", "VrWorldInternalScopeinternal;", "g_tc.info.resource!=plan.targetRes", "res.Get()==plan.screenRes",
                                "g_draw.decided=true;", "g_draw.family=UiLayerFamily::kScreen;", "g_draw.shape=plan.shape;", "beginGuarded(ctx,0)",
                                "ctx->PSGetShaderResources(0,1,&plan.savedSrv);", "ctx->PSGetSamplers(0,1,&plan.savedSampler);",
                                "vScreenPSSetShaderResourcesRaw(ctx,0,1,&mips);", "ctx->PSSetSamplers(0,1,&sampler);"},
                        &missing),
                missing, "Begin: the internal scope, the decided draw's bindings re-checked, the layer bound by the ordinary machinery, then the mipped screen and the trilinear sampler at PS slot 0");
    check(!has(begin, "vrWorldRouteNoteEyeTaken(") && !has(begin, "returntrue;}}}"),
          "Begin never tells the route an eye was taken (only a re-issue that landed does)");
    check(has(begin, "worldReleaseSaved(plan);worldRefuse(plan.eye,uiWorldReasonId(why));returnfalse;"),
          "a refusal at Begin releases what it held, is counted by reason and named, and leaves the game's state alone");
    // End: everything back first, the layer's own End, and only then the route told of the eye.
    const std::string end = bodyOf(text, "void uiLayerWorldReissueEnd(ID3D11DeviceContext* ctx)", "uiLayerWorldReissueEnd");
    reportOrder(inOrder(end, {"VrWorldInternalScopeinternal;", "worldRestoreSources(ctx,plan);", "uiLayerEnd(ctx);", "worldReleaseSaved(plan);",
                              "worldRefuse(plan.eye,uiWorldReasonId(UiWorldRefuse::kFault));return;", "++g_win.worldReissued;",
                              "g_eye[plan.eye].worldSeq=plan.seq;", "vrWorldRouteNoteEyeTaken(static_cast<uint32_t>(plan.eye),plan.seq);"},
                        &missing),
                missing, "End: the game's texture and sampler back, the layer's own End, the refusal path first, and only then the eye counted and handed to the route");
    check(countOf(all, "vrWorldRouteNoteEyeTaken(") == 1, "vrWorldRouteNoteEyeTaken is called in exactly one place");
    // The plan: the route's refusals before anything is held; the sources under the internal scope, outside the guard.
    const std::string plan = bodyOf(text, "void worldReissuePlan(ID3D11DeviceContext* ctx, const UiLayerDrawFacts& f,", "worldReissuePlan");
    reportOrder(inOrder(plan, {"if(substituted){why=UiWorldRefuse::kCurved;}", "f.ds.tests()||f.ds.writes()", "f.blend!=UiBlendShape::kOpaque",
                               "VrWorldInternalScopeinternal;", "guardedBudget(g_worldBudget,", "worldReissueSources(ctx,seq,&plan)",
                               "plan.pending=true;g_reissue=plan;detail::g_uiLayerWorldReissue=true;"},
                        &missing),
                missing, "the plan refuses a curved, depth-tested or blended screen draw, then asks the mips module under the internal scope and the guard, and only then holds the draw");
    const std::string sources = bodyOf(text, "UiWorldRefuse worldReissueSources(ID3D11DeviceContext* ctx,", "worldReissueSources");
    reportOrder(inOrder(sources, {"ctx->PSGetShaderResources(0,1,&srv);", "returnUiWorldRefuse::kNoSource;", "ctx->PSGetSamplers(0,1,&smp);",
                                  "returnUiWorldRefuse::kNoSampler;", "vrWorldMipsScreen(ctx,tex.Get(),seq)", "returnUiWorldRefuse::kMipsNull;",
                                  "vrWorldMipsSampler(dev.Get(),sd)", "returnUiWorldRefuse::kSamplerNull;"},
                        &missing),
                missing, "the sources: the draw's own PS slot 0 texture and sampler, the mipped screen for this frame, a sampler made like the game's");
    // The boundary and shutdown forget a pending re-issue.
    const std::string boundary = bodyOf(text, "void uiLayerFrameBoundary(ID3D11DeviceContext* ctx)", "uiLayerFrameBoundary");
    check(has(boundary, "worldReissueReset();"), "the frame boundary drops a pending re-issue");
    // The accessors the route reads.
    check(has(all, "booluiLayerWorldScreenHeld(){returng_screenHeld==1;}") && has(all, "booluiLayerLiveForWorldRoute(){returndetail::g_uiLayerLive;}") &&
              has(all, "detail::g_uiLayerLive=uiLayerLiveFor(g_target,g_temporal,g_jitterAsShipped,g_stoodDown);"),
          "the route's accessors read the gate and the layer's liveness, and the layer's liveness is uiLayerLiveFor's");
}

void testWiringVscreen() {
    const std::string text = readText("src/d3d11/vscreen.cpp");
    check(!text.empty(), "src/d3d11/vscreen.cpp is readable from the working directory");
    std::string missing;
    const std::string reissue = bodyOf(text, "__declspec(noinline) void worldScreenReissue(", "worldScreenReissue");
    reportOrder(inOrder(reissue, {"if(uiLayerIssueBlocked())return;", "VrWorldInternalScopeinternal;", "if(uiLayerWorldReissueBegin(self)){",
                                  "GpuCensusScopecensus(self,GpuCensusSection::FrameWorldLayer);", "pureDrawReissue(self,kind,count,instances,args);",
                                  "uiLayerWorldReissueEnd(self);"},
                        &missing),
                missing, "worldScreenReissue: the internal scope, Begin, the census scope (FrameWorldLayer), the game's draw once more, End");
    const std::string fwd = bodyOf(text, "void forwardWithVerdict(ID3D11DeviceContext* self, DrawVerdict v,", "forwardWithVerdict");
    reportOrder(inOrder(fwd, {"structWorldReissueScope{boolon=false;~WorldReissueScope(){if(on)uiLayerWorldReissueAbandon();}}worldReissue;",
                              "uiLayer=uiLayerDecide(self,static_cast<int>(uiFamily)", "worldReissue.on=uiLayerWorldReissuePending();",
                              "uiLayer=uiLayerNoteOther(", "if(uiLayer&&worldReissue.on){worldReissue.on=false;uiLayerWorldReissueAbandon();}",
                              "constbooloriginalIssued=observedDraw(", "crispHudTonemapReissue(self,kind,count,instances,args);",
                              "if(worldReissue.on&&originalIssued){worldScreenReissue(self,kind,count,instances,args);}", "forwardVerdictEnd(self,v);"},
                        &missing),
                missing,
                "forwardWithVerdict: the pending re-issue is scoped to the call, taken from the decision, dropped if the after-UI retry took the draw, issued after the game's own draw and the crisp re-issue, before the verdict is undone");
    const std::string tail = bodyOf(text, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(", "hookedDrawIndexedInstanced");
    reportOrder(inOrder(tail, {"if(screenMotionLive()&&uiLayerWorldReissuePending())screenMotionRecognize();",
                               "if(screenMotionLive()&&!uiLayerRedirecting()&&!uiLayerWorldReissuePending()){", "screenMotionUiDraw(self,", "screenMotionDraw(self,"},
                        &missing),
                missing, "the tail of the game's draw: a draw the route re-issues runs the recognition alone, and skips the per-eye screen motion reissues (the two existing calls are untouched, behind one more term)");
    check(countOf(afterui::squeeze(text), "screenMotionRecognize()") == 1, "the recognition is called in exactly one place in vscreen.cpp");
}

void testWiringDoor() {
    const std::string temporalText = readText("src/d3d11/native_temporal.cpp");
    check(!temporalText.empty(), "src/d3d11/native_temporal.cpp is readable from the working directory");
    std::string missing;
    const std::string begin = bodyOf(temporalText, "HRESULT WINAPI begin(void* p,", "native_temporal begin()");
    check(has(begin, "s->currentSettings.on&&!s->standDown&&s->currentSettings.jitter&&!edvr::vrWorldRouteOwnsNextFrame())for(inte=0;e<2;++e)"),
          "begin(): the eye shift is advertised only while the route does not own the next frame (the gate gains one lock-free read)");
    check(countOf(afterui::squeeze(temporalText), "vrWorldRouteOwnsNextFrame()") == 1, "...and that is the only place native_temporal asks");
    const std::string treat = bodyOf(temporalText, "HRESULT WINAPI treat(void* p,uint64_t seq,", "native_temporal treat()");
    reportOrder(inOrder(treat, {"floorOutput(*s,eye,w,h,outW,outH);", "if(layerOnlyTreat(*s,seq,eye,outW?outW:w,outH?outH:h,d.Format,b,output,outBox)){s->treated[eye]=true;returnS_OK;}",
                                "s->verdictPending[eye]", "void*raw=edvrTemporalAa("},
                        &missing),
                missing, "treat(): the layer-only branch sits after the output sizing (the served floor's cut included) and before the history and the pass, and answers S_OK");
    check(!has(treat.substr(0, treat.find("void*raw=edvrTemporalAa(")), "s->continuity[eye]=") && !has(treat.substr(0, treat.find("if(layerOnlyTreat(")), "hst.valid=true"),
          "...and leaves the eye's continuity alone (a later eye-route frame finds it broken and resets the history)");
    const std::string helper = bodyOf(temporalText, "bool layerOnlyTreat(State& s,", "layerOnlyTreat");
    reportOrder(inOrder(helper, {"if(!edvr::vrWorldRouteDoorLayerOnly(eye,seq))returnfalse;", "blankFrame(s,eye,outW,outH,format)", "edvr::uiLayerWorldDoorGap(seq,eye,frame)",
                                 "returnfalse;}", "frame->AddRef();*output=frame;", "edvr::uiLayerNoteTemporal(seq,eye,frame);", "++s.layerOnly;", "returntrue;}"},
                        &missing),
                missing, "layerOnlyTreat: only for an eye the route took, the black frame, the layer's preflight (a gap leaves the eye to the pass), then the frame handed on and noted to the layer");
    check(!has(helper, "returnS_FALSE") && !has(helper, "returnE_") && !has(helper, "*output=nullptr") && !has(helper, "s.continuity") && !has(helper, "s.history"),
          "...and it has no S_FALSE, no error, no null output, and touches neither continuity nor history");
    const std::string blank = bodyOf(temporalText, "ID3D11Texture2D* blankFrame(State& s,", "blankFrame");
    check(has(blank, "BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;") && has(blank, "black[4]={0.f,0.f,0.f,1.f};edvr::vScreenClearRenderTargetViewRaw(ctx,rtv,black);") &&
              has(blank, "other.texture->AddRef();mine=other;returnmine.texture;"),
          "the black frame: made once per (size, format), cleared once to (0, 0, 0, 1) when made, shared by the eyes while they agree, never cleared again");
    check(has(afterui::squeeze(temporalText), "releaseBlank(s->blank[0]);releaseBlank(s->blank[1]);"), "...and released at close");

    const std::string sharpenText = readText("src/d3d11/native_sharpen.cpp");
    check(!sharpenText.empty(), "src/d3d11/native_sharpen.cpp is readable from the working directory");
    const std::string sharpen = bodyOf(sharpenText, "HRESULT WINAPI treat(void* p,uint64_t seq,", "native_sharpen treat()");
    const size_t only = sharpen.find("if(edvr::vrWorldRouteDoorLayerOnly(eye,seq)){");
    const size_t ordinary = sharpen.rfind("if(s->strength<=0.f||s->stoodDown){");
    check(only != std::string::npos && ordinary != std::string::npos && only < ordinary, "sharpen: the layer-only branch precedes the ordinary path");
    if (only != std::string::npos && ordinary != std::string::npos && only < ordinary) {
        const std::string layerOnly = sharpen.substr(only, ordinary - only);
        const size_t composite = layerOnly.find("edvr::uiLayerComposite(seq,eye,source,region,layerUv)");
        const size_t rcas = layerOnly.find("edvrSharpen(layered,int(eye),full,s->strength)");
        check(composite != std::string::npos && rcas != std::string::npos && composite < rcas,
              "sharpen: for a layer-only eye the UI layer is composited FIRST and RCAS runs over the composited eye");
        check(has(layerOnly, "++s->layerOnlyBlack;") && has(layerOnly, "returnS_FALSE;") && has(layerOnly, "s->layerOnlyBlack<=8"),
              "sharpen: a composite that does not run for a layer-only eye is counted, and the first eight named");
        const std::string rest = sharpen.substr(ordinary);
        check(rest.find("edvrSharpen(source,int(eye),bounds,s->strength)") < rest.find("edvr::uiLayerComposite(seq,eye,result,whole,layerUv)"),
              "sharpen: every other eye keeps the ordinary order (RCAS on the frame, then the layer over it)");
    }
    check(countOf(sharpen, "vrWorldRouteDoorLayerOnly(") == 1, "sharpen asks the route once");
}

void testWiring() {
    testWiringLayer();
    testWiringVscreen();
    testWiringDoor();
}

}  // namespace worldroute
