// galaxy-choya-native: puente Aurora única → motor GLES de Choya. Ver galaxy_choya_bridge.hpp.
// Se compila dentro del objetivo aurora_gx (con las rutas internas de Aurora: -iquote <aurora>/lib).
#include "galaxy_choya_bridge.hpp"
#include "galaxy_choya_host.hpp"

#include "gfx/clear.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/texture.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"

#include "choya_gx_core.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace galaxy_choya {
namespace core = ::choya::gx;
namespace gx = ::aurora::gx;
namespace gfx = ::aurora::gfx;

namespace {

// ---------------------------------------------------------------- estado
// Cabecera que el núcleo lee por posición fija en cada bloque de uniformes (choya_gx_core.cpp, build_sources y
// attr_address): palabra 0 vtx_start, 1 matriz de posición actual, 2-3 tamaño del viewport (u[0].zw), 8.. comienzos
// de los arreglos indexados (8 + atributo - GX_VA_POS). 20 palabras; el bloque de Aurora va a continuación.
constexpr uint32_t kHeaderBytes = 80;
constexpr uint32_t kHeaderWords = kHeaderBytes / 4;
constexpr uint32_t kBlockAlign = 256;  // máximo habitual de GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
constexpr uint32_t kFogBytes = 32;     // color + (a, b, c, desplazamiento) como los lee el núcleo
constexpr uint32_t kIndBytes = gx::MaxIndTexMtxs * 32;  // 2 ivec4 por matriz

constexpr uint32_t kNone = UINT32_MAX;

// Lo que hace falta por pipeline para reempaquetar sus uniformes.
struct PipeLayout {
  uint32_t uniformSize = 0;     // tamaño del bloque de Aurora (build_shader_info, alineado)
  uint32_t payload = 0;         // bytes que escribe fill_uniform (sin el relleno de alineación)
  uint32_t fogSrc = kNone;      // bytes dentro del bloque de Aurora
  uint32_t indSrc = kNone;
  uint32_t fogDst = kNone;      // bytes dentro del bloque reempaquetado
  uint32_t indDst = kNone;
  uint32_t blockSize = 0;       // tamaño del bloque reempaquetado
  bool lines = false;
};

std::mutex gMutex;  // registros (hilo del GX) frente a la captura (hilo que cierra el cuadro)
std::unordered_map<uint64_t, PipeLayout> gPipes;
std::unordered_map<uintptr_t, std::weak_ptr<gfx::TextureRef>> gTracked;
std::unordered_set<const void*> gPendingDisplayDests;
std::unordered_set<uintptr_t> gDisplayTextures;  // texturas destino de GXCopyDisp en el cuadro en curso
Stats gStats;
std::atomic<bool> gUsed{false};

// Búferes del cuadro (se reutilizan).
std::vector<uint8_t> gUniforms;
std::vector<uint8_t> gIndices;

// Operaciones del FramePacket ya entregadas al núcleo en un segmento (lectura del EFB) del cuadro en curso.
uint64_t gCursorFrame = UINT64_MAX;
size_t gCursorOp = 0;

// Lecturas del EFB pedidas y servidas. Ids de copia propios del puente (bit 62; las texturas de Aurora son punteros
// y la ruta directa usa el bit 63).
constexpr uint64_t kSnapIdBase = uint64_t{1} << 62;
struct Readback {
  uint64_t ticket;
  SnapshotReady ready;
  uint32_t width, height;
};
struct Snapshot {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> depth, argb;
};
std::mutex gSnapMutex;
std::deque<Readback> gPendingReadbacks;
std::map<uint64_t, Snapshot> gSnapshots;

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "[choya-puente] %s\n", buf);
}

// Una vez por motivo: lo que el puente aproxima o no traduce.
void note_once(const char* key, const char* what) {
  static std::mutex m;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(m);
  if (seen.insert(key).second) log_line("%s", what);
}

uint16_t w16(uint32_t bytes) { return static_cast<uint16_t>(bytes / 4); }

uint8_t wrap_mode(wgpu::AddressMode m) {
  return m == wgpu::AddressMode::Repeat ? 1 : m == wgpu::AddressMode::MirrorRepeat ? 2 : 0;
}

