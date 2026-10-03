// Reports crashes in host libraries of app processes.
//
// When a host library faults under the translator (the Vulkan or EGL proxies, the drivers, Prism's
// own replacements), the translator's handler gives up and reverts the signal to SIG_DFL, so the
// process dies without a tombstone. Ahead of it (and of ART's fault handler) this logs the faulting
// pc and a stack scan for return addresses, as module+offset, then lets the signal go on unchanged
// (and for a null call in arm64 code, the guest's call chain; see report_guest). Host code that
// calls an arm64 function directly (an arm64 window's or callback's function pointer) faults in
// the arm64 library; that is reported too, with the host callers.
// ART's own faults (implicit null and stack checks, in compiled Java) are not in a host .so, and
// pass through silently. Only the preloaded libprism_jni has it: libsigchain takes two handlers.
#ifndef PRISM_STUB_LIB
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define TAG "PrismFault"
#define LOG(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define SCAN_WORDS 1024
#define MAX_FRAMES 32

struct SigchainAction {  // libsigchain's (art/sigchainlib/sigchain.h)
  bool (*sc_sigaction)(int, siginfo_t *, void *);
  sigset_t sc_mask;
  uint64_t sc_flags;
};
typedef void (*AddSpecialSignalHandlerFn)(int, struct SigchainAction *);

static bool host_module(uintptr_t addr, Dl_info *info) {
  if (!dladdr((void *)addr, info) || !info->dli_fname) return false;
  size_t n = strlen(info->dli_fname);
  return n > 3 && !strcmp(info->dli_fname + n - 3, ".so") && !strstr(info->dli_fname, "/libart");
}

static void describe(const char *what, uintptr_t addr, const Dl_info *info) {
  LOG("  %s %#lx %s+%#lx (%s+%#lx)", what, (unsigned long)addr, info->dli_fname,
      (unsigned long)(addr - (uintptr_t)info->dli_fbase), info->dli_sname ? info->dli_sname : "?",
      (unsigned long)(addr - (uintptr_t)info->dli_saddr));
}

// Faults in translated arm64 code don't come here as such: the translator delivers them to the
// guest, and a guest call through a null pointer is raised by the translator itself, with
// rt_tgsigqueueinfo (so it arrives at the host syscall instruction). Its siginfo is on the host
// stack, next to the guest thread's state: the argument ExecuteGuest was called with, saved beside
// its return address. That state starts with the guest's x0..x30, and x29's frame records give
// the guest's call chain, in arm64 libraries the host linker doesn't know: those are named from
// /proc/self/maps.
static char g_maps[4 << 20];

static size_t read_maps(void) {
  int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  size_t n = 0;
  ssize_t got;
  while (n < sizeof(g_maps) - 1 && (got = read(fd, g_maps + n, sizeof(g_maps) - 1 - n)) > 0) n += got;
  close(fd);
  g_maps[n] = 0;
  return n;
}

// The readable mapping holding addr: its end, file offset at addr and path (len 0 if anonymous).
static bool find_mapping(uintptr_t addr, uintptr_t *end, uintptr_t *offset, const char **path, int *path_len) {
  for (char *line = g_maps; *line;) {
    char *eol = strchr(line, '\n');
    if (!eol) eol = line + strlen(line);
    char *cursor = line;
    uintptr_t lo = strtoull(cursor, &cursor, 16), hi = strtoull(cursor + 1, &cursor, 16);
    if (addr >= lo && addr < hi) {
      char *perms = cursor + 1;
      uintptr_t off = strtoull(perms + 5, &cursor, 16);
      for (int field = 0; field < 2 && cursor < eol; field++) {  // dev, inode
        while (cursor < eol && *cursor == ' ') cursor++;
        while (cursor < eol && *cursor != ' ') cursor++;
      }
      while (cursor < eol && *cursor == ' ') cursor++;
      *end = hi, *offset = addr - lo + off, *path = cursor, *path_len = (int)(eol - cursor);
      return perms[0] == 'r';
    }
    line = *eol ? eol + 1 : eol;
  }
  return false;
}

static bool readable(uintptr_t addr, size_t size) {
  uintptr_t end, offset;
  const char *path;
  int len;
  return find_mapping(addr, &end, &offset, &path, &len) && addr + size <= end;
}

