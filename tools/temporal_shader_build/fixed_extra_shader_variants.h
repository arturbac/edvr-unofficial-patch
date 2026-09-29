#pragma once
// Build-only registry and independent original call contracts.
#include "../../src/d3d11/fixed_extra_shader_source.h"
static const D3D_SHADER_MACRO resolveWhite[]={{"PROBE_WHITE","1"},{nullptr,nullptr}};
static const D3D_SHADER_MACRO sunNoGate[]={{"NOGATE","1"},{nullptr,nullptr}};
static const D3D_SHADER_MACRO sunAllWorld[]={{"ALLWORLD","1"},{nullptr,nullptr}};
static const D3D_SHADER_MACRO sunAllFlat[]={{"ALLFLAT","1"},{nullptr,nullptr}};
static std::vector<Variant> extraVariants() { return {
    {"kBackdropBytecode","backdrop deband","main",nullptr,{},false,edvr::fixed_extra_source::backdrop_fix::kBackdropCsHlsl,"cs_5_0"},
    {"kMenuCompositeBytecode","menu_panel_cs","main",nullptr,{},false,edvr::fixed_extra_source::menu_panel::kCompositeCs,"cs_5_0"},
    {"kSplashDimBytecode","splash_dim_ps","main",nullptr,{},false,edvr::fixed_extra_source::splash_dim::kPsHlsl,"ps_5_0"},
    {"kFssHealBytecode","fss_heal_cs","main",nullptr,{},false,edvr::fixed_extra_source::fss_heal::kHealCsHlsl,"cs_5_0"},
    {"kFssMirrorBytecode","fss_mirror_cs","main",nullptr,{},false,edvr::fixed_extra_source::fss_heal::kMirrorCsHlsl,"cs_5_0"},
    {"kFssSeriesBytecode","fss_series_cs","main",nullptr,{},false,edvr::fixed_extra_source::fss_dump::kSeriesCsHlsl,"cs_5_0"},
    {"kDepthProbeBytecode","depth_probe_cs","main",nullptr,{},false,edvr::fixed_extra_source::depth_probe::kSampleCsHlsl,"cs_5_0"},
    {"kResolveProbeBytecode","resolve_probe_ps","main",nullptr,{},false,edvr::fixed_extra_source::resolve_probe::kProbePsHlsl,"ps_5_0"},
    {"kResolveProbeWhiteBytecode","resolve_probe_ps","main",resolveWhite,{},false,edvr::fixed_extra_source::resolve_probe::kProbePsHlsl,"ps_5_0"},
    {"kSharpenBytecode","render_sharpen_cs","main",nullptr,{},false,edvr::fixed_extra_source::sharpen_pass::kRcasSource.c_str(),"cs_5_0"},
    {"kHudSpriteEasuBytecode","hud sprite easu","main",nullptr,{},false,edvr::fixed_extra_source::hud_sprite::kEasuSource.c_str(),"cs_5_0"},
    {"kHudSpriteRcasBytecode","hud sprite rcas","main",nullptr,{},false,edvr::fixed_extra_source::hud_sprite::kRcasSource.c_str(),"cs_5_0"},
    {"kPanelEasuBytecode","panel easu","main",nullptr,{},false,edvr::fixed_extra_source::panel_upscale::kEasuSource.c_str(),"cs_5_0"},
    {"kPanelRcasBytecode","panel rcas","main",nullptr,{},false,edvr::fixed_extra_source::panel_upscale::kRcasSource.c_str(),"cs_5_0"},
    {"kIntroEasuBytecode","intro easu","main",nullptr,{},false,edvr::fixed_extra_source::intro_upscale::kEasuSource.c_str(),"cs_5_0"},
    {"kIntroRcasBytecode","intro rcas","main",nullptr,{},false,edvr::fixed_extra_source::intro_upscale::kRcasSource.c_str(),"cs_5_0"},
    {"kIntroCubicBytecode","intro cubic","main",nullptr,{},false,edvr::fixed_extra_source::intro_upscale::kCubicHlsl,"cs_5_0"},
    {"kIntroDebandBytecode","intro deband","main",nullptr,{},false,edvr::fixed_extra_source::intro_upscale::kDebandHlsl,"cs_5_0"},
    {"kParticleWorldBytecode","particle_vs","main",nullptr,{},false,edvr::kParticleWorldVS,"vs_5_0"},
    {"kFlareWorldBytecode","flare_vs","main",nullptr,{},false,edvr::kFlareWorldVS,"vs_5_0"},
    {"kSunglareDefaultBytecode","sunglare_world_vs","main",nullptr,{},false,edvr::kSunglareWorldVS,"vs_5_0"},
    {"kSunglareNoGateBytecode","sunglare_world_vs","main",sunNoGate,{},false,edvr::kSunglareWorldVS,"vs_5_0"},
    {"kSunglareAllWorldBytecode","sunglare_world_vs","main",sunAllWorld,{},false,edvr::kSunglareWorldVS,"vs_5_0"},
    {"kSunglareAllFlatBytecode","sunglare_world_vs","main",sunAllFlat,{},false,edvr::kSunglareWorldVS,"vs_5_0"},
}; }
static std::vector<LegacyContract> extraLegacyContracts() { return {
    {"backdrop deband","main","cs_5_0",nullptr,0x6D209E9C37D3C19Bull},
    {"menu_panel_cs","main","cs_5_0",nullptr,0xF74E850A81A3B7B0ull},
    {"splash_dim_ps","main","ps_5_0",nullptr,0x6D672151F841B696ull},
    {"fss_heal_cs","main","cs_5_0",nullptr,0xE8DA07272D9394EBull},
    {"fss_mirror_cs","main","cs_5_0",nullptr,0xCF8ED1F8A5128648ull},
    {"fss_series_cs","main","cs_5_0",nullptr,0x39D22A2CEC2CA3B4ull},
    {"depth_probe_cs","main","cs_5_0",nullptr,0x81178F52E95106A3ull},
    {"resolve_probe_ps","main","ps_5_0",nullptr,0xFD04E71DDC43975Aull},
    {"resolve_probe_ps","main","ps_5_0",resolveWhite,0xFD04E71DDC43975Aull},
    {"render_sharpen_cs","main","cs_5_0",nullptr,0x55C78550EA904C99ull},
    {"hud sprite easu","main","cs_5_0",nullptr,0xD693C1B6B1A51624ull},
    {"hud sprite rcas","main","cs_5_0",nullptr,0xABA93E5D6D353D5Eull},
    {"panel easu","main","cs_5_0",nullptr,0xE55BC2C204C872FEull},
    {"panel rcas","main","cs_5_0",nullptr,0xABA93E5D6D353D5Eull},
    {"intro easu","main","cs_5_0",nullptr,0x4637348A10CA6AD8ull},
    {"intro rcas","main","cs_5_0",nullptr,0xCB221BA6DFFEE904ull},
    {"intro cubic","main","cs_5_0",nullptr,0x996C28B7D66A5782ull},
    {"intro deband","main","cs_5_0",nullptr,0x0517424C1670B7D5ull},
    {"particle_vs","main","vs_5_0",nullptr,0x52A02AF08112E67Aull},
    {"flare_vs","main","vs_5_0",nullptr,0x02E844F657945524ull},
    {"sunglare_world_vs","main","vs_5_0",nullptr,0xC6C3446804DBAB3Eull},
    {"sunglare_world_vs","main","vs_5_0",sunNoGate,0xC6C3446804DBAB3Eull},
    {"sunglare_world_vs","main","vs_5_0",sunAllWorld,0xC6C3446804DBAB3Eull},
    {"sunglare_world_vs","main","vs_5_0",sunAllFlat,0xC6C3446804DBAB3Eull},
}; }
