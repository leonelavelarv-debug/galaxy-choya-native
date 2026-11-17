// galaxy-choya-native: API gráfica directa sobre el motor GLES de Choya (fase 2).
//
// La ruta GX (fase 1) es: J3D emite órdenes GX → FIFO → Aurora las decodifica y arma el cuadro → adaptador → motor de
// Choya. Esta API deja que el adaptador J3D entregue el cuadro ya armado al MISMO motor (choya::gx, el mismo
// generador de shaders TEV, la misma caché de programas, el mismo EFB, las mismas copias): no se genera FIFO ni lista
// GX, ni se decodifica nada por cuadro.
//
// Contrato con el núcleo de Choya (choya_gx_core.cpp, verificado en su código):
//   - los vértices se leen en el shader desde un búfer de bytes big endian (vtx_start + vidx * stride + offset);
//   - los índices son u16 de triángulos (GL_TRIANGLES) relativos a vtx_start;
//   - cada dibujo tiene su bloque de uniformes (std140, 16 KiB como máximo) con:
//       palabra 0: vtx_start; palabra 1: matriz de posición por omisión; palabras 2-3: tamaño del viewport (float);
//       y en las posiciones que dice el PipelineDesc: proyección (16), matrices de posición (12 cada una), normales,
//       registros TEV, konstantes, colores de material/ambiente, escala/tamaño de textura.
//
// Qué es persistente:
//   - geometría: se convierte UNA vez al crear la malla (a big endian, con índices de triángulos) y vive en un arena
//     estable; el cuadro solo copia los bloques al búfer que recibe el núcleo (memcpy, sin conversión);
//   - materiales: un PipelineDesc por material (programa GLSL cacheado por el núcleo con su clave);
//   - grupos de texturas: registrados una vez;
//   - por cuadro solo se escriben los uniformes que cambian (matrices, colores animados).
// Pendiente (docs/BLOQUEOS.md): que el núcleo conserve en la GPU la parte estática del búfer de vértices entre cuadros
// en vez de volver a subirla.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace choya::gx {
struct PipelineDesc;
}