bool combiner_uses_texture(const gx::TevStage& st) {
  const auto& c = st.colorPass;
  const auto& a = st.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

// gx::PipelineConfig (Aurora única) → PipelineDesc del núcleo + disposición de uniformes.
core::PipelineDesc describe(const gx::PipelineConfig& c, PipeLayout& L) {
  core::PipelineDesc d{};
  const auto& s = c.shaderConfig;
  const gx::ShaderInfo info = gx::build_shader_info(s);
  static_assert(gx::MaxVtxAttr <= 26, "PipelineDesc.attrs se quedó corto");
  for (size_t i = 0; i < gx::MaxVtxAttr; ++i) {
    const auto& a = s.attrs[i];
    d.attrs[i] = {a.attrType, a.cnt, a.compType, a.offset, a.stride, a.frac, static_cast<uint8_t>(a.le ? 1 : 0), 0};
  }
  // ext.valid: attrs[NRM].pad es el número de índices de la normal (3 con GX_NRM_NBT3), como aurora-main.
  d.attrs[GX_VA_NRM].pad = s.attrs[GX_VA_NRM].nbt3 ? 3 : 1;
  for (size_t i = 0; i < 4; ++i) {
    const auto& cc = s.colorChannels[i];
    d.chans[i] = {static_cast<uint8_t>(cc.matSrc), static_cast<uint8_t>(cc.ambSrc),
                  static_cast<uint8_t>(cc.lightingEnabled ? 1 : 0),
                  static_cast<uint8_t>(0x80u | (static_cast<uint32_t>(cc.diffFn) & 3u) |
                                       ((static_cast<uint32_t>(cc.attnFn) & 3u) << 2))};
  }
  d.vtxStride = s.vtxStride;
  d.lineMode = s.lineMode;
  L.lines = s.lineMode != 0;
  d.sampledChannels = static_cast<uint8_t>(info.sampledColorChannels.to_ulong());
  // Matrices de Aurora única: 10 de posición, 10 de textura y 10 de normales, siempre, en ese orden. Para el núcleo
  // son 20 "postex" (texgen con matriz m → ranura m / 3) seguidas de 10 de normales.
  d.postexCount = static_cast<uint8_t>(gx::MaxPnMtx + gx::MaxTexMtx);
  d.nrmCount = static_cast<uint8_t>(gx::MaxPnMtx);
  d.tevRegLoads = static_cast<uint8_t>(info.loadsTevReg.count());
  d.lightingEnabled = info.lightingEnabled ? 1 : 0;
  d.sampledTexCoords = static_cast<uint8_t>(info.sampledTexCoords.to_ulong());
  uint8_t numTexGens = 0;
  for (uint32_t i = 0; i < gx::MaxTexCoord; ++i) {
    if (s.tcgs[i].src != GX_MAX_TEXGENSRC) numTexGens = static_cast<uint8_t>(i + 1);
  }
  d.numTexGens = numTexGens;

  // TEV.
  d.tevStageCount = static_cast<uint8_t>(std::min<uint32_t>(s.tevStageCount, 16));
  auto& e = d.ext;
  e.valid = 1;
  for (uint32_t i = 0; i < d.tevStageCount; ++i) {
    const auto& st = s.tevStages[i];
    auto& o = d.stages[i];
    o.ca = st.colorPass.a; o.cb = st.colorPass.b; o.cc = st.colorPass.c; o.cd = st.colorPass.d;
    o.cop = st.colorOp.op; o.cbias = st.colorOp.bias; o.cscale = st.colorOp.scale;
    o.cclamp = st.colorOp.clamp ? 1 : 0; o.cout = st.colorOp.outReg;
    o.aa = st.alphaPass.a; o.ab = st.alphaPass.b; o.ac = st.alphaPass.c; o.ad = st.alphaPass.d;
    o.aop = st.alphaOp.op; o.abias = st.alphaOp.bias; o.ascale = st.alphaOp.scale;
    o.aclamp = st.alphaOp.clamp ? 1 : 0; o.aout = st.alphaOp.outReg;
    o.kcSel = st.kcSel; o.kaSel = st.kaSel;
    o.channel = static_cast<uint8_t>(st.channelId);
    o.swapRas = st.tevSwapRas; o.swapTex = st.tevSwapTex;
    const bool hasCoord = st.texCoordId != GX_TEXCOORD_NULL && st.texCoordId < gx::MaxTexCoord;
    const bool hasMap = st.texMapId != GX_TEXMAP_NULL && st.texMapId < 8;
    // Mismo criterio que build_shader_info de Aurora: la etapa muestrea si su textura quedó entre las muestreadas.
    const bool samples = hasCoord && hasMap && info.sampledTextures.test(st.texMapId);
    o.texCoord = samples ? static_cast<uint8_t>(st.texCoordId) : 0xFF;
    o.texMap = samples ? static_cast<uint8_t>(st.texMapId) : 0xFF;
    o.pad = hasCoord ? static_cast<uint8_t>(st.texCoordId) : 0xFF;  // coordenada efectiva (ext.valid)
    const bool indActive = st.indTexStage < s.numIndStages;
    const bool fixedCoord = indActive && (st.indTexMtxId != GX_ITM_OFF || st.indTexWrapS != GX_ITW_OFF ||
                                          st.indTexWrapT != GX_ITW_OFF || st.indTexAddPrev);
    e.tind[i] = {static_cast<uint8_t>(st.indTexStage), static_cast<uint8_t>(st.indTexFormat),
                 static_cast<uint8_t>(st.indTexBiasSel), static_cast<uint8_t>(st.indTexAlphaSel),
                 static_cast<uint8_t>(st.indTexMtxId), static_cast<uint8_t>(st.indTexWrapS),
                 static_cast<uint8_t>(st.indTexWrapT),
                 static_cast<uint8_t>((st.indTexAddPrev ? 1 : 0) | (fixedCoord ? 2 : 0) |
                                      (hasMap && combiner_uses_texture(st) ? 4 : 0))};
  }
  for (size_t i = 0; i < 4; ++i) {
    const auto& sw = s.tevSwapTable[i];
    d.swap[i][0] = sw.red; d.swap[i][1] = sw.green; d.swap[i][2] = sw.blue; d.swap[i][3] = sw.alpha;
  }
  d.alphaComp0 = s.alphaCompare.comp0;
  d.alphaRef0 = static_cast<uint8_t>(s.alphaCompare.ref0);
  d.alphaOp = s.alphaCompare.op;
  d.alphaComp1 = s.alphaCompare.comp1;
  d.alphaRef1 = static_cast<uint8_t>(s.alphaCompare.ref1);
  bool bump = false, postMtx = false;
  for (uint32_t i = 0; i < gx::MaxTexCoord; ++i) {
    const auto& tcg = s.tcgs[i];
    auto& tg = d.texgens[i];
    tg.type = tcg.type;
    tg.src = tcg.src;
    tg.mtxSlot = 0xFF;
    if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
      tg.mtxSlot = 0xFE;  // índice por vértice
    } else if (tcg.mtx != GX_IDENTITY) {
      const uint32_t slot = static_cast<uint32_t>(tcg.mtx) / 3;
      if (slot < gx::MaxPnMtx + gx::MaxTexMtx) tg.mtxSlot = static_cast<uint8_t>(slot);
    }
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7 && info.sampledTexCoords.test(i)) bump = true;
    e.tgFlags[i] = static_cast<uint8_t>(tcg.normalize ? 1 : 0);
    e.tgPost[i] = 0xFF;
    if (tcg.postMtx != GX_PTIDENTITY) {
      e.tgPost[i] = static_cast<uint8_t>((tcg.postMtx - GX_PTTEXMTX0) / 3);
      postMtx = true;
    }
  }
  // Aurora única aplica la matriz posterior siempre que la hay (no tiene bandera dualTex): se le dice al núcleo.
  e.dualTex = postMtx ? 1 : 0;
  e.fogType = s.fogType;
  e.fogRange = 0;  // el rango de niebla de Aurora única es una tabla por píxel en el almacenamiento: no se traduce
  e.zFreeze = 0;
  e.zTexture = 0;
  e.zTexStage = 0xFF;
  e.numIndStages = static_cast<uint8_t>(std::min<uint32_t>(s.numIndStages, 4));
  e.usedIndStages = static_cast<uint8_t>(info.usedIndStages.to_ulong());
  for (uint32_t i = 0; i < 4; ++i) {
    const auto& is = s.indStages[i];
    e.ind[i] = {static_cast<uint8_t>(is.texCoordId == GX_TEXCOORD_NULL ? 0xFF : is.texCoordId),
                static_cast<uint8_t>(is.texMapId), static_cast<uint8_t>(is.scaleS), static_cast<uint8_t>(is.scaleT)};
  }
  if (s.fogRangeEnabled) L.fogSrc = kNone;  // marcado abajo como aproximación
  if (bump) note_once("bump", "texgen de relieve (GX_TG_BUMPn): el núcleo de Choya no lo implementa; se dibuja sin el "
                              "desplazamiento de relieve");

  // Desplazamientos: mismo orden y tamaños que fill_uniform (gx/shader_info.cpp de Aurora única).
  uint32_t b = 16;  // tamaños de viewport de render y lógico
  if (s.lineMode != 0) b += 16;
  const uint32_t H = kHeaderBytes;
  d.offProj = w16(H + b);
  b += 64;
  d.offPostex = w16(H + b);
  b += 48 * (gx::MaxPnMtx + gx::MaxTexMtx + gx::MaxPnMtx);
  for (size_t i = 0; i < 4; ++i) {
    if (info.loadsTevReg.test(i)) {
      d.offTevReg[i] = w16(H + b);
      b += 16;
    }
  }
  if (info.lightingEnabled) b += 80 * GX::MaxLights + 16;
  for (size_t i = 0; i < 2; ++i) {
    if (!info.sampledColorChannels.test(i)) continue;
    const auto& cc = s.colorChannels[i];
    const auto& cca = s.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) b += 16;
    if (cc.matSrc == GX_SRC_REG) {
      d.offChanMat[i] = w16(H + b);
      b += 16;
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) b += 16;
    if (cca.matSrc == GX_SRC_REG) {
      d.offChanMatA[i] = w16(H + b);
      b += 16;
    }
  }
  for (size_t i = 0; i < 4; ++i) {
    if (info.sampledKColors.test(i)) {
      d.offKColor[i] = w16(H + b);
      b += 16;
    }
  }
  if (info.usesPTTexMtx.any()) {
    e.offPostMtx = w16(H + b);
    b += 48 * gx::MaxPTTexMtx;
  }
  if (info.usesFog) {
    L.fogSrc = b;
    b += sizeof(gx::Fog);
  }
  for (size_t i = 0; i < gx::MaxTexCoord; ++i) d.offTexScale[i] = w16(H + b + 16 * static_cast<uint32_t>(i));
  b += 16 * gx::MaxTexCoord;
  if (info.usedIndTexMtxs.any()) {
    L.indSrc = b;
    b += 32 * gx::MaxIndTexMtxs;
  }
  for (size_t i = 0; i < 8; ++i) {
    if (info.sampledTextures.test(i)) {
      d.offTexSize[i] = w16(H + b);
      b += 16;
    }
  }
  L.uniformSize = info.uniformSize;
  L.payload = b;
  if (b > info.uniformSize) {
    log_line("disposición de uniformes inconsistente (%u > %u): el pipeline no se registra", b, info.uniformSize);
    L.uniformSize = 0;
  }
  uint32_t end = H + info.uniformSize;
  if (L.fogSrc != kNone) {
    L.fogDst = end;
    e.offFog = w16(end);
    end += kFogBytes;
  }
  if (L.indSrc != kNone) {
    L.indDst = end;
    e.offIndMtx = w16(end);
    end += kIndBytes;
  }
  L.blockSize = end;

  // Estado de render.
  d.depthCompare = c.depthCompare;
  d.depthUpdate = c.depthUpdate;
  d.colorUpdate = c.colorUpdate;
  d.alphaUpdate = c.alphaUpdate;
  d.depthFunc = static_cast<uint8_t>(c.depthFunc);
  d.cullMode = static_cast<uint8_t>(c.cullMode);
  d.blendMode = static_cast<uint8_t>(c.blendMode);
  d.blendSrc = static_cast<uint8_t>(c.blendFacSrc);
  d.blendDst = static_cast<uint8_t>(c.blendFacDst);
  d.blendOp = static_cast<uint8_t>(c.blendOp);
  // Aurora única ya anula alphaUpdate si el EFB no tiene alfa (populate_pipeline_config); el núcleo lo decide con
  // pixelFmt, así que se deduce de ahí.
  d.pixelFmt = static_cast<uint8_t>(c.alphaUpdate ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24);
  return d;
}

