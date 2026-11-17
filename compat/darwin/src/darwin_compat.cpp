// galaxy-choya-native: implementación Linux de la capa Darwin (ver include/galaxy_darwin_compat.h).
#include <galaxy_darwin_compat.h>

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- registro pthread ↔ tid
std::mutex gRegistryMutex;
std::unordered_map<pid_t, pthread_t>& registry() {
  static auto* m = new std::unordered_map<pid_t, pthread_t>();
  return *m;
}

// ---------------------------------------------------------------- suspensión por señal
// Cada hilo suspendido ocupa una ranura (máximo kSlots a la vez; Galaxy suspende uno por vez). El manejador corre en
// el hilo objetivo: guarda sus registros, avisa con `stopped` y espera `resume` con sem_wait (seguro en señales).
constexpr int kSlots = 16;
struct Slot {
  std::atomic<pid_t> tid{0};
  std::atomic<int> depth{0};  // anidamiento de thread_suspend
  sem_t stopped;
  sem_t resume;
  arm_thread_state64_t regs{};
  std::atomic<bool> haveRegs{false};
};
Slot gSlots[kSlots];
std::once_flag gSignalOnce;
int suspendSignal() { return SIGRTMIN + 3; }

Slot* findSlot(pid_t tid) {
  for (auto& s : gSlots) {
    if (s.tid.load(std::memory_order_acquire) == tid) return &s;
  }
  return nullptr;
}

void captureRegs(arm_thread_state64_t& out, const ucontext_t* uc) {
#if defined(__aarch64__)
  const auto& m = uc->uc_mcontext;
  for (int i = 0; i < 29; ++i) out.__x[i] = m.regs[i];
  out.__fp = m.regs[29];
  out.__lr = m.regs[30];
  out.__sp = m.sp;
  out.__pc = m.pc;
  out.__cpsr = static_cast<uint32_t>(m.pstate);
#elif defined(__x86_64__)
  const auto* g = uc->uc_mcontext.gregs;
  out.__pc = static_cast<uint64_t>(g[REG_RIP]);
  out.__fp = static_cast<uint64_t>(g[REG_RBP]);
  out.__sp = static_cast<uint64_t>(g[REG_RSP]);
  // x86-64 no tiene registro de enlace. Se entrega uno que apunta a la propia función, como el lr de una función
  // AArch64 que ya llamó a otra: MacioGalaxy recorre pc, lr y luego la cadena de fp, y un lr nulo cortaría ahí la
  // clasificación de marcos (la expropiación forzada no vería nunca el inicio del hilo).
  out.__lr = out.__pc + 1;
#else
#error "arquitectura no soportada por la capa Darwin"
#endif
}

// Rango ejecutable del binario principal: el muestreador de MacioGalaxy busca en la pila el primer marco suyo.
std::uintptr_t gExeLow = 0, gExeHigh = 0;

void findExeText() {
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void*) -> int {
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const auto& ph = info->dlpi_phdr[i];
          if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) != 0) {
            gExeLow = info->dlpi_addr + ph.p_vaddr;
            gExeHigh = gExeLow + ph.p_memsz;
          }
        }
        return 1;  // el primer objeto es el ejecutable
      },
      nullptr);
}

bool inExe(std::uintptr_t pc) { return pc >= gExeLow && pc < gExeHigh; }

#if defined(__aarch64__)
constexpr int kFpDwarfReg = 29;
#else
constexpr int kFpDwarfReg = 6;  // rbp
#endif

