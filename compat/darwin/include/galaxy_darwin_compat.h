// galaxy-choya-native: capa Darwin → Linux para el código nativo de MacioGalaxy.
//
// MacioGalaxy se escribió para macOS/arm64 y usa algunas API de Mach, libdispatch y libSystem en su planificador de
// hilos, VI, audio y diagnósticos. En vez de reescribir esos archivos (y divergir de MacioGalaxy en cada
// actualización), estas cabeceras dan en Linux las mismas firmas con implementaciones Linux reales:
//
//   mach_port_t / thread_act_t  = tid de Linux (gettid)
//   thread_suspend/resume       = señal en tiempo real: el hilo se detiene dentro del manejador (sem_wait) con sus
//                                 registros guardados; thread_resume lo libera. Anidable como en Mach.
//   thread_get_state            = registros guardados por el manejador (PC/LR/FP/SP de aarch64; RIP/RBP/RSP en x86-64)
//   thread_info(BASIC)          = /proc/self/task/<tid>/stat (estado y tiempo de CPU)
//   semaphore_* / dispatch_semaphore_* = sem_t POSIX
//   mach_absolute_time         = CLOCK_MONOTONIC en ns (timebase 1/1)
//   thread_policy_set(TIME_CONSTRAINT) = SCHED_FIFO si el proceso puede (root en KNULLI); si no, KERN_FAILURE
//   QoS                         = setpriority() del hilo (nice), nunca falla en silencio: devuelve el errno
//   getsegmentdata(__TEXT)      = segmento ejecutable del programa principal (dl_iterate_phdr)
//   sys_icache_invalidate       = __builtin___clear_cache
//   _NSGetArgv/_NSGetExecutablePath = /proc/self/cmdline y /proc/self/exe
//
// Lo que Linux no tiene equivalente (task_info TASK_VM_INFO, simbolización nlist de Mach-O) no se emula aquí: los
// archivos que lo usan llevan un parche explícito (patches/MacioGalaxy).
#pragma once

#if !defined(__linux__)
#error "galaxy_darwin_compat.h solo se usa en Linux"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int kern_return_t;
typedef int mach_port_t;  // tid
typedef mach_port_t thread_act_t;
typedef mach_port_t thread_t;
typedef mach_port_t task_t;
typedef unsigned int mach_msg_type_number_t;
typedef int* thread_state_t;
typedef int* thread_info_t;
typedef int thread_flavor_t;
typedef thread_act_t* thread_act_array_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;

#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_INVALID_ARGUMENT 4
#define KERN_OPERATION_TIMED_OUT 49
#define KERN_ABORTED 14
#define MACH_PORT_NULL 0

static inline pid_t galaxy_gettid(void) { return (pid_t)syscall(SYS_gettid); }

static inline task_t mach_task_self(void) { return (task_t)getpid(); }
static inline thread_t mach_thread_self(void) { return (thread_t)galaxy_gettid(); }
static inline kern_return_t mach_port_deallocate(task_t task, mach_port_t port) {
  (void)task;
  (void)port;
  return KERN_SUCCESS;
}

// pthread ↔ tid. pthread_mach_thread_np registra la pareja la primera vez que la llama el propio hilo; para otro
// hilo solo funciona si ese hilo ya se registró (si no, MACH_PORT_NULL: el llamador lo trata como "sin puerto").
mach_port_t pthread_mach_thread_np(pthread_t thread);
pthread_t pthread_from_mach_thread_np(mach_port_t port);
int pthread_main_np(void);
void* pthread_get_stackaddr_np(pthread_t thread);  // extremo alto de la pila, como en Darwin
size_t pthread_get_stacksize_np(pthread_t thread);

// Suspensión real de hilos (señal SIGRTMIN+3 + sem_wait en el manejador).
kern_return_t thread_suspend(thread_act_t thread);
kern_return_t thread_resume(thread_act_t thread);

// Estados de registros.
#if defined(__aarch64__)
typedef struct {
  uint64_t __x[29];
  uint64_t __fp, __lr, __sp, __pc;
  uint32_t __cpsr, __pad;
} arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define arm_thread_state64_get_pc(ts) ((ts).__pc)
#define arm_thread_state64_get_lr(ts) ((ts).__lr)
#define arm_thread_state64_get_fp(ts) ((ts).__fp)
#define arm_thread_state64_get_sp(ts) ((ts).__sp)
#else
// x86-64 (solo para compilar y probar en la PC): pc = RIP, fp = RBP, lr = dirección de retorno en [RBP+8]
// (válida solo si la función guardó RBP; el recorrido de marcos lo comprueba contra los límites de la pila).
typedef struct {
  uint64_t __fp, __lr, __sp, __pc;
} arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define arm_thread_state64_get_pc(ts) ((ts).__pc)
#define arm_thread_state64_get_lr(ts) ((ts).__lr)
#define arm_thread_state64_get_fp(ts) ((ts).__fp)
#define arm_thread_state64_get_sp(ts) ((ts).__sp)
#endif
kern_return_t thread_get_state(thread_act_t thread, int flavor, thread_state_t state, mach_msg_type_number_t* count);

// thread_info (THREAD_BASIC_INFO).
typedef struct {
  int seconds, microseconds;
} time_value_t;
typedef struct {
  time_value_t user_time, system_time;
  int cpu_usage, policy, run_state, flags, suspend_count, sleep_time;
} thread_basic_info_data_t;
typedef thread_basic_info_data_t* thread_basic_info_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5
#define TH_FLAGS_SWAPPED 0x1
#define TH_FLAGS_IDLE 0x2
kern_return_t thread_info(thread_act_t thread, int flavor, thread_info_t info, mach_msg_type_number_t* count);

