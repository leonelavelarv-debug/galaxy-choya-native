// galaxy-choya-native: prueba de integración del host de pantalla (GX → Aurora → puente → hilo de pantalla → imagen).
//
// A diferencia de bridge_gx_test, la prueba NO crea su contexto: lo crea el host, como en la app (galaxy_choya_host_start
// con la ventana de Aurora). Recorre con datos sintéticos lo que la app hace en cada cuadro:
//   - Aurora con el backend nulo y SIN superficie WebGPU (Choya presenta; en pbuffer con SDL "offscreen": sin X11).
//   - Lectura del EFB a la hora de un token (PetariNative::GX::captureEfb, lo que dispara un token del juego): se dibuja
//     un cuadrado rojo, se pide la lectura y DESPUÉS se pinta encima de verde. La instantánea debe ver rojo (lo que había
//     al llegar el token) y la imagen presentada, verde.
//   - Cuadros de lógica, dibujados y presentados contados por separado; parada limpia del hilo.
// Salida: 0 aprobada, 1 fallida, 2 omitida (sin EGL/GLES 3 en esta máquina).
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>

#include "galaxy_choya_bridge.hpp"
#include "galaxy_choya_host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace PetariNative::GX {
using SnapshotReady = void (*)(std::uint64_t);
void captureEfb(std::uint64_t ticket, SnapshotReady ready);  // recording.cpp (efb_recording.inc de MacioGalaxy)
}

namespace {
constexpr int W = 640, H = 480;
alignas(32) uint8_t gXfb[W * H * 2];
int gFails = 0;
std::mutex gReadyMutex;
std::condition_variable gReadyCv;
uint64_t gReadyTicket = 0;
std::thread::id gReadyThread;

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++gFails;
}
void quiet(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  if (level >= LOG_WARNING) std::fprintf(stderr, "[aurora %s] %s\n", module, message);
}
void on_ready(std::uint64_t ticket) {
  std::lock_guard lock(gReadyMutex);
  gReadyTicket = ticket;
  gReadyThread = std::this_thread::get_id();
  gReadyCv.notify_all();
}