// Si el hilo se detuvo dentro de glibc (futex, nanosleep…), su fp no sirve para recorrer la pila: glibc no guarda
// punteros de marco y en x86-64 no hay registro de enlace. Con el desenrollador de libgcc (CFI, entiende el marco
// de señal) se busca el primer marco del ejecutable y se entrega como lr/fp, igual que lo vería el recorrido en
// macOS, donde libsystem sí conserva punteros de marco. El bloqueo de dl_iterate_phdr es recursivo en glibc.
struct UnwindScan {
  std::uintptr_t pc;
  std::uintptr_t stackLow;  // dirección de una variable del manejador: todo lo que se lee está por encima
  bool pastSignal = false;
  std::uintptr_t candIp = 0, candFp = 0;    // marco del ejecutable pendiente de comprobar
  std::uintptr_t firstIp = 0, firstFp = 0;  // respaldo: el primer marco del ejecutable
  std::uintptr_t ip = 0, fp = 0;
};

// El marco elegido debe dejar una cadena de punteros de marco coherente: [fp+8] (dirección de retorno en x86-64 y
// en el registro de marco {x29, x30} de AArch64) tiene que ser la IP del llamador según la CFI. Así se saltan las
// funciones de libc++ (enlazada estática, sin punteros de marco), que en macOS están fuera del ejecutable.
bool chainOk(const UnwindScan& scan, std::uintptr_t fp, std::uintptr_t callerIp) {
  if (fp <= scan.stackLow || fp - scan.stackLow > (std::uintptr_t{64} << 20) || (fp & 7) != 0) return false;
  return *reinterpret_cast<const std::uintptr_t*>(fp + 8) == callerIp;
}

_Unwind_Reason_Code unwindStep(_Unwind_Context* ctx, void* arg) {
  auto* scan = static_cast<UnwindScan*>(arg);
  int before = 0;
  const auto ip = static_cast<std::uintptr_t>(_Unwind_GetIPInfo(ctx, &before));
  if (!scan->pastSignal) {
    // Marcos del propio manejador hasta llegar al interrumpido (IP exacta = pc del contexto).
    if (ip == scan->pc) scan->pastSignal = true;
    return _URC_NO_REASON;
  }
  if (scan->candIp != 0 && chainOk(*scan, scan->candFp, ip)) {
    scan->ip = scan->candIp;
    scan->fp = scan->candFp;
    return _URC_END_OF_STACK;
  }
  scan->candIp = 0;
  if (inExe(ip)) {
    scan->candIp = ip;
    scan->candFp = static_cast<std::uintptr_t>(_Unwind_GetGR(ctx, kFpDwarfReg));
    if (scan->firstIp == 0) {
      scan->firstIp = scan->candIp;
      scan->firstFp = scan->candFp;
    }
  }
  return _URC_NO_REASON;
}

void recoverExeFrame(arm_thread_state64_t& regs) {
  if (gExeHigh == 0 || inExe(regs.__pc)) return;
  int here = 0;
  UnwindScan scan{static_cast<std::uintptr_t>(regs.__pc), reinterpret_cast<std::uintptr_t>(&here)};
  _Unwind_Backtrace(unwindStep, &scan);
  if (scan.ip == 0) {
    scan.ip = scan.firstIp;
    scan.fp = scan.firstFp;
  }
  if (scan.ip != 0) {
    regs.__lr = scan.ip;
    regs.__fp = scan.fp;
  }
}

void onSuspendSignal(int, siginfo_t*, void* ctx) {
  const int saved = errno;
  const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
  Slot* s = findSlot(self);
  if (s != nullptr) {
    captureRegs(s->regs, static_cast<const ucontext_t*>(ctx));
    recoverExeFrame(s->regs);
    s->haveRegs.store(true, std::memory_order_release);
    sem_post(&s->stopped);
    while (sem_wait(&s->resume) != 0 && errno == EINTR) {
    }
  }
  errno = saved;
}

void installSignal() {
  std::call_once(gSignalOnce, [] {
    findExeText();
    for (auto& s : gSlots) {
      sem_init(&s.stopped, 0, 0);
      sem_init(&s.resume, 0, 0);
    }
    struct sigaction sa {};
    sa.sa_sigaction = onSuspendSignal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);  // nada interrumpe al hilo mientras está detenido
    sigaction(suspendSignal(), &sa, nullptr);
  });
}

