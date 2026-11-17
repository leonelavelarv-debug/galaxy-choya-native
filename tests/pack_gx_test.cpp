// galaxy-choya-native: prueba de assets.wlpk por la ruta de la app (GX → Aurora → puente → host de pantalla).
//
// Escribe un assets.wlpk sintético con el formato del conversor de Choya (WLPK v1, misma clave: crc32 del nivel 0 GX +
// tamaño + formato; con paleta, también la TLUT y su formato) y dibuja tres cuadrados texturizados:
//   A  RGB565 amarilla en el juego, en el paquete magenta      → debe verse magenta (contenido del paquete)
//   B  C8 + TLUT RGB565 (gris en el juego), en el paquete cian → debe verse cian (identidad con paleta)
//   C  RGB565 blanca, no está en el paquete                    → debe verse blanca (original de Aurora)
// Salida: 0 aprobada, 1 fallida, 2 omitida (sin EGL/GLES 3).
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>

#include "galaxy_choya_bridge.hpp"
#include "galaxy_choya_host.hpp"
#include "choya_gx_core.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int W = 640, H = 480;
alignas(32) uint8_t gXfb[W * H * 2];
alignas(32) uint8_t gTexA[8 * 8 * 2];  // RGB565 amarillo
alignas(32) uint8_t gTexC[8 * 8 * 2];  // RGB565 blanco
alignas(32) uint8_t gTexB[8 * 8];      // C8: índice 7 en todos los texeles
alignas(32) uint8_t gTlut[256 * 2];    // RGB565: entrada 7 gris medio
GXTexObj gObjA, gObjB, gObjC;
GXTlutObj gTlutObj;
int gFails = 0;

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++gFails;
}
void quiet(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  if (level >= LOG_WARNING) std::fprintf(stderr, "[aurora %s] %s\n", module, message);
}

uint32_t crc32(const uint8_t* d, size_t n, uint32_t prev = 0) {  // IEEE, encadenable como zlib (igual que el núcleo)
  uint32_t c = prev ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
uint64_t key_of(uint32_t crc, uint32_t w, uint32_t h, uint32_t fmt) {
  return (uint64_t{crc} << 32) | (uint64_t((w - 1) & 0x3FF) << 22) | (uint64_t((h - 1) & 0x3FF) << 12) |
         (uint64_t(fmt & 0xF) << 8);
}

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint32_t version, entryCount, flags;
  uint64_t tocOffset, namesOffset, namesSize, manifestOffset, manifestSize, reserved;
};
struct Entry {
  uint8_t kind, format, count, flags;
  uint32_t nameOffset;
  uint64_t key, dataOffset, dataSize;
  uint16_t width, height;
  uint32_t extra0, extra1, extra2;
};
#pragma pack(pop)

bool write_pack(const char* path) {
  const uint64_t keyA = key_of(crc32(gTexA, sizeof gTexA), 8, 8, GX_TF_RGB565);
  const uint8_t tlutFmt = GX_TL_RGB565;
  const uint32_t crcB = crc32(&tlutFmt, 1, crc32(gTlut, sizeof gTlut, crc32(gTexB, sizeof gTexB)));
  const uint64_t keyB = key_of(crcB, 8, 8, GX_TF_C8);
  std::vector<uint8_t> magenta(8 * 8 * 4), cyan(8 * 8 * 4);
  for (size_t i = 0; i < 64; ++i) {
    const uint8_t m[4] = {255, 0, 255, 255}, c[4] = {0, 255, 255, 255};
    std::memcpy(&magenta[i * 4], m, 4);
    std::memcpy(&cyan[i * 4], c, 4);
  }
  Entry e[2] = {};
  const uint64_t data0 = sizeof(Header) + sizeof e;
  e[0] = Entry{1, 10 /*RGBA8*/, 1, 0, 0, keyA, data0, magenta.size(), 8, 8, 0, 0, 0};
  e[1] = Entry{1, 10, 1, 0, 0, keyB, data0 + magenta.size(), cyan.size(), 8, 8, 0, 0, 0};
  Header h{{'W', 'L', 'P', 'K'}, 1, 2, 0, sizeof(Header), 0, 0, 0, 0, 0};
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 && std::fwrite(e, sizeof e, 1, f) == 1 &&
            std::fwrite(magenta.data(), 1, magenta.size(), f) == magenta.size() &&
            std::fwrite(cyan.data(), 1, cyan.size(), f) == cyan.size();
  return std::fclose(f) == 0 && ok;
}

