// galaxy-choya-native: ruta directa hacia el motor GLES de Choya. Ver include/galaxy/choya_direct.hpp.
#include "galaxy/choya_direct.hpp"

#include "choya_gx_core.hpp"

#include <dolphin/gx/GXEnum.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace galaxy::direct {
namespace core = ::choya::gx;
namespace {

// ---------------------------------------------------------------- disposición de uniformes (palabras de 32 bits)
// Las primeras 36 palabras son las que el núcleo lee por posición fija (vtx_start, matriz por omisión, tamaño del
// viewport y comienzos de arreglos indexados, que la ruta directa no usa). Después, en este orden, lo que declara el
// PipelineDesc; channel_layout() del núcleo exige que los colores de material sigan a las luces en este mismo orden.
constexpr uint32_t kWordVtxStart = 0, kWordDefaultMtx = 1, kWordViewport = 2;
constexpr uint32_t kOffProj = 36;
constexpr uint32_t kOffPostex = kOffProj + 16;
constexpr uint32_t kPosMatrices = 10;
constexpr uint32_t kLightWords = 160 + 4;  // 8 luces × 20 palabras + máscaras (núcleo: gx_light)

struct Layout {
  uint32_t postexCount = kPosMatrices, nrmCount = 0, texMatrices = 0;
  uint32_t tevReg[4] = {}, kColor[4] = {}, chanMat[2] = {}, chanMatA[2] = {}, amb[2] = {}, ambA[2] = {};
  uint32_t lights = 0;
  uint32_t words = 0;
};

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// Bytes de un vértice de la ruta directa y desplazamiento de cada atributo.
struct VertexFormat {
  uint32_t stride = 0;
  uint32_t pnmtx = UINT32_MAX, pos = 0, nrm = UINT32_MAX, clr[2] = {UINT32_MAX, UINT32_MAX};
  uint32_t tex[8] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
};
VertexFormat vertex_format(const VertexLayout& l) {
  VertexFormat f;
  uint32_t o = 0;
  if (l.matrixIndex) {
    f.pnmtx = o;
    o += 1;
  }
  o = align_up(o, 4);
  f.pos = o;
  o += 12;
  if (l.normal) {
    f.nrm = o;
    o += 12;
  }
  if (l.color0) {
    f.clr[0] = o;
    o += 4;
  }
  if (l.color1) {
    f.clr[1] = o;
    o += 4;
  }
  for (uint32_t i = 0; i < l.texCoords && i < 8; ++i) {
    f.tex[i] = o;
    o += 8;
  }
  f.stride = align_up(o, 4);
  return f;
}

void put_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
void put_bef(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  put_be32(p, v);
}

struct Mesh {
  VertexLayout layout;
  VertexFormat fmt;
  std::vector<uint8_t> vertices;  // big endian, ya en el formato que lee el shader
  std::vector<uint16_t> indices;
  uint32_t vertexCount = 0;
  // Posición en el arena estable (se rehace solo al crear o soltar mallas).
  uint32_t vtxOffset = 0, idxOffset = 0;
  std::string label;
};

struct Material {
  MaterialDesc desc;
  core::PipelineDesc pipe{};
  Layout layout;
  uint64_t ref = 0;  // en el espacio del núcleo (kDirectRefBit | id)
};

enum class OpKind : uint8_t { Pass, Viewport, Scissor, Draw, Copy };
struct Op {
  OpKind kind;
  // Pass
  uint32_t width = 0, height = 0;
  bool clearColor = false, clearDepth = false, displayCopy = false;
  float rgba[4] = {};
  float depth = 1.f;
  // Viewport/Scissor/Copy
  core::Viewport vp{};
  core::Rect rect{};
  // Draw
  uint64_t pipeline = 0, bindGroup = 0;
  uint32_t idxOffset = 0, indexCount = 0, uniformOffset = 0, dstAlpha = UINT32_MAX;
  // Copy
  uint64_t tex = 0;
  uint32_t texW = 0, texH = 0, fmt = 0;
  bool opaque = false;
};

} // namespace