// ---------------------------------------------------------------- QoS → nice
int niceFor(qos_class_t q) {
  switch (q) {
  case QOS_CLASS_USER_INTERACTIVE: return -2;
  case QOS_CLASS_USER_INITIATED: return -1;
  case QOS_CLASS_UTILITY: return 5;
  case QOS_CLASS_BACKGROUND: return 10;
  default: return 0;
  }
}

std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

} // namespace

struct galaxy_qos_override {
  pid_t tid;
  int previousNice;
};

extern "C" {

mach_port_t pthread_mach_thread_np(pthread_t thread) {
  if (pthread_equal(thread, pthread_self())) {
    const pid_t tid = galaxy_gettid();
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    registry()[tid] = thread;
    return tid;
  }
  std::lock_guard<std::mutex> lock(gRegistryMutex);
  for (const auto& [tid, pt] : registry()) {
    if (pthread_equal(pt, thread)) return tid;
  }
  return MACH_PORT_NULL;
}

pthread_t pthread_from_mach_thread_np(mach_port_t port) {
  std::lock_guard<std::mutex> lock(gRegistryMutex);
  const auto it = registry().find(port);
  return it == registry().end() ? pthread_t{} : it->second;
}

int pthread_main_np(void) { return galaxy_gettid() == getpid() ? 1 : 0; }

void* pthread_get_stackaddr_np(pthread_t thread) {
  pthread_attr_t attr;
  if (pthread_getattr_np(thread, &attr) != 0) return nullptr;
  void* low = nullptr;
  size_t size = 0;
  pthread_attr_getstack(&attr, &low, &size);
  pthread_attr_destroy(&attr);
  return static_cast<char*>(low) + size;  // Darwin devuelve el extremo alto
}

size_t pthread_get_stacksize_np(pthread_t thread) {
  pthread_attr_t attr;
  if (pthread_getattr_np(thread, &attr) != 0) return 0;
  void* low = nullptr;
  size_t size = 0;
  pthread_attr_getstack(&attr, &low, &size);
  pthread_attr_destroy(&attr);
  return size;
}

kern_return_t thread_suspend(thread_act_t thread) {
  if (thread == MACH_PORT_NULL) return KERN_INVALID_ARGUMENT;
  if (thread == galaxy_gettid()) return KERN_INVALID_ARGUMENT;  // suspenderse a sí mismo no se usa en Galaxy
  installSignal();
  Slot* s = findSlot(thread);
  if (s != nullptr) {  // ya suspendido: solo anida
    s->depth.fetch_add(1, std::memory_order_acq_rel);
    return KERN_SUCCESS;
  }
  for (auto& slot : gSlots) {
    pid_t expected = 0;
    if (slot.tid.compare_exchange_strong(expected, thread, std::memory_order_acq_rel)) {
      s = &slot;
      break;
    }
  }
  if (s == nullptr) return KERN_FAILURE;
  s->depth.store(1, std::memory_order_release);
  s->haveRegs.store(false, std::memory_order_release);
  if (syscall(SYS_tgkill, getpid(), thread, suspendSignal()) != 0) {
    s->tid.store(0, std::memory_order_release);
    return KERN_FAILURE;
  }
  // Espera a que el hilo se detenga (con límite: un hilo que terminó no responde).
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 1;
  while (sem_timedwait(&s->stopped, &deadline) != 0) {
    if (errno == EINTR) continue;
    // No se detuvo: puede llegar tarde. Se deja una entrada de reanudación para que no quede bloqueado si lo hace.
    sem_post(&s->resume);
    s->tid.store(0, std::memory_order_release);
    return KERN_FAILURE;
  }
  return KERN_SUCCESS;
}

kern_return_t thread_resume(thread_act_t thread) {
  Slot* s = findSlot(thread);
  if (s == nullptr) return KERN_FAILURE;
  if (s->depth.fetch_sub(1, std::memory_order_acq_rel) > 1) return KERN_SUCCESS;
  s->tid.store(0, std::memory_order_release);
  sem_post(&s->resume);
  return KERN_SUCCESS;
}

kern_return_t thread_get_state(thread_act_t thread, int flavor, thread_state_t state, mach_msg_type_number_t* count) {
  if (flavor != ARM_THREAD_STATE64 || count == nullptr || *count < ARM_THREAD_STATE64_COUNT) {
    return KERN_INVALID_ARGUMENT;
  }
  Slot* s = findSlot(thread);
  if (s == nullptr || !s->haveRegs.load(std::memory_order_acquire)) return KERN_FAILURE;  // hay que suspenderlo antes
  std::memcpy(state, &s->regs, sizeof(arm_thread_state64_t));
  *count = ARM_THREAD_STATE64_COUNT;
  return KERN_SUCCESS;
}

kern_return_t thread_info(thread_act_t thread, int flavor, thread_info_t info, mach_msg_type_number_t* count) {
  if (flavor != THREAD_BASIC_INFO || count == nullptr || *count < THREAD_BASIC_INFO_COUNT) {
    return KERN_INVALID_ARGUMENT;
  }
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", thread);
  const std::string stat = readFile(path);
  const size_t close = stat.rfind(')');
  if (close == std::string::npos || close + 2 >= stat.size()) return KERN_FAILURE;
  // Campos tras el nombre: estado(3) ... utime(14) stime(15) en ticks de reloj.
  std::vector<std::string> f;
  size_t p = close + 2;
  while (p < stat.size()) {
    size_t q = stat.find(' ', p);
    if (q == std::string::npos) q = stat.size();
    f.emplace_back(stat.substr(p, q - p));
    p = q + 1;
  }
  if (f.size() < 13) return KERN_FAILURE;
  auto* out = reinterpret_cast<thread_basic_info_data_t*>(info);
  std::memset(out, 0, sizeof *out);
  const char st = f[0].empty() ? '?' : f[0][0];
  out->run_state = st == 'R' ? TH_STATE_RUNNING
                   : st == 'D' ? TH_STATE_UNINTERRUPTIBLE
                   : st == 'T' || st == 't' ? TH_STATE_STOPPED
                   : st == 'Z' || st == 'X' ? TH_STATE_HALTED
                                            : TH_STATE_WAITING;
  const long hz = sysconf(_SC_CLK_TCK);
  const unsigned long long ut = std::strtoull(f[11].c_str(), nullptr, 10);
  const unsigned long long stt = std::strtoull(f[12].c_str(), nullptr, 10);
  out->user_time.seconds = static_cast<int>(ut / hz);
  out->user_time.microseconds = static_cast<int>((ut % hz) * 1000000ull / hz);
  out->system_time.seconds = static_cast<int>(stt / hz);
  out->system_time.microseconds = static_cast<int>((stt % hz) * 1000000ull / hz);
  Slot* s = findSlot(thread);
  out->suspend_count = s ? s->depth.load(std::memory_order_relaxed) : 0;
  *count = THREAD_BASIC_INFO_COUNT;
  return KERN_SUCCESS;
}

kern_return_t task_threads(task_t, thread_act_array_t* list, mach_msg_type_number_t* count) {
  std::vector<thread_act_t> tids;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids.push_back(std::atoi(e->d_name));
    }
    closedir(d);
  }
  if (tids.empty()) return KERN_FAILURE;
  auto* out = static_cast<thread_act_t*>(std::malloc(tids.size() * sizeof(thread_act_t)));
  if (!out) return KERN_FAILURE;
  std::memcpy(out, tids.data(), tids.size() * sizeof(thread_act_t));
  *list = out;
  *count = static_cast<mach_msg_type_number_t>(tids.size());
  return KERN_SUCCESS;
}

