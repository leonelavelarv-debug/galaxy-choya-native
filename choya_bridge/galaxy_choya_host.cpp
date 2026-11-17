// galaxy-choya-native: host de pantalla (ver galaxy_choya_host.hpp).
#include "galaxy_choya_host.hpp"

#include "galaxy_choya_bridge.hpp"
#include "choya_gx_core.hpp"

#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace galaxy_choya::host {
namespace {

// --- EGL mínimo, cargado con dlopen (el ejecutable no depende de libEGL para arrancar) -------------------------
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLSurface = void*;
using EGLContext = void*;
using EGLint = int32_t;
constexpr EGLint EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_WINDOW_BIT = 0x0004, EGL_PBUFFER_BIT = 0x0001,
                 EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES3_BIT = 0x0040, EGL_RED_SIZE = 0x3024,
                 EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021, EGL_DEPTH_SIZE = 0x3025,
                 EGL_STENCIL_SIZE = 0x3026, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056,
                 EGL_CONTEXT_MAJOR_VERSION = 0x3098, EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_VENDOR = 0x3053,
                 EGL_VERSION = 0x3054;
constexpr unsigned EGL_OPENGL_ES_API = 0x30A0;

struct Egl {
  void* lib = nullptr;
  void* gles = nullptr;
  void* (*GetProcAddress)(const char*) = nullptr;
  EGLDisplay (*GetDisplay)(void*) = nullptr;
  unsigned (*Initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
  unsigned (*Terminate)(EGLDisplay) = nullptr;
  unsigned (*BindAPI)(unsigned) = nullptr;
  unsigned (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
  EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*) = nullptr;
  EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
  unsigned (*DestroyContext)(EGLDisplay, EGLContext) = nullptr;
  unsigned (*DestroySurface)(EGLDisplay, EGLSurface) = nullptr;
  unsigned (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
  unsigned (*SwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
  unsigned (*SwapInterval)(EGLDisplay, EGLint) = nullptr;
  const char* (*QueryString)(EGLDisplay, EGLint) = nullptr;
  EGLint (*GetError)() = nullptr;
};

struct FbdevWindow {  // ventana nativa del EGL de Mali para fbdev
  unsigned short width;
  unsigned short height;
};

struct State {
  std::mutex mutex;
  std::condition_variable cv;      // trabajo nuevo o parada
  std::condition_variable idleCv;  // el hilo terminó lo avisado
  uint64_t requested = 0, handled = 0;
  bool stopping = false;
  std::thread thread;
  std::atomic<bool> running{false};
  SDL_Window* window = nullptr;
  Mode mode = Mode::Sdl;
  // Contexto (solo el hilo de pantalla).
  Egl egl;
  EGLDisplay display = nullptr;
  EGLSurface surface = nullptr;
  EGLContext context = nullptr;
  FbdevWindow fbWindow{640, 480};
  SDL_GLContext sdlContext = nullptr;
  void (*readPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
  void (*finish)() = nullptr;
  void (*bindFramebuffer)(unsigned, unsigned) = nullptr;
  int probeXY[Stats::kMaxProbes][2] = {};
  int probeCount = 0;
  Stats stats;
  std::mutex statsMutex;
};
State& st() {
  static State* s = new State;  // nunca se destruye: el proceso termina con _Exit
  return *s;
}

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "[choya-pantalla] %s\n", buf);
}

Mode parse_mode() {
  const char* v = std::getenv("GALAXY_PRESENT");
  if (v != nullptr && std::strcmp(v, "sdl") == 0) return Mode::Sdl;
  if (v != nullptr && std::strcmp(v, "fbdev") == 0) return Mode::Fbdev;
  if (v != nullptr && std::strcmp(v, "pbuffer") == 0) return Mode::Pbuffer;
  // auto: el driver Mali de fbdev (sin DRM) es lo que tiene KNULLI en la H700 según Choya; con DRM, SDL (KMSDRM o
  // X11/Wayland en la PC).
  const bool fb = access("/dev/fb0", F_OK) == 0;
  const bool drm = access("/dev/dri/card0", F_OK) == 0 || access("/dev/dri/card1", F_OK) == 0;
  const bool display = std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
  if (fb && !drm && !display) return Mode::Fbdev;
  return Mode::Sdl;
}

void* gl_proc(const char* name) {
  State& s = st();
  if (s.mode == Mode::Sdl) return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
  void* p = s.egl.gles != nullptr ? dlsym(s.egl.gles, name) : nullptr;
  if (p == nullptr && s.egl.GetProcAddress != nullptr) p = s.egl.GetProcAddress(name);
  return p;
}

void core_log(const char* line) { std::fprintf(stderr, "[choya-motor] %s\n", line); }

void* open_first(const char* const* names) {
  for (const char* const* n = names; *n != nullptr; ++n) {
    if (void* h = dlopen(*n, RTLD_NOW | RTLD_GLOBAL)) {
      log_line("biblioteca: %s", *n);
      return h;
    }
  }
  return nullptr;
}

template <typename T>
bool load(void* lib, T& fn, const char* name) {
  fn = reinterpret_cast<T>(lib != nullptr ? dlsym(lib, name) : nullptr);
  if (fn == nullptr) log_line("falta la función %s", name);
  return fn != nullptr;
}

bool open_egl(State& s) {
  static const char* const kEgl[] = {"libEGL.so.1", "libEGL.so", "libmali.so.0", "libmali.so", nullptr};
  static const char* const kGles[] = {"libGLESv2.so.2", "libGLESv2.so", "libmali.so.0", "libmali.so", nullptr};
  s.egl.lib = open_first(kEgl);
  s.egl.gles = open_first(kGles);
  if (s.egl.lib == nullptr || s.egl.gles == nullptr) {
    log_line("no hay libEGL/libGLESv2 (ni libmali)");
    return false;
  }
  Egl& e = s.egl;
  bool ok = load(e.lib, e.GetProcAddress, "eglGetProcAddress");
  ok &= load(e.lib, e.GetDisplay, "eglGetDisplay");
  ok &= load(e.lib, e.Initialize, "eglInitialize");
  ok &= load(e.lib, e.Terminate, "eglTerminate");
  ok &= load(e.lib, e.BindAPI, "eglBindAPI");
  ok &= load(e.lib, e.ChooseConfig, "eglChooseConfig");
  ok &= load(e.lib, e.CreateWindowSurface, "eglCreateWindowSurface");
  ok &= load(e.lib, e.CreatePbufferSurface, "eglCreatePbufferSurface");
  ok &= load(e.lib, e.CreateContext, "eglCreateContext");
  ok &= load(e.lib, e.DestroyContext, "eglDestroyContext");
  ok &= load(e.lib, e.DestroySurface, "eglDestroySurface");
  ok &= load(e.lib, e.MakeCurrent, "eglMakeCurrent");
  ok &= load(e.lib, e.SwapBuffers, "eglSwapBuffers");
  ok &= load(e.lib, e.SwapInterval, "eglSwapInterval");
  ok &= load(e.lib, e.QueryString, "eglQueryString");
  ok &= load(e.lib, e.GetError, "eglGetError");
  return ok;
}

bool create_egl_context(State& s, int w, int h) {
  if (!open_egl(s)) return false;
  Egl& e = s.egl;
  s.display = e.GetDisplay(nullptr);  // EGL_DEFAULT_DISPLAY (EGL_PLATFORM decide en Mesa)
  EGLint major = 0, minor = 0;
  if (s.display == nullptr || !e.Initialize(s.display, &major, &minor)) {
    log_line("eglInitialize falló");
    return false;
  }
  log_line("EGL %s %s", e.QueryString(s.display, EGL_VENDOR), e.QueryString(s.display, EGL_VERSION));
  e.BindAPI(EGL_OPENGL_ES_API);
  const EGLint surfaceBit = s.mode == Mode::Fbdev ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT;
  const EGLint cfgAttrs[] = {EGL_SURFACE_TYPE, surfaceBit, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                             EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
  EGLConfig cfg = nullptr;
  EGLint n = 0;
  if (!e.ChooseConfig(s.display, cfgAttrs, &cfg, 1, &n) || n == 0) {
    log_line("sin configuración EGL GLES 3 RGBA8 + profundidad 24");
    return false;
  }
  if (s.mode == Mode::Fbdev) {
    s.fbWindow = FbdevWindow{static_cast<unsigned short>(w), static_cast<unsigned short>(h)};
    s.surface = e.CreateWindowSurface(s.display, cfg, &s.fbWindow, nullptr);
  } else {
    const EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    s.surface = e.CreatePbufferSurface(s.display, cfg, pb);
  }
  if (s.surface == nullptr) {
    log_line("no se pudo crear la superficie EGL (0x%04X)", static_cast<unsigned>(e.GetError()));
    return false;
  }
  for (EGLint minorVersion : {2, 1, 0}) {
    const EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minorVersion, EGL_NONE};
    s.context = e.CreateContext(s.display, cfg, nullptr, ctx);
    if (s.context != nullptr) break;
  }
  if (s.context == nullptr || !e.MakeCurrent(s.display, s.surface, s.surface, s.context)) {
    log_line("no se pudo crear o activar el contexto GLES 3");
    return false;
  }
  e.SwapInterval(s.display, 0);  // el ritmo lo marca el VI del juego
  return true;
}

bool create_sdl_context(State& s) {
  if (s.window == nullptr) {
    log_line("modo sdl sin ventana de Aurora");
    return false;
  }
  if ((SDL_GetWindowFlags(s.window) & SDL_WINDOW_OPENGL) == 0) {
    log_line("la ventana de Aurora no tiene SDL_WINDOW_OPENGL (falta el gancho de window.cpp)");
    return false;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  for (int minorVersion : {2, 1, 0}) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minorVersion);
    s.sdlContext = SDL_GL_CreateContext(s.window);
    if (s.sdlContext != nullptr) break;
  }
  if (s.sdlContext == nullptr) {
    log_line("SDL_GL_CreateContext: %s", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(s.window, s.sdlContext);
  SDL_GL_SetSwapInterval(0);
  log_line("SDL %s, vídeo %s", SDL_GetRevision(), SDL_GetCurrentVideoDriver());
  return true;
}

void screen_size(State& s, int& w, int& h) {
  if (s.mode == Mode::Sdl && s.window != nullptr) {
    SDL_GetWindowSizeInPixels(s.window, &w, &h);
  } else if (s.mode == Mode::Fbdev) {
    w = s.fbWindow.width;
    h = s.fbWindow.height;
  } else {
    w = s.stats.width;
    h = s.stats.height;
  }
}

void fbdev_size(int& w, int& h) {
  w = 640;
  h = 480;
  if (int fd = open("/dev/fb0", O_RDONLY); fd >= 0) {
    fb_var_screeninfo v{};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres != 0 && v.yres != 0) {
      w = static_cast<int>(v.xres);
      h = static_cast<int>(v.yres);
    }
    close(fd);
  }
}

bool present(State& s) {
  if (s.mode == Mode::Sdl) return SDL_GL_SwapWindow(s.window);
  return s.egl.SwapBuffers(s.display, s.surface) != 0;
}

void destroy_context(State& s) {
  if (s.mode == Mode::Sdl) {
    if (s.sdlContext != nullptr) {
      SDL_GL_MakeCurrent(s.window, nullptr);
      SDL_GL_DestroyContext(s.sdlContext);
      s.sdlContext = nullptr;
    }
    return;
  }
  if (s.display != nullptr) {
    s.egl.MakeCurrent(s.display, nullptr, nullptr, nullptr);
    if (s.context != nullptr) s.egl.DestroyContext(s.display, s.context);
    if (s.surface != nullptr) s.egl.DestroySurface(s.display, s.surface);
    s.egl.Terminate(s.display);
  }
  s.context = s.surface = s.display = nullptr;
}

void render_thread(std::promise<bool>* ready) {
  pthread_setname_np(pthread_self(), "choya-pantalla");
  State& s = st();
  int w = 0, h = 0;
  bool ok = false;
  if (s.mode == Mode::Sdl) {
    ok = create_sdl_context(s);
  } else {
    if (s.mode == Mode::Fbdev) {
      fbdev_size(w, h);
    } else {
      w = s.stats.width > 0 ? s.stats.width : 640;
      h = s.stats.height > 0 ? s.stats.height : 480;
    }
    ok = create_egl_context(s, w, h);
  }
  if (ok) {
    choya::gx::set_gl_loader(gl_proc);
    choya::gx::set_logger(core_log);
    s.readPixels = reinterpret_cast<decltype(s.readPixels)>(gl_proc("glReadPixels"));
    s.finish = reinterpret_cast<decltype(s.finish)>(gl_proc("glFinish"));
    s.bindFramebuffer = reinterpret_cast<decltype(s.bindFramebuffer)>(gl_proc("glBindFramebuffer"));
    if (s.bindFramebuffer == nullptr) s.readPixels = nullptr;  // sin enlace no hay muestra fiable
    screen_size(s, w, h);
    std::lock_guard lock(s.statsMutex);
    s.stats.width = w;
    s.stats.height = h;
    s.stats.contextReady = true;
    log_line("contexto GLES listo (%s, %dx%d) en el hilo choya-pantalla", mode_name(s.mode), w, h);
  } else {
    log_line("la pantalla de Choya no se pudo abrir en modo %s: el juego no tiene salida gráfica", mode_name(s.mode));
    destroy_context(s);
  }
  ready->set_value(ok);
  if (!ok) {
    s.running.store(false);
    return;
  }
  auto lastReport = std::chrono::steady_clock::now();
  Stats lastStats;
  for (;;) {
    uint64_t target = 0;
    {
      std::unique_lock lock(s.mutex);
      s.cv.wait(lock, [&] { return s.stopping || s.requested != s.handled; });
      if (s.stopping) break;
      target = s.requested;
    }
    screen_size(s, w, h);
    // El núcleo dibuja todos los grupos en cola, en orden; devuelve true si el último se muestra.
    const bool drawn = choya::gx::draw_frame(w, h);
    bool presented = false;
    uint8_t probes[Stats::kMaxProbes][4] = {};
    const bool probing = drawn && s.probeCount > 0 && s.readPixels != nullptr;
    if (probing) {
      s.bindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, 0);  // la imagen que se presenta, no el EFB
      for (int i = 0; i < s.probeCount; ++i) {
        if (s.probeXY[i][1] < h) {
          s.readPixels(s.probeXY[i][0], h - 1 - s.probeXY[i][1], 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/,
                       probes[i]);
        }
      }
    }
    if (drawn) presented = present(s);
    // Lecturas del EFB después de presentar: leen las copias propias del lote del token (enlazan otros framebuffers).
    const uint64_t served = galaxy_choya::serve_readbacks();
    {
      std::lock_guard lock(s.statsMutex);
      ++s.stats.drawPasses;
      s.stats.readbacks += served;
      s.stats.width = w;
      s.stats.height = h;
      if (drawn) ++s.stats.framesDrawn;
      if (probing) {
        std::memcpy(s.stats.probes, probes, sizeof probes);
        std::memcpy(s.stats.probe, probes[0], 4);
        s.stats.probeCount = s.probeCount;
        s.stats.probed = true;
      }
      if (presented) ++s.stats.imagesPresented;
      if (drawn && !presented) ++s.stats.presentFailures;
      s.stats.logicFrames = galaxy_choya::stats().frames;
    }
    {
      std::lock_guard lock(s.mutex);
      s.handled = target;
    }
    s.idleCv.notify_all();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) {
      const double secs = std::chrono::duration<double>(now - lastReport).count();
      Stats cur = stats();
      log_line("en %.0f s: %.1f cuadros de lógica/s, %.1f dibujados/s, %.1f imágenes presentadas/s, %llu lecturas del "
               "EFB, %llu fallos al presentar",
               secs, (cur.logicFrames - lastStats.logicFrames) / secs, (cur.framesDrawn - lastStats.framesDrawn) / secs,
               (cur.imagesPresented - lastStats.imagesPresented) / secs,
               static_cast<unsigned long long>(cur.readbacks - lastStats.readbacks),
               static_cast<unsigned long long>(cur.presentFailures));
      lastStats = cur;
      lastReport = now;
    }
  }
  if (s.finish != nullptr) s.finish();
  destroy_context(s);
  {
    std::lock_guard lock(s.statsMutex);
    s.stats.contextReady = false;
  }
  log_line("hilo de pantalla detenido; contexto liberado");
}

void wake_from_core() { notify(); }

} // namespace