struct Device::Impl {
  std::map<MeshId, Mesh> meshes;  // ordenado: el arena se arma siempre en el mismo orden
  std::unordered_map<MaterialId, Material> materials;
  std::unordered_map<BindingId, uint64_t> bindings;
  MeshId nextMesh = 1;
  MaterialId nextMaterial = 1;
  BindingId nextBinding = 1;
  // Arena estable: todas las mallas una tras otra. Solo cambia al crear o soltar mallas.
  std::vector<uint8_t> arenaVerts;
  std::vector<uint8_t> arenaIdx;  // u16 en bytes
  bool arenaDirty = true;
  // Cuadro en curso.
  bool inFrame = false;
  std::vector<uint8_t> uniforms;
  std::vector<Op> ops;
  float viewportW = 640.f, viewportH = 480.f;

  void rebuild_arena(Stats& st) {
    arenaVerts.clear();
    arenaIdx.clear();
    for (auto& [id, m] : meshes) {
      m.vtxOffset = static_cast<uint32_t>(arenaVerts.size());
      arenaVerts.insert(arenaVerts.end(), m.vertices.begin(), m.vertices.end());
      // vtx_start es un desplazamiento en bytes y alinea a 4: las mallas empiezan en palabra.
      arenaVerts.resize(align_up(static_cast<uint32_t>(arenaVerts.size()), 4), 0);
      m.idxOffset = static_cast<uint32_t>(arenaIdx.size());
      const auto* p = reinterpret_cast<const uint8_t*>(m.indices.data());
      arenaIdx.insert(arenaIdx.end(), p, p + m.indices.size() * 2);
      arenaIdx.resize(align_up(static_cast<uint32_t>(arenaIdx.size()), 4), 0);
    }
    arenaDirty = false;
    ++st.arenaRebuilds;
  }
};

Device::Device() : impl_(new Impl) {}
Device::~Device() { delete impl_; }

// ---------------------------------------------------------------- mallas
MeshId Device::create_mesh(const MeshDesc& d, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return kInvalid;
  };
  if (d.vertexCount == 0 || d.positions == nullptr) return fail("malla sin posiciones");
  if (d.vertexCount > 65536) return fail("más de 65536 vértices: el núcleo dibuja con índices u16");
  if (d.indices == nullptr || d.indexCount == 0 || d.indexCount % 3 != 0) return fail("índices de triángulos inválidos");
  if (d.layout.normal && !d.normals) return fail("falta el arreglo de normales");
  if (d.layout.color0 && !d.colors0) return fail("falta el color 0");
  if (d.layout.color1 && !d.colors1) return fail("falta el color 1");
  if (d.layout.matrixIndex && !d.matrixIndices) return fail("faltan los índices de matriz");
  if (d.layout.texCoords > 8) return fail("más de 8 coordenadas de textura");
  for (uint32_t t = 0; t < d.layout.texCoords; ++t) {
    if (!d.texCoords[t]) return fail("falta un arreglo de coordenadas de textura");
  }
  for (uint32_t i = 0; i < d.indexCount; ++i) {
    if (d.indices[i] >= d.vertexCount) return fail("índice fuera de la malla");
  }
  if (d.layout.matrixIndex) {
    for (uint32_t v = 0; v < d.vertexCount; ++v) {
      if (d.matrixIndices[v] % 3 != 0 || d.matrixIndices[v] / 3 >= kPosMatrices) {
        return fail("índice de matriz fuera de GX_PNMTX0..9");
      }
    }
  }
  Mesh m;
  m.layout = d.layout;
  m.fmt = vertex_format(d.layout);
  m.vertexCount = d.vertexCount;
  m.label = d.label;
  m.vertices.assign(static_cast<size_t>(m.fmt.stride) * d.vertexCount, 0);
  for (uint32_t v = 0; v < d.vertexCount; ++v) {
    uint8_t* out = m.vertices.data() + static_cast<size_t>(v) * m.fmt.stride;
    if (m.fmt.pnmtx != UINT32_MAX) out[m.fmt.pnmtx] = d.matrixIndices[v];
    for (int c = 0; c < 3; ++c) put_bef(out + m.fmt.pos + 4 * c, d.positions[v * 3 + c]);
    if (m.fmt.nrm != UINT32_MAX) {
      for (int c = 0; c < 3; ++c) put_bef(out + m.fmt.nrm + 4 * c, d.normals[v * 3 + c]);
    }
    if (m.fmt.clr[0] != UINT32_MAX) std::memcpy(out + m.fmt.clr[0], d.colors0 + v * 4, 4);
    if (m.fmt.clr[1] != UINT32_MAX) std::memcpy(out + m.fmt.clr[1], d.colors1 + v * 4, 4);
    for (uint32_t t = 0; t < d.layout.texCoords; ++t) {
      put_bef(out + m.fmt.tex[t], d.texCoords[t][v * 2]);
      put_bef(out + m.fmt.tex[t] + 4, d.texCoords[t][v * 2 + 1]);
    }
  }
  m.indices.assign(d.indices, d.indices + d.indexCount);
  const MeshId id = impl_->nextMesh++;
  impl_->meshes.emplace(id, std::move(m));
  impl_->arenaDirty = true;
  ++stats_.meshes;
  return id;
}