// ---------------------------------------------------------------- texturas
void send_texture(uintptr_t id, const gfx::TextureRef& ref, const uint8_t* data, size_t size) {
  const uint32_t w = ref.size.width, h = ref.size.height, mips = std::max<uint32_t>(ref.mipCount, 1);
  size_t texels = 0;
  for (uint32_t m = 0; m < mips; ++m) texels += size_t{std::max(w >> m, 1u)} * std::max(h >> m, 1u);
  switch (ref.format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    if (size < texels * 4) break;
    core::texture_data(id, w, h, mips, data, texels * 4);
    ++gStats.texturesSent;
    return;
  case wgpu::TextureFormat::R8Unorm:
  case wgpu::TextureFormat::RG8Unorm: {
    // Formatos de PC con subida directa: Aurora los ve con swizzle RRRR / RRRG.
    const size_t comps = ref.format == wgpu::TextureFormat::R8Unorm ? 1 : 2;
    if (size < texels * comps) break;
    std::vector<uint8_t> rgba(texels * 4);
    for (size_t t = 0; t < texels; ++t) {
      const uint8_t i = data[t * comps];
      const uint8_t a = comps == 2 ? data[t * comps + 1] : i;
      rgba[t * 4] = rgba[t * 4 + 1] = rgba[t * 4 + 2] = i;
      rgba[t * 4 + 3] = a;
    }
    core::texture_data(id, w, h, mips, rgba.data(), rgba.size());
    ++gStats.texturesExpanded;
    return;
  }
  default:
    break;
  }
  ++gStats.texturesUnsupported;
  note_once("texfmt", "textura en un formato que el puente no envía al núcleo (BC1 directo o índices R16Sint): "
                      "se dibuja sin datos; las de paleta llegan por la conversión de paleta");
}

} // namespace