// Lista de hilos del proceso (/proc/self/task). La memoria se libera con vm_deallocate.
kern_return_t task_threads(task_t task, thread_act_array_t* list, mach_msg_type_number_t* count);
kern_return_t vm_deallocate(task_t task, vm_address_t address, vm_size_t size);

// Política de tiempo real.
typedef struct {
  uint32_t period, computation, constraint;
  int preemptible;
} thread_time_constraint_policy_data_t;
typedef int* thread_policy_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT \
  ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
kern_return_t thread_policy_set(thread_act_t thread, int flavor, thread_policy_t policy, mach_msg_type_number_t count);

// Semáforos Mach.
typedef sem_t* semaphore_t;
typedef int clock_res_t;
typedef struct {
  unsigned int tv_sec;
  clock_res_t tv_nsec;
} mach_timespec_t;
#define SYNC_POLICY_FIFO 0
kern_return_t semaphore_create(task_t task, semaphore_t* semaphore, int policy, int value);
kern_return_t semaphore_destroy(task_t task, semaphore_t semaphore);
kern_return_t semaphore_signal(semaphore_t semaphore);
kern_return_t semaphore_wait(semaphore_t semaphore);
kern_return_t semaphore_timedwait(semaphore_t semaphore, mach_timespec_t wait_time);

// Tiempo.
typedef struct {
  uint32_t numer, denom;
} mach_timebase_info_data_t;
typedef mach_timebase_info_data_t* mach_timebase_info_t;
static inline kern_return_t mach_timebase_info(mach_timebase_info_t info) {
  info->numer = 1;
  info->denom = 1;
  return KERN_SUCCESS;
}
static inline uint64_t mach_absolute_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t mach_continuous_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
kern_return_t mach_wait_until(uint64_t deadline);  // clock_nanosleep TIMER_ABSTIME sobre CLOCK_MONOTONIC

#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t clock) {
  struct timespec ts;
  if (clock_gettime(clock, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// QoS (pthread/qos.h). Darwin reparte por clase; en Linux se traduce a nice por hilo:
//   USER_INTERACTIVE -2, USER_INITIATED -1, DEFAULT 0, UTILITY 5, BACKGROUND 10.
// Sin permiso para bajar el nice (usuario normal) devuelve EPERM, como haría cualquier llamada que falla.
typedef enum {
  QOS_CLASS_USER_INTERACTIVE = 0x21,
  QOS_CLASS_USER_INITIATED = 0x19,
  QOS_CLASS_DEFAULT = 0x15,
  QOS_CLASS_UTILITY = 0x11,
  QOS_CLASS_BACKGROUND = 0x09,
  QOS_CLASS_UNSPECIFIED = 0x00,
} qos_class_t;
typedef struct galaxy_qos_override* pthread_override_t;
int pthread_set_qos_class_self_np(qos_class_t qos, int relative_priority);
// Darwin eleva temporalmente la QoS de otro hilo (herencia de prioridad para el que tiene el "testigo" de la CPU de
// la Wii). En Linux se sube su nice mientras dure y se restaura al terminar; si no hay permiso, devuelve NULL.
pthread_override_t pthread_override_qos_class_start_np(pthread_t thread, qos_class_t qos, int relative_priority);
int pthread_override_qos_class_end_np(pthread_override_t override);

// libdispatch: solo los semáforos.
typedef sem_t* dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW ((dispatch_time_t)0)
#define DISPATCH_TIME_FOREVER (~(dispatch_time_t)0)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t dsema);
long dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout);  // 0 al obtenerlo
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
void dispatch_release(void* object);

// os/lock.h: os_unfair_lock → mutex sin recursión (Linux usa futex por debajo).
typedef struct {
  pthread_mutex_t m;
} os_unfair_lock;
typedef os_unfair_lock* os_unfair_lock_t;
#define OS_UNFAIR_LOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void os_unfair_lock_lock(os_unfair_lock_t lock) { pthread_mutex_lock(&lock->m); }
static inline void os_unfair_lock_unlock(os_unfair_lock_t lock) { pthread_mutex_unlock(&lock->m); }
static inline int os_unfair_lock_trylock(os_unfair_lock_t lock) { return pthread_mutex_trylock(&lock->m) == 0; }
static inline void os_unfair_lock_assert_owner(const os_unfair_lock* lock) { (void)lock; }

// libkern/OSCacheControl.h
static inline void sys_icache_invalidate(void* start, size_t len) {
  __builtin___clear_cache((char*)start, (char*)start + len);
}
static inline void sys_dcache_flush(void* start, size_t len) {
  __builtin___clear_cache((char*)start, (char*)start + len);
}

// crt_externs.h y mach-o/dyld.h
char*** _NSGetArgv(void);
int* _NSGetArgc(void);
int _NSGetExecutablePath(char* buf, uint32_t* bufsize);  // 0 o -1 con *bufsize = tamaño necesario

// mach-o/getsect.h: getsegmentdata(cabecera, "__TEXT", &tam) → inicio y tamaño del segmento ejecutable (PT_LOAD con
// PF_X) del objeto que contiene `header` (la base que da dladdr en dli_fbase).
struct mach_header_64;
uint8_t* getsegmentdata(const struct mach_header_64* header, const char* segname, unsigned long* size);

#ifdef __cplusplus
}

// Darwin: pthread_setname_np(nombre) nombra al hilo que llama. Linux pide el hilo y limita el nombre a 15 bytes.
#include <cstring>
inline int pthread_setname_np(const char* name) {
  char shortName[16];
  std::strncpy(shortName, name, sizeof shortName - 1);
  shortName[sizeof shortName - 1] = '\0';
  return pthread_setname_np(pthread_self(), shortName);
}
#endif