void Device::release_mesh(MeshId id) {
  if (impl_->meshes.erase(id) != 0) {
    impl_->arenaDirty = true;
    --stats_.meshes;
  }
}

// ---------------------------------------------------------------- materiales
Unsupported check_material(const MaterialDesc& m) {
  Unsupported u;
  auto no = [&](std::string why) {
    if (!u.any) {
      u.any = true;
      u.reason = std::move(why);
    }
  };
  if (m.stages.empty() || m.stages.size() > 16) no("número de etapas TEV fuera de 1..16");
  if (m.texGens.size() > 8) no("más de 8 generadores de coordenadas");
  for (const auto& tg : m.texGens) {
    if (tg.type != GX_TG_MTX2x4 && tg.type != GX_TG_MTX3x4) no("texgen no matricial (bump/SRTG): ruta GX");
    if (tg.matrix != 0xFF && tg.matrix >= 10) no("matriz de textura fuera de 0..9");
  }
  for (const auto& s : m.stages) {
    if (s.texMap != 0xFF && (s.texMap >= 8 || s.texCoord >= m.texGens.size())) {
      no("etapa que muestrea una textura o coordenada inexistente");
    }
  }
  for (int ch = 0; ch < 4; ++ch) {
    if (m.channels[ch].lighting) no("iluminación por vértice: pendiente en la ruta directa (ruta GX)");
  }
  return u;
}

namespace {
Layout make_layout(const MaterialDesc& m, core::PipelineDesc& d) {
  Layout L;
  for (const auto& tg : m.texGens) {
    if (tg.matrix != 0xFF) L.texMatrices = std::max<uint32_t>(L.texMatrices, tg.matrix + 1u);
  }
  L.postexCount = kPosMatrices + L.texMatrices;
  L.nrmCount = 0;
  uint32_t w = kOffPostex + 12u * L.postexCount;
  // Normales: solo si algo las usa (texgen de normal). Sin iluminación no hacen falta matrices de normales.
  bool needNrm = false;
  for (const auto& tg : m.texGens) needNrm = needNrm || tg.src == GX_TG_NRM;
  if (needNrm) {
    L.nrmCount = kPosMatrices;
    w += 12u * L.nrmCount;
  }
  // Registros TEV que se leen (GX_CC_C0.. / GX_CA_A0..) y el de salida si solo se escribe alfa: se cargan los 4.
  for (int r = 0; r < 4; ++r) {
    L.tevReg[r] = w;
    w += 4;
  }
  L.lights = w;  // sin iluminación (check_material la rechaza): channel_layout no reserva luces
  const bool ch0 = true;
  (void)ch0;
  for (uint32_t ch = 0; ch < 2; ++ch) {
    if (ch >= m.numChannels) continue;
    if (m.channels[ch].matSrc == GX_SRC_REG) {
      L.chanMat[ch] = w;
      w += 4;
    }
    if (m.channels[ch + 2].matSrc == GX_SRC_REG) {
      L.chanMatA[ch] = w;
      w += 4;
    }
  }
  for (int k = 0; k < 4; ++k) {
    L.kColor[k] = w;
    w += 4;
  }
  L.words = w;
  d.offProj = static_cast<uint16_t>(kOffProj);
  d.offPostex = static_cast<uint16_t>(kOffPostex);
  for (int r = 0; r < 4; ++r) d.offTevReg[r] = static_cast<uint16_t>(L.tevReg[r]);
  for (int k = 0; k < 4; ++k) d.offKColor[k] = static_cast<uint16_t>(L.kColor[k]);
  for (int ch = 0; ch < 2; ++ch) {
    d.offChanMat[ch] = static_cast<uint16_t>(L.chanMat[ch]);
    d.offChanMatA[ch] = static_cast<uint16_t>(L.chanMatA[ch]);
  }
  d.postexCount = static_cast<uint8_t>(L.postexCount);
  d.nrmCount = static_cast<uint8_t>(L.nrmCount);
  return L;
}
} // namespace