// ---------------------------------------------------------------- API
bool enabled() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("CHOYA_GLES");
    if (v == nullptr || std::strcmp(v, "1") != 0) return false;
    core::set_reversed_z(gx::UseReversedZ);
    if (const char* pk = std::getenv("CHOYA_PAQUETE"); pk != nullptr && *pk != '\0' && std::strcmp(pk, "0") != 0) {
      core::pack_open(pk);
    }
    log_line("activo: Aurora única → motor GLES de Choya");
    return true;
  }();
  return on;
}

void register_gx_pipeline(uint64_t ref, const gx::PipelineConfig& config) noexcept {
  if (!enabled()) return;
  {
    std::lock_guard lock(gMutex);
    if (gPipes.count(ref) != 0) return;
  }
  PipeLayout layout;
  const core::PipelineDesc desc = describe(config, layout);
  if (std::getenv("GALAXY_BRIDGE_DEBUG")) {
    const auto& st = config.shaderConfig.tevStages[0];
    log_line("pipeline %016llx: etapas %u, etapa0 color %u %u %u %u alfa %u %u %u %u canal %u mapa %u coord %u; "
             "canales muestreados %u; uniformes %u (%u escritos)",
             static_cast<unsigned long long>(ref), config.shaderConfig.tevStageCount, st.colorPass.a, st.colorPass.b,
             st.colorPass.c, st.colorPass.d, st.alphaPass.a, st.alphaPass.b, st.alphaPass.c, st.alphaPass.d,
             st.channelId, st.texMapId, st.texCoordId, desc.sampledChannels, layout.uniformSize, layout.payload);
  }
  if (config.polygonOffsetBits != 0 || config.polygonOffsetScaleBits != 0) {
    ++gStats.approxPolygonOffset;
    note_once("polyoff", "desplazamiento de polígono (polygon offset) de Aurora: el núcleo no lo aplica");
  }
  if (config.shaderConfig.fogRangeEnabled) {
    ++gStats.approxFogRange;
    note_once("fogrange", "niebla con ajuste por rango: se dibuja sin el ajuste (tabla por píxel de Aurora)");
  }
  {
    std::lock_guard lock(gMutex);
    gPipes.emplace(ref, layout);
  }
  if (layout.uniformSize != 0) core::register_pipeline(ref, desc);
}

void register_clear_pipeline(uint64_t ref, const gfx::clear::PipelineConfig& config) noexcept {
  if (!enabled() || core::has_clear(ref)) return;
  core::register_clear(ref, core::ClearDesc{static_cast<uint8_t>(config.clearColor ? 1 : 0),
                                           static_cast<uint8_t>(config.clearAlpha ? 1 : 0),
                                           static_cast<uint8_t>(config.clearDepth ? 1 : 0), 0});
}