void quad(float x0, float y0, float x1, float y1, float z, GXColor c) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  GXPosition3f32(x0, y0, z); GXColor4u8(c.r, c.g, c.b, c.a);
  GXPosition3f32(x1, y0, z); GXColor4u8(c.r, c.g, c.b, c.a);
  GXPosition3f32(x1, y1, z); GXColor4u8(c.r, c.g, c.b, c.a);
  GXPosition3f32(x0, y1, z); GXColor4u8(c.r, c.g, c.b, c.a);
  GXEnd();
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
  GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);  // escribe siempre: el verde tapa al rojo y ambos dejan profundidad
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetCullMode(GX_CULL_NONE);
  GXSetCopyClear(GXColor{0, 0, 255, 255}, 0x00FFFFFF);
}
} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "pbuffer";
  setenv("CHOYA_GLES", "1", 1);
  setenv("GALAXY_PRESENT", mode.c_str(), 1);
  setenv("GALAXY_PRESENT_SIZE", "640x480", 1);
  setenv("GALAXY_PRESENT_PROBE", "100,100", 1);  // dentro del cuadrado (y desde arriba)
  // pbuffer: sin servidor gráfico (lo que importa en la consola); sdl: la ventana de Aurora con GLES (xvfb en la PC).
  setenv("SDL_VIDEO_DRIVER", mode == "sdl" ? "x11" : "offscreen", 0);

  AuroraConfig config{};
  config.appName = "galaxy-choya-native host test";
  config.userPath = "/tmp/galaxy-host-test";
  config.cachePath = "/tmp/galaxy-host-test";
  config.desiredBackend = BACKEND_NULL;
  config.windowWidth = W;
  config.windowHeight = H;
  config.logCallback = quiet;
  config.logLevel = LOG_WARNING;
  const AuroraInfo info = aurora_initialize(1, argv, &config);
  if (info.window == nullptr) {
    std::puts("OMITIDA: Aurora sin ventana (backend nulo no disponible)");
    return 2;
  }
  check(aurora_get_backend() == BACKEND_NULL, "Aurora en el backend nulo (graba; no presenta)");
  check(galaxy_choya_presents(), "Choya presenta (CHOYA_GLES=1)");
  if (!galaxy_choya_host_start(info.window)) {
    std::puts("OMITIDA: el host no pudo crear el contexto GLES 3 en esta máquina");
    aurora_shutdown();
    return 2;
  }
  const auto started = galaxy_choya::host::stats();
  check(started.contextReady && started.mode == galaxy_choya::host::requested_mode(),
        "contexto GLES creado por el hilo de pantalla en el modo pedido");

  VIInit();
  VIConfigure(&GXNtsc480IntDf);
  static void* fifo = std::aligned_alloc(32, 0x40000);
  if (fifo == nullptr || !GXInit(fifo, 0x40000)) {
    std::puts("GXInit falló");
    return 1;
  }

  constexpr int kFrames = 4;
  uint32_t snapColor[kFrames] = {}, snapDepth[kFrames] = {}, bgColor[kFrames] = {}, bgDepth[kFrames] = {};
  bool served[kFrames] = {};
  bool readyOnRenderThread = true;
  for (int frame = 0; frame < kFrames; ++frame) {
    for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
    }
    if (!aurora_begin_frame()) {
      check(false, "aurora_begin_frame sin superficie WebGPU");
      continue;
    }
    setup_frame();
    quad(50, 50, 250, 250, -0.25f, GXColor{255, 0, 0, 255});
    // Token: lo que el juego leería con GXPeekARGB/GXPeekZ a esta altura del cuadro.
    const uint64_t ticket = 1000 + static_cast<uint64_t>(frame);
    // En el juego la pide el procesador del FIFO al llegar al token; aquí, con lo anterior ya procesado (GXDrawDone).
    GXDrawDone();
    if (!std::getenv("SIN_TOKEN")) PetariNative::GX::captureEfb(ticket, on_ready);
    quad(50, 50, 250, 250, -0.75f, GXColor{0, 255, 0, 255});  // encima, después del token
    GXSetDispCopySrc(0, 0, W, H);
    GXSetDispCopyDst(W, H);
    GXCopyDisp(gXfb, GX_TRUE);
    GXDrawDone();
    aurora_end_frame();
    {
      std::unique_lock lock(gReadyMutex);
      served[frame] = gReadyCv.wait_for(lock, std::chrono::seconds(10), [&] { return gReadyTicket == ticket; });
      if (served[frame] && gReadyThread == std::this_thread::get_id()) readyOnRenderThread = false;
    }
    if (served[frame]) {
      galaxy_choya::read_snapshot(ticket, 150, 150, snapDepth[frame], snapColor[frame]);
      galaxy_choya::read_snapshot(ticket, 400, 400, bgDepth[frame], bgColor[frame]);
      galaxy_choya::retire_snapshots(ticket);
    }
    std::printf("      cuadro %d: instantánea centro %08x z=%06x, fondo %08x z=%06x\n", frame, snapColor[frame],
                snapDepth[frame], bgColor[frame], bgDepth[frame]);
  }
  galaxy_choya::host::wait_idle(5000);
  const auto st = galaxy_choya::host::stats();
  const auto br = galaxy_choya::stats();
  std::printf("medida: modo=%s lógica=%llu dibujados=%llu presentados=%llu lecturas=%llu segmentos=%llu pasadas=%llu "
              "muestra=%02x%02x%02x\n",
              galaxy_choya::host::mode_name(st.mode), static_cast<unsigned long long>(st.logicFrames),
              static_cast<unsigned long long>(st.framesDrawn), static_cast<unsigned long long>(st.imagesPresented),
              static_cast<unsigned long long>(st.readbacks), static_cast<unsigned long long>(br.segments),
              static_cast<unsigned long long>(st.drawPasses), st.probe[0], st.probe[1], st.probe[2]);

  check(served[2] && served[3], "las lecturas del EFB se sirvieron (ready llamado)");
  check(readyOnRenderThread, "ready() llega desde el hilo de pantalla, no desde el que pidió");
  check((snapColor[3] & 0xFFFFFF) == 0xFF0000, "la instantánea ve el cuadrado rojo (estado a la hora del token)");
  check((bgColor[3] & 0xFFFFFF) == 0x0000FF && bgDepth[3] == 0xFFFFFF, "fondo: color y profundidad del borrado");
  check(snapDepth[3] != 0 && snapDepth[3] < 0xFFFFFF, "profundidad del cuadrado escrita (ni vacía ni la del borrado)");
  check(st.probed && st.probe[0] == 0 && st.probe[1] == 255 && st.probe[2] == 0,
        "la imagen presentada es la del final del cuadro (verde encima)");
  check(st.logicFrames == kFrames, "cuadros de lógica contados (uno por aurora_end_frame)");
  check(st.framesDrawn >= 1 && st.imagesPresented >= 1 && st.imagesPresented <= st.logicFrames,
        "cuadros dibujados e imágenes presentadas contados aparte");
  check(st.readbacks == kFrames && br.readbacksFailed == 0, "una lectura del EFB por token, ninguna fallida");
  check(st.presentFailures == 0, "sin fallos al presentar");

  galaxy_choya_host_stop();
  check(!galaxy_choya::host::running() && !galaxy_choya::host::stats().contextReady,
        "parada limpia: hilo terminado y contexto liberado");
  galaxy_choya_host_stop();  // idempotente
  aurora_shutdown();
  if (gFails == 0) std::puts("PRUEBA HOST OK");
  return gFails == 0 ? 0 : 1;
}