static void describe_guest(const char *what, uintptr_t addr) {
  uintptr_t end, offset;
  const char *path;
  int len;
  if (find_mapping(addr, &end, &offset, &path, &len) && len > 0)
    LOG("  %s %#lx %.*s+%#lx", what, (unsigned long)addr, len, path, (unsigned long)offset);
  else
    LOG("  %s %#lx", what, (unsigned long)addr);
}

static void report_guest(uintptr_t siginfo) {
  if (!read_maps() || !readable(siginfo, 64 * sizeof(uint64_t))) return;
  const uint64_t *words = (const uint64_t *)siginfo, *state = NULL;
  for (int i = 4; i < 62 && !state; i++) {
    Dl_info at;
    if (dladdr((void *)words[i], &at) && at.dli_sname && strstr(at.dli_sname, "ExecuteGuestEPNS"))
      state = (const uint64_t *)words[i + 2];
  }
  if (!state || !readable((uintptr_t)state, 31 * sizeof(uint64_t))) return;
  describe_guest("guest x30", state[30]);
  uintptr_t fp = state[29];
  for (int depth = 0; depth < MAX_FRAMES && fp && readable(fp, 16); depth++) {
    const uint64_t *record = (const uint64_t *)fp;  // {caller's x29, return address}
    describe_guest("guest lr ", record[1]);
    if (record[0] <= fp) break;
    fp = record[0];
  }
}

static bool on_fault(int sig, siginfo_t *si, void *context) {
  static _Atomic int reporting;
  ucontext_t *uc = context;
#if defined(__x86_64__)
  uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP], sp = uc->uc_mcontext.gregs[REG_RSP];
#else
  uintptr_t pc = uc->uc_mcontext.pc, sp = uc->uc_mcontext.sp;
#endif
  Dl_info info;
  // Host code that jumps into arm64 code (an arm64 callback called directly) faults on executing a
  // page that isn't executable for the host: si_addr is the pc itself.
  bool guest_jump = sig == SIGSEGV && si->si_code == SEGV_ACCERR && (uintptr_t)si->si_addr == pc;
  if (!(guest_jump || host_module(pc, &info)) || reporting++) return false;
  LOG("signal %d code %d addr %p in tid %d", sig, si->si_code, si->si_addr, gettid());
  if (guest_jump && read_maps())
    describe_guest("host code called arm64 code at", pc);
  else
    describe("pc", pc, &info);
#if defined(__x86_64__)
  if (!guest_jump && info.dli_sname && !strcmp(info.dli_sname, "syscall")) {
    greg_t *r = uc->uc_mcontext.gregs;
    LOG("  syscall args %#llx %#llx %#llx %#llx", (unsigned long long)r[REG_RDI], (unsigned long long)r[REG_RSI],
        (unsigned long long)r[REG_RDX], (unsigned long long)r[REG_R10]);
    if (sig == SIGSEGV && r[REG_RDX] == SIGSEGV) report_guest(r[REG_R10]);
  }
#endif
  uintptr_t top = sp + SCAN_WORDS * sizeof(uintptr_t);
  pthread_attr_t attr;
  void *base;
  size_t size;
  if (!pthread_getattr_np(pthread_self(), &attr) && !pthread_attr_getstack(&attr, &base, &size) &&
      sp >= (uintptr_t)base && sp < (uintptr_t)base + size && (uintptr_t)base + size < top)
    top = (uintptr_t)base + size;
  int frames = 0;
  for (uintptr_t *p = (uintptr_t *)sp; (uintptr_t)p < top && frames < MAX_FRAMES; p++) {
    if (host_module(*p, &info)) {
      describe("stack", *p, &info);
      frames++;
    }
  }
  return false;
}

__attribute__((constructor)) static void fault_report_init(void) {
  AddSpecialSignalHandlerFn add = (AddSpecialSignalHandlerFn)dlsym(RTLD_DEFAULT, "AddSpecialSignalHandlerFn");
  if (!add) return;  // not under ART (an exec'd tool)
  struct SigchainAction action = {.sc_sigaction = on_fault};
  sigemptyset(&action.sc_mask);
  add(SIGSEGV, &action);
}

#endif