Mode requested_mode() noexcept {
  static const Mode m = parse_mode();
  return m;
}

const char* mode_name(Mode m) noexcept {
  switch (m) {
  case Mode::Sdl: return "sdl";
  case Mode::Fbdev: return "fbdev";
  case Mode::Pbuffer: return "pbuffer";
  }
  return "?";
}

bool start(SDL_Window* window) noexcept {
  if (!galaxy_choya::enabled()) return false;
  State& s = st();
  if (s.running.load()) return true;
  s.window = window;
  s.mode = requested_mode();
  s.stopping = false;
  s.stats = Stats{};
  s.stats.mode = s.mode;
  if (s.mode == Mode::Pbuffer) {
    const char* sz = std::getenv("GALAXY_PRESENT_SIZE");  // AxB (pruebas)
    int pw = 640, ph = 480;
    if (sz != nullptr) std::sscanf(sz, "%dx%d", &pw, &ph);
    s.stats.width = pw;
    s.stats.height = ph;
  }
  s.probeCount = 0;
  if (const char* pr = std::getenv("GALAXY_PRESENT_PROBE"); pr != nullptr) {
    for (const char* p = pr; p != nullptr && *p != '\0' && s.probeCount < Stats::kMaxProbes;) {
      int x = -1, y = -1;
      if (std::sscanf(p, "%d,%d", &x, &y) == 2 && x >= 0 && y >= 0) {
        s.probeXY[s.probeCount][0] = x;
        s.probeXY[s.probeCount][1] = y;
        ++s.probeCount;
      }
      p = std::strchr(p, ';');
      if (p != nullptr) ++p;
    }
  }
  log_line("modo de presentación: %s%s", mode_name(s.mode),
           std::getenv("GALAXY_PRESENT") == nullptr ? " (auto)" : "");
  choya::gx::set_wake(wake_from_core);
  s.running.store(true);
  std::promise<bool> ready;
  auto result = ready.get_future();
  s.thread = std::thread(render_thread, &ready);
  const bool ok = result.get();
  if (!ok && s.thread.joinable()) s.thread.join();
  return ok;
}