void setup_frame() {
  f32 proj[4][4] = {};
  proj[0][0] = 2.f / W;
  proj[0][3] = -1.f;
  proj[1][1] = -2.f / H;
  proj[1][3] = 1.f;
  proj[2][2] = -1.f;
  proj[2][3] = -1.f;
  proj[3][3] = 1.f;
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  f32 ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXLoadPosMtxImm(ident, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetViewport(0, 0, W, H, 0, 1);
  GXSetScissor(0, 0, W, H);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetCullMode(GX_CULL_NONE);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetCopyClear(GXColor{0, 0, 0, 255}, 0x00FFFFFF);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

void textured_quad(GXTexObj* obj, float x0) {
  GXLoadTexObj(obj, GX_TEXMAP0);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  GXPosition3f32(x0, 100, -0.5f); GXTexCoord2f32(0, 0);
  GXPosition3f32(x0 + 100, 100, -0.5f); GXTexCoord2f32(1, 0);
  GXPosition3f32(x0 + 100, 200, -0.5f); GXTexCoord2f32(1, 1);
  GXPosition3f32(x0, 200, -0.5f); GXTexCoord2f32(0, 1);
  GXEnd();
}
} // namespace

int main(int, char** argv) {
  for (size_t i = 0; i < sizeof gTexA; i += 2) {
    gTexA[i] = 0xFF; gTexA[i + 1] = 0xE0;  // amarillo
    gTexC[i] = 0xFF; gTexC[i + 1] = 0xFF;  // blanco
  }
  std::memset(gTexB, 7, sizeof gTexB);
  gTlut[14] = 0x84; gTlut[15] = 0x10;  // entrada 7: gris (RGB565 0x8410)
  const char* packPath = "/tmp/galaxy-pack-test.wlpk";
  if (!write_pack(packPath)) {
    std::puts("no se pudo escribir el paquete");
    return 1;
  }
  setenv("CHOYA_GLES", "1", 1);
  setenv("CHOYA_PAQUETE", packPath, 1);
  setenv("GALAXY_PRESENT", "pbuffer", 1);
  setenv("GALAXY_PRESENT_SIZE", "640x480", 1);
  setenv("GALAXY_PRESENT_PROBE", "150,150;350,150;550,150", 1);
  setenv("SDL_VIDEO_DRIVER", "offscreen", 0);

  AuroraConfig config{};
  config.appName = "galaxy-choya-native pack test";
  config.userPath = "/tmp/galaxy-pack-test";
  config.cachePath = "/tmp/galaxy-pack-test";
  config.desiredBackend = BACKEND_NULL;
  config.windowWidth = W;
  config.windowHeight = H;
  config.logCallback = quiet;
  config.logLevel = LOG_WARNING;
  const AuroraInfo info = aurora_initialize(1, argv, &config);
  if (info.window == nullptr) {
    std::puts("OMITIDA: Aurora sin ventana");
    return 2;
  }
  if (!galaxy_choya_host_start(info.window)) {
    std::puts("OMITIDA: el host no pudo crear el contexto GLES 3");
    aurora_shutdown();
    return 2;
  }
  check(choya::gx::pack_is_open(), "el puente abrió assets.wlpk (CHOYA_PAQUETE)");
  VIInit();
  VIConfigure(&GXNtsc480IntDf);
  static void* fifo = std::aligned_alloc(32, 0x40000);
  if (fifo == nullptr || !GXInit(fifo, 0x40000)) return 1;
  GXInitTexObj(&gObjA, gTexA, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObj(&gObjC, gTexC, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTlutObj(&gTlutObj, gTlut, GX_TL_RGB565, 256);
  GXInitTexObjCI(&gObjB, gTexB, 8, 8, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
  for (GXTexObj* o : {&gObjA, &gObjB, &gObjC}) GXInitTexObjLOD(o, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);

  for (int frame = 0; frame < 3; ++frame) {
    for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
    }
    if (!aurora_begin_frame()) continue;
    setup_frame();
    GXLoadTlut(&gTlutObj, GX_TLUT0);
    textured_quad(&gObjA, 100);
    textured_quad(&gObjB, 300);
    textured_quad(&gObjC, 500);
    GXSetDispCopySrc(0, 0, W, H);
    GXSetDispCopyDst(W, H);
    GXCopyDisp(gXfb, GX_TRUE);
    GXDrawDone();
    aurora_end_frame();
  }
  galaxy_choya::host::wait_idle(5000);
  const auto st = galaxy_choya::host::stats();
  const auto br = galaxy_choya::stats();
  auto rgb = [&](int i) { return (uint32_t{st.probes[i][0]} << 16) | (uint32_t{st.probes[i][1]} << 8) | st.probes[i][2]; };
  std::printf("medida: A=%06x B=%06x C=%06x aciertos=%llu (paleta %llu) fallos=%llu bytes_sustituidos=%llu\n", rgb(0),
              rgb(1), rgb(2), static_cast<unsigned long long>(br.packHits),
              static_cast<unsigned long long>(br.packPaletteHits), static_cast<unsigned long long>(br.packMisses),
              static_cast<unsigned long long>(br.packBytesReplaced));
  check(st.probed && rgb(0) == 0xFF00FF, "A: se ve la textura del paquete (magenta), no la del juego (amarilla)");
  check(st.probed && rgb(1) == 0x00FFFF, "B: textura con paleta sustituida por su identidad GX + TLUT (cian)");
  check(st.probed && rgb(2) == 0xFFFFFF, "C: sin entrada en el paquete se usa la original (blanca)");
  check(br.packHits == 2 && br.packPaletteHits == 1 && br.packMisses >= 1, "contadores: 2 aciertos (1 con paleta), fallos");
  check(br.packBytesReplaced == 2 * 8 * 8 * 4, "bytes decodificados que no se mandaron (2 texturas RGBA8 8x8)");
  galaxy_choya_host_stop();
  aurora_shutdown();
  if (gFails == 0) std::puts("PRUEBA PAQUETE OK");
  return gFails == 0 ? 0 : 1;
}