kern_return_t vm_deallocate(task_t, vm_address_t address, vm_size_t) {
  std::free(reinterpret_cast<void*>(address));
  return KERN_SUCCESS;
}

kern_return_t thread_policy_set(thread_act_t thread, int flavor, thread_policy_t, mach_msg_type_number_t) {
  if (flavor != THREAD_TIME_CONSTRAINT_POLICY) return KERN_INVALID_ARGUMENT;
  // Tiempo real: SCHED_FIFO con prioridad media. Requiere CAP_SYS_NICE (root en KNULLI/muOS). Sin permiso, falla y el
  // llamador lo anota (MacioGalaxy ya informa "audio thread not realtime").
  struct sched_param sp {};
  sp.sched_priority = 20;
  if (sched_setscheduler(thread == galaxy_gettid() ? 0 : thread, SCHED_FIFO, &sp) != 0) return KERN_FAILURE;
  return KERN_SUCCESS;
}

kern_return_t semaphore_create(task_t, semaphore_t* semaphore, int, int value) {
  auto* s = static_cast<sem_t*>(std::malloc(sizeof(sem_t)));
  if (!s || sem_init(s, 0, static_cast<unsigned>(value)) != 0) {
    std::free(s);
    return KERN_FAILURE;
  }
  *semaphore = s;
  return KERN_SUCCESS;
}
kern_return_t semaphore_destroy(task_t, semaphore_t semaphore) {
  if (!semaphore) return KERN_INVALID_ARGUMENT;
  sem_destroy(semaphore);
  std::free(semaphore);
  return KERN_SUCCESS;
}
kern_return_t semaphore_signal(semaphore_t semaphore) { return sem_post(semaphore) == 0 ? KERN_SUCCESS : KERN_FAILURE; }
kern_return_t semaphore_wait(semaphore_t semaphore) {
  while (sem_wait(semaphore) != 0) {
    if (errno != EINTR) return KERN_FAILURE;
  }
  return KERN_SUCCESS;
}
kern_return_t semaphore_timedwait(semaphore_t semaphore, mach_timespec_t wait) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += wait.tv_sec;
  deadline.tv_nsec += wait.tv_nsec;
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000;
  }
  while (sem_timedwait(semaphore, &deadline) != 0) {
    if (errno == EINTR) continue;
    return errno == ETIMEDOUT ? KERN_OPERATION_TIMED_OUT : KERN_FAILURE;
  }
  return KERN_SUCCESS;
}