void stop() noexcept {
  State& s = st();
  if (!s.thread.joinable()) return;
  {
    std::lock_guard lock(s.mutex);
    s.stopping = true;
  }
  s.cv.notify_all();
  s.idleCv.notify_all();
  s.thread.join();
  s.running.store(false);
  choya::gx::set_wake(nullptr);
}

bool running() noexcept { return st().running.load(); }

void notify() noexcept {
  State& s = st();
  {
    std::lock_guard lock(s.mutex);
    ++s.requested;
  }
  s.cv.notify_one();
}

Stats stats() noexcept {
  State& s = st();
  std::lock_guard lock(s.statsMutex);
  return s.stats;
}

bool wait_idle(int timeoutMs) noexcept {
  State& s = st();
  std::unique_lock lock(s.mutex);
  const uint64_t target = s.requested;
  return s.idleCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [&] { return s.handled >= target || s.stopping || !s.running.load(); });
}

} // namespace galaxy_choya::host

extern "C" bool galaxy_choya_presents() { return galaxy_choya::enabled(); }

extern "C" uint64_t galaxy_choya_window_flags() {
  if (!galaxy_choya::enabled()) return 0;
  return galaxy_choya::host::requested_mode() == galaxy_choya::host::Mode::Sdl ? SDL_WINDOW_OPENGL : 0;
}

extern "C" bool galaxy_choya_host_start(void* sdlWindow) {
  return galaxy_choya::host::start(static_cast<SDL_Window*>(sdlWindow));
}

extern "C" void galaxy_choya_host_stop() { galaxy_choya::host::stop(); }