void register_bind_group(uint64_t ref, const gx::ShaderInfo& info) noexcept {
  if (!enabled() || ref == 0) return;
  core::BindDesc d{};
  for (uint32_t i = 0; i < gx::MaxTextures && i < 8; ++i) {
    const auto& tex = gx::g_gxState.textures[i];
    if (!tex || !(info.sampledTextures[i] || info.sampledIndTextures[i])) continue;
    d.tex[i] = reinterpret_cast<uintptr_t>(tex.ref.get());
    const auto& r = *tex.ref;
    core::describe_texture(d.tex[i], r.size.width, r.size.height, r.gxFormat, r.mipCount);
    const auto sd = tex.get_descriptor();
    auto& o = d.samp[i];
    o.wrapS = wrap_mode(sd.addressModeU);
    o.wrapT = wrap_mode(sd.addressModeV);
    o.minFilter = sd.minFilter == wgpu::FilterMode::Linear ? 1 : 0;
    o.magFilter = sd.magFilter == wgpu::FilterMode::Linear ? 1 : 0;
    o.mipFilter = sd.mipmapFilter == wgpu::MipmapFilterMode::Linear    ? 2
                  : sd.mipmapFilter == wgpu::MipmapFilterMode::Nearest ? 1
                                                                       : 0;
    o.minLod = sd.lodMinClamp;
    o.maxLod = sd.lodMaxClamp;
  }
  // El mismo ref de Aurora (hash del descriptor de WebGPU) siempre describe las mismas vistas y muestreadores.
  core::register_bind_group(ref, d);
}

void texture_data(const gfx::TextureRef& ref, const uint8_t* data, size_t size) noexcept {
  if (!enabled() || data == nullptr) return;
  send_texture(reinterpret_cast<uintptr_t>(&ref), ref, data, size);
}

std::shared_ptr<gfx::TextureRef> tracked(std::shared_ptr<gfx::TextureRef> handle) noexcept {
  if (!enabled() || !handle) return handle;
  const auto id = reinterpret_cast<uintptr_t>(handle.get());
  std::lock_guard lock(gMutex);
  auto it = gTracked.find(id);
  if (it != gTracked.end() && it->second.expired()) {
    // Otra textura ocupó la dirección de una ya liberada: se suelta la vieja antes de que lleguen datos nuevos.
    core::texture_released(id);
    ++gStats.texturesReleased;
  }
  gTracked[id] = handle;
  return handle;
}

void note_display_dest(const void* dest) noexcept {
  if (!enabled()) return;
  std::lock_guard lock(gMutex);
  gPendingDisplayDests.insert(dest);
}

void note_copy(const void* dest, const gfx::TextureRef* texture) noexcept {
  if (!enabled() || texture == nullptr) return;
  std::lock_guard lock(gMutex);
  if (gPendingDisplayDests.erase(dest) != 0) gDisplayTextures.insert(reinterpret_cast<uintptr_t>(texture));
}