kern_return_t mach_wait_until(uint64_t deadline) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(deadline / 1000000000ull);
  ts.tv_nsec = static_cast<long>(deadline % 1000000000ull);
  int r;
  while ((r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {
  }
  return r == 0 ? KERN_SUCCESS : KERN_ABORTED;
}

int pthread_set_qos_class_self_np(qos_class_t qos, int relative) {
  const int nice = niceFor(qos) - relative;
  return setpriority(PRIO_PROCESS, static_cast<id_t>(galaxy_gettid()), nice) == 0 ? 0 : errno;
}

pthread_override_t pthread_override_qos_class_start_np(pthread_t thread, qos_class_t qos, int) {
  const pid_t tid = pthread_mach_thread_np(thread);
  if (tid == MACH_PORT_NULL) return nullptr;
  errno = 0;
  const int previous = getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
  if (errno != 0) return nullptr;
  const int target = niceFor(qos);
  if (target >= previous) return new galaxy_qos_override{tid, previous};  // ya tiene esa prioridad o más
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), target) != 0) return nullptr;
  return new galaxy_qos_override{tid, previous};
}

int pthread_override_qos_class_end_np(pthread_override_t o) {
  if (!o) return EINVAL;
  const int r = setpriority(PRIO_PROCESS, static_cast<id_t>(o->tid), o->previousNice) == 0 ? 0 : errno;
  delete o;
  return r;
}