MaterialId Device::register_material(const MaterialDesc& m, std::string* error) {
  const Unsupported u = check_material(m);
  if (u.any) {
    if (error) *error = u.reason;
    return kInvalid;
  }
  Material mat;
  mat.desc = m;
  core::PipelineDesc& d = mat.pipe;
  // Atributos: la disposición final depende de la malla; el material fija el formato "completo" y cada dibujo usa
  // un pipeline por (material, formato de malla). Ver draw().
  mat.layout = make_layout(m, d);
  d.tevStageCount = static_cast<uint8_t>(m.stages.size());
  uint8_t sampledTc = 0;
  for (size_t i = 0; i < m.stages.size(); ++i) {
    const TevStage& s = m.stages[i];
    auto& o = d.stages[i];
    o.ca = s.colorIn[0]; o.cb = s.colorIn[1]; o.cc = s.colorIn[2]; o.cd = s.colorIn[3];
    o.cop = s.colorOp; o.cbias = s.colorBias; o.cscale = s.colorScale; o.cclamp = s.colorClamp; o.cout = s.colorOut;
    o.aa = s.alphaIn[0]; o.ab = s.alphaIn[1]; o.ac = s.alphaIn[2]; o.ad = s.alphaIn[3];
    o.aop = s.alphaOp; o.abias = s.alphaBias; o.ascale = s.alphaScale; o.aclamp = s.alphaClamp; o.aout = s.alphaOut;
    o.kcSel = s.kcSel; o.kaSel = s.kaSel;
    o.channel = s.channel;
    o.swapRas = s.swapRas; o.swapTex = s.swapTex;
    o.texCoord = s.texMap != 0xFF ? s.texCoord : 0xFF;
    o.texMap = s.texMap;
    o.pad = o.texCoord;  // ext.valid: coordenada efectiva de la etapa
    if (s.texMap != 0xFF) sampledTc |= static_cast<uint8_t>(1u << s.texCoord);
    d.ext.tind[i] = {0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(s.texMap != 0xFF ? 4 : 0)};
  }
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 4; ++c) d.swap[i][c] = m.swapTable[i][c];
  }
  d.numTexGens = static_cast<uint8_t>(m.texGens.size());
  for (size_t i = 0; i < m.texGens.size(); ++i) {
    d.texgens[i].type = m.texGens[i].type;
    d.texgens[i].src = m.texGens[i].src;
    d.texgens[i].mtxSlot = m.texGens[i].matrix == 0xFF ? 0xFF : static_cast<uint8_t>(kPosMatrices + m.texGens[i].matrix);
  }
  d.sampledTexCoords = sampledTc;
  for (int ch = 0; ch < 4; ++ch) {
    const auto& c = m.channels[ch];
    d.chans[ch] = {c.matSrc, c.ambSrc, static_cast<uint8_t>(c.lighting ? 1 : 0),
                   static_cast<uint8_t>(0x80u | (c.diffuseFn & 3u) | ((c.attnFn & 3u) << 2))};
  }
  // Canales que muestrea el TEV (GX_COLOR0A0 / GX_COLOR1A1 y sus variantes).
  uint8_t sampledCh = 0;
  for (const auto& s : m.stages) {
    if (s.channel == GX_COLOR0 || s.channel == GX_ALPHA0 || s.channel == GX_COLOR0A0) sampledCh |= 1;
    if (s.channel == GX_COLOR1 || s.channel == GX_ALPHA1 || s.channel == GX_COLOR1A1) sampledCh |= 2;
  }
  d.sampledChannels = sampledCh;
  d.tevRegLoads = 4;
  d.lightingEnabled = 0;
  d.alphaComp0 = m.alphaComp0; d.alphaRef0 = m.alphaRef0; d.alphaOp = m.alphaOp;
  d.alphaComp1 = m.alphaComp1; d.alphaRef1 = m.alphaRef1;
  d.depthCompare = m.depthTest ? 1 : 0;
  d.depthUpdate = m.depthWrite ? 1 : 0;
  d.depthFunc = m.depthFunc;
  d.colorUpdate = m.colorUpdate ? 1 : 0;
  d.alphaUpdate = m.alphaUpdate ? 1 : 0;
  d.cullMode = m.cullMode;
  d.blendMode = m.blendMode; d.blendSrc = m.blendSrc; d.blendDst = m.blendDst; d.blendOp = m.blendOp;
  d.pixelFmt = GX_PF_RGB8_Z24;
  auto& e = d.ext;
  e.valid = 1;
  e.fogType = GX_FOG_NONE;
  e.zTexStage = 0xFF;
  for (int i = 0; i < 8; ++i) e.tgPost[i] = 0xFF;
  for (int i = 0; i < 4; ++i) e.ind[i] = {0xFF, 0xFF, 0, 0};
  const MaterialId id = impl_->nextMaterial++;
  mat.ref = 0;
  impl_->materials.emplace(id, std::move(mat));
  ++stats_.materials;
  return id;
}

