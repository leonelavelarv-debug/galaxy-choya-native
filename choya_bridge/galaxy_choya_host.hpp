// galaxy-choya-native: host de pantalla de la ruta GX → Choya → pantalla.
//
// Un solo hilo ("choya-pantalla") es dueño del contexto GLES: lo crea, dibuja con el núcleo de Choya los cuadros que
// el puente capturó (en orden; la cola acotada es la del núcleo: capture_end espera si va 6 grupos atrás), hace las
// lecturas del EFB pedidas a la hora de un token y presenta. Ningún otro hilo toca GL.
//
// Presentación (GALAXY_PRESENT):
//   sdl      contexto GLES de SDL sobre la ventana de Aurora (X11/Wayland en la PC; KMSDRM con EGL/GBM en la consola
//            si el firmware tiene DRM para la Mali). La ventana se crea con SDL_WINDOW_OPENGL.
//   fbdev    EGL directo sobre /dev/fb0 con la ventana nativa del driver Mali de fbdev (lo que Choya ya usó en la
//            RG35XX H con KNULLI). SDL queda solo para mando y audio.
//   pbuffer  superficie fuera de pantalla (pruebas en la PC, sin pantalla). No muestra nada.
//   auto     (por omisión) fbdev si hay /dev/fb0 y no hay /dev/dri/card0; si no, sdl.
// Lo elegido y por qué se escribe en el registro. Si el modo pedido no se puede abrir, el host lo dice y se apaga:
// no simula cuadros.
#pragma once

#include <cstdint>

struct SDL_Window;

namespace galaxy_choya::host {

enum class Mode : uint8_t { Sdl, Fbdev, Pbuffer };

// Modo pedido por el entorno (sin abrir nada). Lo usa Aurora para crear la ventana con o sin SDL_WINDOW_OPENGL.
Mode requested_mode() noexcept;
const char* mode_name(Mode m) noexcept;

// Arranca el hilo de pantalla (una vez). window: la ventana SDL de Aurora (solo se usa en el modo sdl).
// false si Choya no está activo o el hilo ya no pudo crear el contexto (se espera a que lo intente).
bool start(SDL_Window* window) noexcept;
// Para el hilo: termina lo que esté dibujando, suelta el contexto y la superficie. Idempotente.
void stop() noexcept;
bool running() noexcept;

// El puente avisa: un cuadro capturado (present) o un lote entregado sin mostrar (lecturas del EFB, cargas).
void notify() noexcept;

struct Stats {
  uint64_t logicFrames = 0;     // cuadros que el juego terminó (aurora_end_frame → puente)
  uint64_t drawPasses = 0;      // veces que el hilo llamó al núcleo con trabajo
  uint64_t framesDrawn = 0;     // cuadros que el núcleo dibujó para mostrarse
  uint64_t imagesPresented = 0; // intercambios de búfer hechos (eglSwapBuffers / SDL_GL_SwapWindow)
  uint64_t readbacks = 0;       // lecturas del EFB servidas
  uint64_t presentFailures = 0;
  int width = 0, height = 0;
  // Muestras de la imagen presentada (GALAXY_PRESENT_PROBE=x,y[;x,y...], hasta 4, y desde arriba): RGBA del último
  // cuadro dibujado, leídas en el hilo de pantalla justo antes de presentarlo. Para pruebas de integración.
  static constexpr int kMaxProbes = 4;
  uint8_t probe[4] = {0, 0, 0, 0};  // la primera
  uint8_t probes[kMaxProbes][4] = {};
  int probeCount = 0;
  bool probed = false;
  Mode mode = Mode::Sdl;
  bool contextReady = false;
};
Stats stats() noexcept;

// Espera (en pruebas) hasta que el hilo haya procesado todo lo avisado hasta ahora. false si se agotó el tiempo.
bool wait_idle(int timeoutMs) noexcept;


} // namespace galaxy_choya::host

extern "C" {
// Para el host de la app (MacioGalaxy) y Aurora, con enlace débil: existen solo cuando el puente está compilado.
bool galaxy_choya_presents();             // Choya presenta (CHOYA_GLES=1): Aurora no necesita superficie WebGPU
uint64_t galaxy_choya_window_flags();     // banderas SDL extra para la ventana de Aurora (SDL_WINDOW_OPENGL en sdl)
bool galaxy_choya_host_start(void* sdlWindow);
void galaxy_choya_host_stop();
}