dispatch_semaphore_t dispatch_semaphore_create(long value) {
  semaphore_t s = nullptr;
  return semaphore_create(0, &s, 0, static_cast<int>(value)) == KERN_SUCCESS ? s : nullptr;
}
long dispatch_semaphore_signal(dispatch_semaphore_t s) {
  sem_post(s);
  return 0;
}
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
  if (timeout == DISPATCH_TIME_FOREVER) return semaphore_wait(s) == KERN_SUCCESS ? 0 : 1;
  if (timeout == DISPATCH_TIME_NOW) {
    while (sem_trywait(s) != 0) {
      if (errno != EINTR) return 1;
    }
    return 0;
  }
  // dispatch_time da un instante absoluto de CLOCK_MONOTONIC en ns; sem_clockwait lo acepta directamente.
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(timeout / 1000000000ull);
  ts.tv_nsec = static_cast<long>(timeout % 1000000000ull);
  while (sem_clockwait(s, CLOCK_MONOTONIC, &ts) != 0) {
    if (errno == EINTR) continue;
    return 1;
  }
  return 0;
}
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
  if (when == DISPATCH_TIME_FOREVER) return when;
  const uint64_t base = when == DISPATCH_TIME_NOW ? mach_absolute_time() : when;
  return base + static_cast<uint64_t>(delta);
}
void dispatch_release(void* object) {
  if (object) semaphore_destroy(0, static_cast<semaphore_t>(object));
}

char*** _NSGetArgv(void) {
  static char** argv = [] {
    static std::string cmd = readFile("/proc/self/cmdline");
    static std::vector<char*> v;
    for (size_t i = 0; i < cmd.size();) {
      v.push_back(&cmd[i]);
      i += std::strlen(&cmd[i]) + 1;
    }
    v.push_back(nullptr);
    return v.data();
  }();
  return &argv;
}
int* _NSGetArgc(void) {
  static int argc = [] {
    int n = 0;
    for (char** a = *_NSGetArgv(); *a; ++a) ++n;
    return n;
  }();
  return &argc;
}
int _NSGetExecutablePath(char* buf, uint32_t* bufsize) {
  char tmp[4096];
  const ssize_t n = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
  if (n < 0) return -1;
  if (static_cast<uint32_t>(n) + 1 > *bufsize) {
    *bufsize = static_cast<uint32_t>(n) + 1;
    return -1;
  }
  std::memcpy(buf, tmp, static_cast<size_t>(n));
  buf[n] = '\0';
  return 0;
}

uint8_t* getsegmentdata(const struct mach_header_64* header, const char* segname, unsigned long* size) {
  if (std::strcmp(segname, "__TEXT") != 0) return nullptr;
  struct Find {
    uintptr_t base;
    uintptr_t lo = 0, hi = 0;
  } find{reinterpret_cast<uintptr_t>(header)};
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) -> int {
        auto* f = static_cast<Find*>(data);
        // El objeto cuyo primer segmento cargable empieza en la base de dladdr.
        uintptr_t first = UINTPTR_MAX;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            first = std::min<uintptr_t>(first, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
          }
        }
        if ((first & ~uintptr_t(0xFFF)) != (f->base & ~uintptr_t(0xFFF)) && info->dlpi_addr != f->base) return 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const auto& ph = info->dlpi_phdr[i];
          if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
            f->lo = info->dlpi_addr + ph.p_vaddr;
            f->hi = f->lo + ph.p_memsz;
            return 1;
          }
        }
        return 0;
      },
      &find);
  if (find.lo == 0) return nullptr;
  *size = find.hi - find.lo;
  return reinterpret_cast<uint8_t*>(find.lo);
}

// ---------------------------------------------------------------- dladdr con la .symtab del ejecutable
int __real_dladdr(const void* address, Dl_info* info);

} // extern "C"