void Device::release_material(MaterialId id) {
  if (impl_->materials.erase(id) != 0) --stats_.materials;
}

const core::PipelineDesc* Device::pipeline_desc(MaterialId id) const {
  const auto it = impl_->materials.find(id);
  return it == impl_->materials.end() ? nullptr : &it->second.pipe;
}

BindingId Device::register_binding(const TextureBinding& b) {
  core::BindDesc bd{};
  for (int i = 0; i < 8; ++i) {
    bd.tex[i] = b.texture[i];
    bd.samp[i] = {b.sampler[i].wrapS, b.sampler[i].wrapT, b.sampler[i].minFilter, b.sampler[i].magFilter,
                  b.sampler[i].mipFilter, {0, 0, 0}, b.sampler[i].minLod, b.sampler[i].maxLod};
  }
  const BindingId id = impl_->nextBinding++;
  const uint64_t ref = kDirectRefBit | (uint64_t{2} << 56) | id;
  core::register_bind_group(ref, bd);
  impl_->bindings.emplace(id, ref);
  return id;
}

// ---------------------------------------------------------------- cuadro
void Device::begin_frame() {
  impl_->inFrame = true;
  impl_->uniforms.clear();
  impl_->ops.clear();
  if (impl_->arenaDirty) impl_->rebuild_arena(stats_);
}

void Device::begin_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float rgba[4],
                        float depth, bool displayCopy) {
  Op op{OpKind::Pass};
  op.width = width;
  op.height = height;
  op.clearColor = clearColor;
  op.clearDepth = clearDepth;
  op.displayCopy = displayCopy;
  std::memcpy(op.rgba, rgba, sizeof op.rgba);
  op.depth = depth;
  op.rect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  impl_->ops.push_back(op);
}