namespace {
// Entrega al núcleo las operaciones del paquete desde la primera no entregada. present: el cuadro se muestra.
// snap: copias de lectura al final (segmento a la hora de un token).
void capture_ops(gfx::detail::FramePacket& packet, bool present, const Readback* snap,
                 const SnapshotArea* area) noexcept {
  using namespace gfx::detail;
  gUsed.store(true, std::memory_order_relaxed);
  std::unordered_set<uintptr_t> displayTex;
  std::unordered_map<uint64_t, PipeLayout> pipes;
  {
    std::lock_guard lock(gMutex);
    displayTex.swap(gDisplayTextures);
    pipes = gPipes;  // copia: los registros pueden seguir llegando desde el hilo del GX
    // Texturas liberadas desde el cuadro anterior.
    for (auto it = gTracked.begin(); it != gTracked.end();) {
      if (it->second.expired()) {
        core::texture_released(it->first);
        ++gStats.texturesReleased;
        it = gTracked.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Índices: los de Aurora tal cual y, detrás, los que hacen falta para los dibujos sin índices (GX_TRIANGLES sin
  // fusionar: Aurora los dibuja con draw(vtxCount)).
  gIndices.assign(packet.indices.data(), packet.indices.data() + packet.indices.size());
  gUniforms.clear();
  const uint8_t* srcUniforms = packet.uniforms.data();
  const size_t srcUniformSize = packet.uniforms.size();

  struct Item {
    enum Kind : uint8_t { Pass, Viewport, Scissor, Draw, Clear, Copy, Palette } kind;
    const RenderPass* pass = nullptr;
    gfx::Viewport vp{};
    gfx::ClipRect rect{};
    const gx::DrawData* draw = nullptr;
    const gfx::clear::DrawData* clear = nullptr;
    uint32_t idxOffset = 0, indexCount = 0, uniformOffset = 0;
    const gfx::tex_palette_conv::ConvRequest* conv = nullptr;
  };
  std::vector<Item> items;
  items.reserve(4096);

  if (gCursorFrame != packet.frameId) {
    gCursorFrame = packet.frameId;
    gCursorOp = 0;
  }
  const size_t firstOp = std::min(gCursorOp, packet.ops.size());
  for (size_t opIndex = firstOp; opIndex < packet.ops.size(); ++opIndex) {
    const FrameOp& op = packet.ops[opIndex];
    if (op.type == FrameOpType::TextureCopy) {
      ++gStats.skippedTextureCopies;
      note_once("texcopy", "copia de textura a textura de Aurora (queue_texture_copy): el núcleo no la tiene");
      continue;
    }
    if (op.type == FrameOpType::EncoderTask) {
      ++gStats.skippedEncoderTasks;
      note_once("enctask", "tarea de codificador registrada en Aurora: no se traduce");
      continue;
    }
    const RenderPass* rp = op.renderPass;
    if (rp == nullptr) continue;
    items.push_back({Item::Pass, rp});
    for (const auto& conv : rp->paletteConvs) {
      Item it{Item::Palette};
      it.conv = &conv;
      items.push_back(it);
    }
    for (const Command& cmd : rp->commands) {
      switch (cmd.type) {
      case CommandType::SetViewport: {
        Item it{Item::Viewport};
        it.vp = cmd.data.setViewport;
        items.push_back(it);
      } break;
      case CommandType::SetScissor: {
        Item it{Item::Scissor};
        it.rect = cmd.data.setScissor;
        items.push_back(it);
      } break;
      case CommandType::Draw: {
        const void* payload = nullptr;
        const int kind = gfx::galaxy_choya_draw_kind(cmd.data.draw, &payload);
        if (kind == 2) {
          Item it{Item::Clear};
          it.clear = static_cast<const gfx::clear::DrawData*>(payload);
          items.push_back(it);
          break;
        }
        if (kind != 1) {
          ++gStats.skippedCustomDraws;
          note_once("otherdraw", "dibujo que no es GX ni borrado (ImGui/RmlUi/tipo registrado): no va al núcleo");
          break;
        }
        const auto& draw = *static_cast<const gx::DrawData*>(payload);
        const auto pit = pipes.find(draw.pipeline);
        if (pit == pipes.end() || pit->second.uniformSize == 0) {
          ++gStats.skippedUnknownPipeline;
          note_once("nopipe", "dibujo con un pipeline que el puente no registró: se omite (contado)");
          break;
        }
        const PipeLayout& L = pit->second;
        // push_uniform sube lo que escribió fill_uniform (sin el relleno hasta uniformSize).
        if (draw.uniformRange.size < L.payload ||
            static_cast<size_t>(draw.uniformRange.offset) + draw.uniformRange.size > srcUniformSize) {
          ++gStats.skippedUniformSize;
          note_once("unisize", "bloque de uniformes de Aurora más corto que su disposición: dibujo omitido (contado)");
          break;
        }
        if (L.lines) ++gStats.lineDraws;
        // Bloque reempaquetado.
        const uint32_t base = static_cast<uint32_t>((gUniforms.size() + kBlockAlign - 1) / kBlockAlign * kBlockAlign);
        gUniforms.resize(base + L.blockSize, 0);
        uint8_t* dst = gUniforms.data() + base;
        const uint8_t* src = srcUniforms + draw.uniformRange.offset;
        const auto& im = draw.immediateData;
        uint32_t head[kHeaderWords] = {};
        head[0] = im.vtxStart;
        head[1] = im.currentPnMtx;
        std::memcpy(&head[2], src, 16);  // render w, h, lógico w, h
        for (uint32_t a = 0; a < gx::MaxIndexAttr; ++a) head[8 + a] = im.arrayStart[a];
        std::memcpy(dst, head, kHeaderBytes);
        std::memcpy(dst + kHeaderBytes, src, std::min<uint32_t>(draw.uniformRange.size, L.uniformSize));
        if (L.fogDst != kNone) {
          // Aurora: base = a / (b - z), z de 0 a 1. Núcleo: a·2^24 / (b' - (z24 >> s)) → b' = b·2^24, s = 0.
          float fog[8];
          std::memcpy(fog, src + L.fogSrc, 32);  // color(4), a, b, c, rangeCenter
          const float out[8] = {fog[0], fog[1], fog[2], fog[3], fog[4], fog[5] * 16777216.f, fog[6], 0.f};
          std::memcpy(dst + L.fogDst, out, sizeof out);
        }
        if (L.indDst != kNone) {
          // Aurora: (m00, m01, m10, m11), (m20, m21, 2^escala, 0) en float. Núcleo: mantisas ×1024 y desplazamiento.
          for (uint32_t m = 0; m < gx::MaxIndTexMtxs; ++m) {
            float f[8];
            std::memcpy(f, src + L.indSrc + 32 * m, 32);
            const int32_t shift = -static_cast<int32_t>(std::lround(std::log2(std::max(f[6], 1e-30f))));
            const int32_t out[8] = {static_cast<int32_t>(std::lround(f[0] * 1024.f)),
                                    static_cast<int32_t>(std::lround(f[1] * 1024.f)),
                                    static_cast<int32_t>(std::lround(f[2] * 1024.f)),
                                    static_cast<int32_t>(std::lround(f[3] * 1024.f)),
                                    static_cast<int32_t>(std::lround(f[4] * 1024.f)),
                                    static_cast<int32_t>(std::lround(f[5] * 1024.f)),
                                    shift,
                                    0};
            std::memcpy(dst + L.indDst + 32 * m, out, sizeof out);
          }
        }
        Item it{Item::Draw};
        it.draw = &draw;
        it.uniformOffset = base;
        if (draw.indexCount != 0) {
          it.idxOffset = draw.idxRange.offset;
          it.indexCount = draw.indexCount;
        } else {
          // Dibujo sin índices: secuencia 0..vtxCount-1 (u16, como prepare_idx_buffer).
          const size_t at = (gIndices.size() + 3) & ~size_t{3};
          gIndices.resize(at + size_t{draw.vtxCount} * 2);
          auto* p = reinterpret_cast<uint16_t*>(gIndices.data() + at);
          for (uint32_t v = 0; v < draw.vtxCount; ++v) p[v] = static_cast<uint16_t>(v);
          it.idxOffset = static_cast<uint32_t>(at);
          it.indexCount = draw.vtxCount;
          ++gStats.generatedIndexDraws;
        }
        items.push_back(it);
      } break;
      case CommandType::CustomDraw:
        ++gStats.skippedCustomDraws;
        note_once("custom", "dibujo personalizado de Aurora (push_custom_draw): no va al núcleo");
        break;
      case CommandType::DebugMarker:
        break;
      }
    }
    if (rp->resolveTarget) {
      Item it{Item::Copy};
      it.pass = rp;
      items.push_back(it);
    }
  }

  core::capture_begin(packet.verts.data(), packet.verts.size(), gUniforms.data(), gUniforms.size(), gIndices.data(),
                      gIndices.size(), packet.storage.data(), packet.storage.size());
  for (const Item& it : items) {
    switch (it.kind) {
    case Item::Pass: {
      const RenderPass& rp = *it.pass;
      const auto& color = rp.colorAttachments[gfx::SceneColorAttachmentIndex];
      const bool display = rp.resolveTarget && displayTex.count(reinterpret_cast<uintptr_t>(rp.resolveTarget.get()));
      const float rgba[4] = {color.clearValue.x(), color.clearValue.y(), color.clearValue.z(), color.clearValue.w()};
      const core::Rect copy = display ? core::Rect{rp.resolveRect.x, rp.resolveRect.y, rp.resolveRect.width,
                                                   rp.resolveRect.height}
                                      : core::Rect{0, 0, 0, 0};
      core::capture_pass(color.size.width, color.size.height, color.clear, rp.clearDepth && rp.hasDepth, rgba,
                         rp.clearDepthValue, display, copy);
      ++gStats.passes;
      if (display) ++gStats.displayCopies;
    } break;
    case Item::Palette: {
      const auto& c = *it.conv;
      if (!c.src || !c.dst || !c.tlut) break;
      const uint32_t variant = c.variant == gfx::tex_palette_conv::Variant::FromFloat8   ? 1
                               : c.variant == gfx::tex_palette_conv::Variant::FromFloat4 ? 2
                                                                                         : 0;
      core::capture_palette_conv(reinterpret_cast<uintptr_t>(c.src.get()), reinterpret_cast<uintptr_t>(c.dst.get()),
                                 reinterpret_cast<uintptr_t>(c.tlut.get()), variant, c.dst->size.width,
                                 c.dst->size.height);
      ++gStats.paletteConvs;
    } break;
    case Item::Viewport:
      core::capture_viewport(core::Viewport{it.vp.left, it.vp.top, it.vp.width, it.vp.height, it.vp.znear, it.vp.zfar});
      break;
    case Item::Scissor:
      core::capture_scissor(core::Rect{it.rect.x, it.rect.y, it.rect.width, it.rect.height});
      break;
    case Item::Draw:
      core::capture_draw(it.draw->pipeline, it.idxOffset, it.indexCount, it.draw->instanceCount, it.uniformOffset,
                         it.draw->dstAlpha, it.draw->bindGroups.textureBindGroup);
      ++gStats.gxDraws;
      break;
    case Item::Clear: {
      const auto& c = *it.clear;
      const float rgba[4] = {static_cast<float>(c.color.r), static_cast<float>(c.color.g),
                             static_cast<float>(c.color.b), static_cast<float>(c.color.a)};
      core::capture_clear(c.pipeline, rgba, c.depth, true, core::Rect{c.rect.x, c.rect.y, c.rect.width, c.rect.height});
      ++gStats.clears;
    } break;
    case Item::Copy: {
      const RenderPass& rp = *it.pass;
      const auto id = reinterpret_cast<uintptr_t>(rp.resolveTarget.get());
      if (displayTex.count(id)) break;  // la copia a pantalla la presenta el pase (hasResolve)
      core::capture_copy(id, core::Rect{rp.resolveRect.x, rp.resolveRect.y, rp.resolveRect.width,
                                        rp.resolveRect.height},
                         rp.resolveTarget->size.width, rp.resolveTarget->size.height,
                         static_cast<uint32_t>(rp.resolveFormat), !gx::efb_has_alpha(rp.resolveSourceFormat));
      ++gStats.copies;
    } break;
    }
  }
  gStats.uniformBytes += gUniforms.size();
  if (snap != nullptr) {
    // Al terminar el último pase: profundidad (Z24X8 → bytes hi, mid, lo en RGB) y color RGBA8 del área lógica.
    const core::Rect r{static_cast<int32_t>(std::lround(area->offsetX)), static_cast<int32_t>(std::lround(area->offsetY)),
                       static_cast<int32_t>(std::lround(area->logicalWidth * area->scaleX)),
                       static_cast<int32_t>(std::lround(area->logicalHeight * area->scaleY))};
    core::capture_copy(kSnapIdBase | (snap->ticket << 1), r, snap->width, snap->height, 0x16 /*GX_TF_Z24X8*/, true);
    core::capture_copy(kSnapIdBase | (snap->ticket << 1) | 1, r, snap->width, snap->height, 0x6 /*GX_TF_RGBA8*/,
                       false);
    core::capture_flush();  // cierra el lote (sin mostrarse) y lo entrega ya
    gCursorOp = packet.ops.size();
    ++gStats.segments;
    return;
  }
  core::capture_end(present);
  gCursorOp = 0;
  gCursorFrame = UINT64_MAX;
}
} // namespace

void capture_packet(gfx::detail::FramePacket& packet) noexcept {
  if (!enabled()) return;
  capture_ops(packet, true, nullptr, nullptr);
  host::notify();
  ++gStats.frames;
  if (gStats.frames == 1 || gStats.frames % 600 == 0) {
    log_line("cuadro %llu: %llu dibujos GX, %llu borrados, %llu copias (%llu a pantalla), %llu paletas; omitidos: "
             "%llu sin pipeline, %llu uniformes, %llu otros dibujos, %llu copias de textura",
             static_cast<unsigned long long>(gStats.frames), static_cast<unsigned long long>(gStats.gxDraws),
             static_cast<unsigned long long>(gStats.clears), static_cast<unsigned long long>(gStats.copies),
             static_cast<unsigned long long>(gStats.displayCopies), static_cast<unsigned long long>(gStats.paletteConvs),
             static_cast<unsigned long long>(gStats.skippedUnknownPipeline),
             static_cast<unsigned long long>(gStats.skippedUniformSize),
             static_cast<unsigned long long>(gStats.skippedCustomDraws),
             static_cast<unsigned long long>(gStats.skippedTextureCopies));
  }
}

bool capture_segment(gfx::detail::FramePacket& packet, uint64_t ticket, SnapshotReady ready,
                     const SnapshotArea& area) noexcept {
  if (!enabled()) return false;
  if (ready == nullptr || area.logicalWidth == 0 || area.logicalHeight == 0 || ticket >= (uint64_t{1} << 61)) {
    log_line("lectura del EFB inválida (ticket %llu, %ux%u)", static_cast<unsigned long long>(ticket),
             area.logicalWidth, area.logicalHeight);
    std::abort();  // el juego esperaría para siempre un token sin instantánea
  }
  const Readback rb{ticket, ready, area.logicalWidth, area.logicalHeight};
  {
    std::lock_guard lock(gSnapMutex);
    gPendingReadbacks.push_back(rb);
  }
  ++gStats.readbacksRequested;
  capture_ops(packet, false, &rb, &area);
  host::notify();
  return true;
}

uint64_t serve_readbacks() noexcept {
  uint64_t served = 0;
  for (;;) {
    Readback rb;
    {
      std::lock_guard lock(gSnapMutex);
      if (gPendingReadbacks.empty()) break;
      rb = gPendingReadbacks.front();
    }
    const uint64_t depthId = kSnapIdBase | (rb.ticket << 1), colorId = depthId | 1;
    // Aún no dibujado (el lote sigue en cola): se sirve en la próxima vuelta del hilo de pantalla.
    if (!core::has_copy_texture(depthId) || !core::has_copy_texture(colorId)) break;
    std::vector<uint8_t> depthRgba, colorRgba;
    uint32_t dw = 0, dh = 0, cw = 0, ch = 0;
    Snapshot snap;
    const bool ok = core::debug_read_copy(depthId, depthRgba, dw, dh) && core::debug_read_copy(colorId, colorRgba, cw, ch) &&
                    dw == rb.width && dh == rb.height && cw == rb.width && ch == rb.height;
    core::texture_released(depthId);
    core::texture_released(colorId);
    if (!ok) {
      ++gStats.readbacksFailed;
      log_line("lectura del EFB del ticket %llu: el núcleo no devolvió las copias (%ux%u y %ux%u, pedido %ux%u)",
               static_cast<unsigned long long>(rb.ticket), dw, dh, cw, ch, rb.width, rb.height);
      std::abort();  // sin datos reales no se entrega el token (la lógica del juego dependería de basura)
    }
    snap.width = rb.width;
    snap.height = rb.height;
    snap.depth.resize(size_t{rb.width} * rb.height);
    snap.argb.resize(snap.depth.size());
    // Fila 0 de la copia = fila de arriba del área (como las texturas del juego).
    for (size_t i = 0; i < snap.depth.size(); ++i) {
      const uint8_t* d = &depthRgba[i * 4];
      const uint8_t* c = &colorRgba[i * 4];
      snap.depth[i] = (uint32_t{d[0]} << 16) | (uint32_t{d[1]} << 8) | d[2];
      snap.argb[i] = (uint32_t{c[3]} << 24) | (uint32_t{c[0]} << 16) | (uint32_t{c[1]} << 8) | c[2];
    }
    {
      std::lock_guard lock(gSnapMutex);
      gPendingReadbacks.pop_front();
      gSnapshots[rb.ticket] = std::move(snap);
    }
    ++gStats.readbacksServed;
    ++served;
    rb.ready(rb.ticket);
  }
  return served;
}

bool read_snapshot(uint64_t ticket, uint16_t x, uint16_t y, uint32_t& depth, uint32_t& argb) noexcept {
  std::lock_guard lock(gSnapMutex);
  const auto it = gSnapshots.find(ticket);
  if (it == gSnapshots.end() || x >= it->second.width || y >= it->second.height) return false;
  const size_t i = size_t{y} * it->second.width + x;
  depth = it->second.depth[i];
  argb = it->second.argb[i];
  return true;
}

void retire_snapshots(uint64_t throughTicket) noexcept {
  std::lock_guard lock(gSnapMutex);
  gSnapshots.erase(gSnapshots.begin(), gSnapshots.upper_bound(throughTicket));
}

Stats stats() noexcept { return gStats; }

} // namespace galaxy_choya

extern "C" bool galaxy_choya_draw_frame(int screenWidth, int screenHeight) {
  if (!galaxy_choya::enabled()) return false;
  return ::choya::gx::draw_frame(screenWidth, screenHeight);
}

// Para efb_snapshot.cpp de MacioGalaxy (enlace débil; ver capture_segment).
extern "C" bool galaxy_choya_read_snapshot(uint64_t ticket, uint16_t x, uint16_t y, uint32_t* depth, uint32_t* argb) {
  return depth != nullptr && argb != nullptr && galaxy_choya::read_snapshot(ticket, x, y, *depth, *argb);
}
extern "C" void galaxy_choya_retire_snapshots(uint64_t throughTicket) { galaxy_choya::retire_snapshots(throughTicket); }