namespace galaxy::direct {

using MeshId = uint32_t;
using MaterialId = uint32_t;
using BindingId = uint32_t;
constexpr uint32_t kInvalid = 0;

// ---------------------------------------------------------------- geometría
// Formato de vértice de la ruta directa: componentes ya decodificados (sin cuantización GX ni índices por atributo).
struct VertexLayout {
  bool normal = false;     // nrm f32x3
  bool color0 = false;     // rgba8
  bool color1 = false;     // rgba8
  uint8_t texCoords = 0;   // tex0..texN-1, f32x2 cada una (máx. 8)
  bool matrixIndex = false;  // u8 por vértice: índice de matriz de posición *3 (como GX_VA_PNMTXIDX) para piel
};
struct MeshDesc {
  VertexLayout layout;
  uint32_t vertexCount = 0;
  const float* positions = nullptr;  // 3 por vértice
  const float* normals = nullptr;    // 3 por vértice (si layout.normal)
  const uint8_t* colors0 = nullptr;  // 4 por vértice
  const uint8_t* colors1 = nullptr;
  const float* texCoords[8] = {};    // 2 por vértice
  const uint8_t* matrixIndices = nullptr;  // GX_VA_PNMTXIDX (0, 3, 6 ... 27)
  const uint16_t* indices = nullptr;  // triángulos
  uint32_t indexCount = 0;
  std::string label;
};

// ---------------------------------------------------------------- materiales
// Estado TEV y de fragmento con valores de GXEnum (los mismos que usa J3D al preparar el material).
struct TevStage {
  uint8_t colorIn[4], colorOp, colorBias, colorScale, colorClamp, colorOut;
  uint8_t alphaIn[4], alphaOp, alphaBias, alphaScale, alphaClamp, alphaOut;
  uint8_t kcSel, kaSel;
  uint8_t texCoord = 0xFF, texMap = 0xFF;  // 0xFF: la etapa no muestrea
  uint8_t channel;                         // GXChannelID
  uint8_t swapRas = 0, swapTex = 0;
};
struct TexGen {
  uint8_t type, src;
  uint8_t matrix = 0xFF;  // 0..9 matriz de textura (texMtx del dibujo); 0xFF identidad
};
struct ColorChannel {
  uint8_t matSrc, ambSrc, lighting, diffuseFn, attnFn;
};
struct MaterialDesc {
  std::vector<TevStage> stages;  // 1..16
  std::array<std::array<uint8_t, 4>, 4> swapTable{{{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}}};
  std::vector<TexGen> texGens;   // 0..8
  ColorChannel channels[4]{};    // COLOR0, COLOR1, ALPHA0, ALPHA1
  uint8_t numChannels = 1;
  uint8_t alphaComp0 = 7, alphaRef0 = 0, alphaOp = 0, alphaComp1 = 7, alphaRef1 = 0;  // GX_ALWAYS, GX_AOP_AND
  uint8_t blendMode = 0, blendSrc = 1, blendDst = 0, blendOp = 0;  // GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO
  bool depthTest = true, depthWrite = true;
  uint8_t depthFunc = 3;  // GX_LEQUAL
  uint8_t cullMode = 2;   // GX_CULL_BACK
  bool colorUpdate = true, alphaUpdate = false;
  std::string label;
};

// Lo que la ruta directa aún no traduce: el material se rechaza con un motivo explícito (el llamador usa la ruta GX).
struct Unsupported {
  bool any = false;
  std::string reason;
};
Unsupported check_material(const MaterialDesc& m);

// ---------------------------------------------------------------- texturas
struct Sampler {
  uint8_t wrapS = 1, wrapT = 1, minFilter = 1, magFilter = 1, mipFilter = 2;
  float minLod = 0.f, maxLod = 1000.f;
};
struct TextureBinding {
  uint64_t texture[8] = {};  // ids de textura del motor (texture_data / texture_from_pack)
  Sampler sampler[8];
};

// ---------------------------------------------------------------- parámetros por dibujo
struct Mat34 {
  float m[3][4];
};
struct DrawParams {
  float projection[4][4];          // fila mayor, como GX (GXSetProjection ya convertida a 4x4)
  Mat34 posMatrices[10];           // GX_PNMTX0..9 (vista * modelo)
  Mat34 nrmMatrices[10];           // inversa transpuesta de cada una (como el XF)
  uint8_t posMatrixCount = 1;      // cuántas se usan (piel: hasta 10)
  uint8_t defaultMatrix = 0;       // sin índice por vértice
  float matColor[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
  float ambColor[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
  float tevColor[4][4] = {};       // GX_TEVPREV, REG0..2
  float kColor[4][4] = {};
  Mat34 texMatrices[10];
  uint32_t dstAlpha = 0xFFFFFFFFu;  // sin alfa de destino
};

struct Stats {
  uint64_t frames = 0, draws = 0, rejectedDraws = 0, meshes = 0, materials = 0, uniformBytes = 0, vertexBytes = 0;
  uint64_t arenaRebuilds = 0;
};

// ---------------------------------------------------------------- dispositivo
class Device {
public:
  Device();
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  MeshId create_mesh(const MeshDesc& desc, std::string* error = nullptr);
  void release_mesh(MeshId id);
  MaterialId register_material(const MaterialDesc& desc, std::string* error = nullptr);
  void release_material(MaterialId id);
  BindingId register_binding(const TextureBinding& binding);

  // Un cuadro: passes y dibujos en orden; end_frame lo entrega al núcleo de Choya (capture_*).
  void begin_frame();
  void begin_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float rgba[4],
                  float depth, bool displayCopy);
  void set_viewport(float x, float y, float w, float h, float znear, float zfar);
  void set_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
  // Dibuja [firstIndex, firstIndex + indexCount) de la malla (indexCount 0 = toda).
  bool draw(MeshId mesh, MaterialId material, BindingId binding, const DrawParams& params, uint32_t firstIndex = 0,
            uint32_t indexCount = 0);
  // Copia del EFB a una textura del motor (GXCopyTex): al terminar el pase actual, como en GX.
  void copy_to_texture(uint64_t texId, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t texW, uint32_t texH,
                       uint32_t gxFormat, bool opaque);
  void end_frame(bool present = true);

  const Stats& stats() const { return stats_; }
  // Para pruebas: el PipelineDesc que registró un material.
  const choya::gx::PipelineDesc* pipeline_desc(MaterialId id) const;

private:
  struct Impl;
  Impl* impl_;
  Stats stats_;
};

// Referencias de pipeline/grupo de la ruta directa en el espacio de ids del núcleo (no chocan con las de Aurora,
// que son punteros/hashes: estas llevan el bit 63).
constexpr uint64_t kDirectRefBit = 1ull << 63;

} // namespace galaxy::direct