void Device::set_viewport(float x, float y, float w, float h, float zn, float zf) {
  Op op{OpKind::Viewport};
  op.vp = {x, y, w, h, zn, zf};
  impl_->viewportW = w;
  impl_->viewportH = h;
  impl_->ops.push_back(op);
}

void Device::set_scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
  Op op{OpKind::Scissor};
  op.rect = {x, y, w, h};
  impl_->ops.push_back(op);
}

namespace {
// Pipeline por (material, formato de malla): mismo TEV, atributos de esa malla.
uint64_t pipeline_for(const Material& mat, const Mesh& mesh, MaterialId matId) {
  uint32_t fmtKey = (mesh.layout.normal ? 1u : 0u) | (mesh.layout.color0 ? 2u : 0u) | (mesh.layout.color1 ? 4u : 0u) |
                    (mesh.layout.matrixIndex ? 8u : 0u) | (static_cast<uint32_t>(mesh.layout.texCoords) << 4);
  const uint64_t ref = kDirectRefBit | (uint64_t{1} << 56) | (uint64_t{fmtKey} << 32) | matId;
  if (core::has_pipeline(ref)) return ref;
  core::PipelineDesc d = mat.pipe;
  for (auto& a : d.attrs) a = {GX_NONE, 0, 0, 0, 0, 0, 0, 0};
  const VertexFormat& f = mesh.fmt;
  auto set = [&](int attr, uint32_t off, uint8_t cnt, uint8_t comp) {
    d.attrs[attr] = {GX_DIRECT, cnt, comp, static_cast<uint8_t>(off), 0, 0, 0, 0};
  };
  if (f.pnmtx != UINT32_MAX) set(GX_VA_PNMTXIDX, f.pnmtx, 1, GX_U8);
  set(GX_VA_POS, f.pos, 3, GX_F32);
  if (f.nrm != UINT32_MAX) set(GX_VA_NRM, f.nrm, 3, GX_F32);
  if (f.clr[0] != UINT32_MAX) set(GX_VA_CLR0, f.clr[0], 4, GX_RGBA8);
  if (f.clr[1] != UINT32_MAX) set(GX_VA_CLR1, f.clr[1], 4, GX_RGBA8);
  for (int t = 0; t < 8; ++t) {
    if (f.tex[t] != UINT32_MAX) set(GX_VA_TEX0 + t, f.tex[t], 2, GX_F32);
  }
  d.vtxStride = static_cast<uint8_t>(f.stride);
  core::register_pipeline(ref, d);
  return ref;
}

void put_words(std::vector<uint8_t>& buf, uint32_t base, uint32_t word, const float* v, uint32_t n) {
  std::memcpy(buf.data() + base + word * 4u, v, n * 4u);
}
} // namespace