namespace {
struct ExeSymbols {
  std::vector<uintptr_t> addr;
  std::vector<uintptr_t> size;
  std::vector<const char*> name;
  uintptr_t base = 0;  // dli_fbase del ejecutable
  const char* path = nullptr;
};
std::atomic<const ExeSymbols*> gExe{nullptr};

const ExeSymbols* buildExeSymbols() {
  auto* t = new ExeSymbols;
  const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  struct stat st {};
  if (fd < 0 || fstat(fd, &st) != 0) {
    if (fd >= 0) close(fd);
    return t;
  }
  void* map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) return t;
  const auto* b = static_cast<const unsigned char*>(map);
  const auto* eh = reinterpret_cast<const ElfW(Ehdr)*>(b);
  if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_shoff == 0) return t;
  uintptr_t bias = 0;
  dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
    *static_cast<uintptr_t*>(data) = info->dlpi_addr;
    return 1;
  }, &bias);
  const auto* sh = reinterpret_cast<const ElfW(Shdr)*>(b + eh->e_shoff);
  std::vector<std::tuple<uintptr_t, uintptr_t, const char*>> v;
  for (int i = 0; i < eh->e_shnum; ++i) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) continue;
    const char* str = reinterpret_cast<const char*>(b + sh[sh[i].sh_link].sh_offset);
    const auto* sym = reinterpret_cast<const ElfW(Sym)*>(b + sh[i].sh_offset);
    for (size_t k = 0; k < sh[i].sh_size / sizeof(ElfW(Sym)); ++k) {
      const int type = ELF64_ST_TYPE(sym[k].st_info);
      if (sym[k].st_shndx == SHN_UNDEF || sym[k].st_name == 0 || (type != STT_FUNC && type != STT_OBJECT)) continue;
      const char* n = str + sym[k].st_name;
      if (n[0] == '$' || n[0] == '.') continue;
      v.emplace_back(sym[k].st_value + bias, sym[k].st_size, n);
    }
  }
  std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
  for (const auto& [a, sz, n] : v) {
    t->addr.push_back(a);
    t->size.push_back(sz);
    t->name.push_back(n);
  }
  Dl_info self{};
  if (__real_dladdr(reinterpret_cast<const void*>(&buildExeSymbols), &self)) {
    t->base = reinterpret_cast<uintptr_t>(self.dli_fbase);
    t->path = self.dli_fname;
  }
  return t;
}

// Antes de que existan otros hilos: así ninguna búsqueda posterior reserva memoria ni toma bloqueos (el planificador
// llama a dladdr con hilos suspendidos).
__attribute__((constructor(101))) void prepareExeSymbols() { gExe.store(buildExeSymbols(), std::memory_order_release); }
} // namespace

extern "C" int __wrap_dladdr(const void* address, Dl_info* info) {
  const int r = __real_dladdr(address, info);
  if (r != 0 && info->dli_sname != nullptr) return r;
  const ExeSymbols* t = gExe.load(std::memory_order_acquire);
  if (t == nullptr || t->addr.empty()) return r;
  const auto a = reinterpret_cast<uintptr_t>(address);
  if (r != 0 && reinterpret_cast<uintptr_t>(info->dli_fbase) != t->base) return r;  // otra biblioteca
  auto it = std::upper_bound(t->addr.begin(), t->addr.end(), a);
  if (it == t->addr.begin()) return r;
  size_t i = static_cast<size_t>(it - t->addr.begin()) - 1;
  // Funciones plegadas comparten dirección: el primero en orden de la tabla, como dladdr.
  while (i > 0 && t->addr[i - 1] == t->addr[i]) --i;
  if (t->size[i] != 0 && a >= t->addr[i] + t->size[i]) return r;  // entre símbolos
  if (r == 0) {
    info->dli_fname = t->path;
    info->dli_fbase = reinterpret_cast<void*>(t->base);
  }
  info->dli_sname = t->name[i];
  info->dli_saddr = reinterpret_cast<void*>(t->addr[i]);
  return 1;
}