bool Device::draw(MeshId meshId, MaterialId matId, BindingId bindingId, const DrawParams& p, uint32_t firstIndex,
                  uint32_t indexCount) {
  const auto mi = impl_->meshes.find(meshId);
  const auto ma = impl_->materials.find(matId);
  if (!impl_->inFrame || mi == impl_->meshes.end() || ma == impl_->materials.end()) {
    ++stats_.rejectedDraws;
    return false;
  }
  const Mesh& mesh = mi->second;
  const Material& mat = ma->second;
  if (indexCount == 0) indexCount = static_cast<uint32_t>(mesh.indices.size()) - firstIndex;
  if (firstIndex + indexCount > mesh.indices.size() || indexCount % 3 != 0) {
    ++stats_.rejectedDraws;
    return false;
  }
  uint64_t bindRef = 0;
  if (bindingId != kInvalid) {
    const auto b = impl_->bindings.find(bindingId);
    if (b == impl_->bindings.end()) {
      ++stats_.rejectedDraws;
      return false;
    }
    bindRef = b->second;
  }
  // Bloque de uniformes de este dibujo: alineado a 256 (el máximo de GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT habitual).
  auto& U = impl_->uniforms;
  const uint32_t base = align_up(static_cast<uint32_t>(U.size()), 256);
  const Layout& L = mat.layout;
  U.resize(base + L.words * 4u, 0);
  const uint32_t vtxStart = mesh.vtxOffset;
  std::memcpy(U.data() + base + kWordVtxStart * 4, &vtxStart, 4);
  const uint32_t defMtx = p.defaultMatrix;
  std::memcpy(U.data() + base + kWordDefaultMtx * 4, &defMtx, 4);
  const float vpSize[2] = {impl_->viewportW, impl_->viewportH};
  put_words(U, base, kWordViewport, vpSize, 2);
  put_words(U, base, kOffProj, &p.projection[0][0], 16);
  for (uint32_t i = 0; i < kPosMatrices; ++i) put_words(U, base, kOffPostex + 12 * i, &p.posMatrices[i].m[0][0], 12);
  for (uint32_t i = 0; i < L.texMatrices; ++i) {
    put_words(U, base, kOffPostex + 12 * (kPosMatrices + i), &p.texMatrices[i].m[0][0], 12);
  }
  for (uint32_t i = 0; i < L.nrmCount; ++i) {
    put_words(U, base, kOffPostex + 12 * (L.postexCount + i), &p.nrmMatrices[i].m[0][0], 12);
  }
  for (int r = 0; r < 4; ++r) put_words(U, base, L.tevReg[r], p.tevColor[r], 4);
  for (int k = 0; k < 4; ++k) put_words(U, base, L.kColor[k], p.kColor[k], 4);
  for (int ch = 0; ch < 2; ++ch) {
    if (L.chanMat[ch]) put_words(U, base, L.chanMat[ch], p.matColor[ch], 4);
    if (L.chanMatA[ch]) put_words(U, base, L.chanMatA[ch], p.matColor[ch], 4);
  }
  Op op{OpKind::Draw};
  op.pipeline = pipeline_for(mat, mesh, matId);
  op.bindGroup = bindRef;
  op.idxOffset = mesh.idxOffset + firstIndex * 2u;
  op.indexCount = indexCount;
  op.uniformOffset = base;
  op.dstAlpha = p.dstAlpha;
  impl_->ops.push_back(op);
  ++stats_.draws;
  return true;
}

void Device::copy_to_texture(uint64_t texId, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t texW,
                             uint32_t texH, uint32_t gxFormat, bool opaque) {
  Op op{OpKind::Copy};
  op.tex = texId;
  op.rect = {x, y, w, h};
  op.texW = texW;
  op.texH = texH;
  op.fmt = gxFormat;
  op.opaque = opaque;
  impl_->ops.push_back(op);
}

void Device::end_frame(bool present) {
  if (!impl_->inFrame) return;
  impl_->inFrame = false;
  auto& I = *impl_;
  // El núcleo copia los búferes: los de la arena no cambian entre cuadros (no se vuelven a convertir).
  core::capture_begin(I.arenaVerts.data(), I.arenaVerts.size(), I.uniforms.data(), I.uniforms.size(),
                      I.arenaIdx.data(), I.arenaIdx.size(), nullptr, 0);
  for (const Op& op : I.ops) {
    switch (op.kind) {
    case OpKind::Pass:
      core::capture_pass(op.width, op.height, op.clearColor, op.clearDepth, op.rgba, op.depth, op.displayCopy, op.rect);
      break;
    case OpKind::Viewport:
      core::capture_viewport(op.vp);
      break;
    case OpKind::Scissor:
      core::capture_scissor(op.rect);
      break;
    case OpKind::Draw:
      core::capture_draw(op.pipeline, op.idxOffset, op.indexCount, 1, op.uniformOffset, op.dstAlpha, op.bindGroup);
      break;
    case OpKind::Copy:
      core::capture_copy(op.tex, op.rect, op.texW, op.texH, op.fmt, op.opaque);
      break;
    }
  }
  core::capture_end(present);
  stats_.uniformBytes += I.uniforms.size();
  stats_.vertexBytes += I.arenaVerts.size();
  ++stats_.frames;
}

} // namespace galaxy::direct
