// LD_PRELOAD loader: owns the hooks (engine tick, buffer swap, player input) and forwards them to the
// reloadable mod library (libolportal_mod.so). Touch run/reload to reload the mod without restarting Outlast.
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "host.h"
#include "syms.h"

static char g_root[512];  // ~/outlast-portal-gun (POD: safe from static-init order)
static Host g_host;
static void* g_lib;
static int g_gen;
static ModTickFn g_tick;
static ModSwapFn g_swap;
static ModInputFn g_input;
static TickFn g_orig_tick;
static InputKeyFn g_orig_input;

static void* patch_slot(void** slot, void* fn) {
  long pg = sysconf(_SC_PAGESIZE);
  void* page = (void*)((uintptr_t)slot & ~(uintptr_t)(pg - 1));
  if (mprotect(page, pg * 2, PROT_READ | PROT_WRITE) != 0) return nullptr;
  void* old = *slot;
  *slot = fn;
  mprotect(page, pg * 2, PROT_READ);
  return old;
}

static void load_mod() {
  if (g_lib) {
    if (auto un = (ModUnloadFn)dlsym(g_lib, "mod_unload")) un();
    g_tick = nullptr; g_swap = nullptr; g_input = nullptr;
    dlclose(g_lib);
    g_lib = nullptr;
  }
  // dlopen caches by path, so load a fresh copy each time
  std::string src = std::string(g_root) + "/build/libolportal_mod.so";
  std::string dst = std::string(g_root) + "/run/.mod" + std::to_string(++g_gen) + ".so";
  std::string old = std::string(g_root) + "/run/.mod" + std::to_string(g_gen - 1) + ".so";
  unlink(old.c_str());
  FILE *in = fopen(src.c_str(), "rb"), *out = fopen(dst.c_str(), "wb");
  if (in && out) { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out); }
  if (in) fclose(in);
  if (out) fclose(out);
  g_lib = dlopen(dst.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!g_lib) { fprintf(stderr, "[olportal] dlopen failed: %s\n", dlerror()); return; }
  auto init = (ModInitFn)dlsym(g_lib, "mod_init");
  g_tick = (ModTickFn)dlsym(g_lib, "mod_tick");
  g_swap = (ModSwapFn)dlsym(g_lib, "mod_swap");
  g_input = (ModInputFn)dlsym(g_lib, "mod_input");
  if (init) init(&g_host);
}

static void hooked_tick(void* self, float dt) {
  g_orig_tick(self, dt);
  std::string flag = std::string(g_root) + "/run/reload";
  if (access(flag.c_str(), F_OK) == 0) { unlink(flag.c_str()); load_mod(); }
  if (g_tick) g_tick(self, dt);
}

static unsigned input_stub(void* self, int ctrl, FName key, int event, float amount, unsigned gamepad) {
  if (g_input) return g_input(self, ctrl, key, event, amount, gamepad, g_orig_input);
  return g_orig_input(self, ctrl, key, event, amount, gamepad);
}

static bool hook_input(void** slot) {
  if (*slot == (void*)input_stub) return true;
  void* old = patch_slot(slot, (void*)input_stub);
  if (!old) return false;
  g_orig_input = (InputKeyFn)old;
  return true;
}

extern "C" void SDL_GL_SwapWindow(void* win) {
  static auto real = (void (*)(void*))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
  if (g_swap) g_swap(win);
  real(win);
}

__attribute__((constructor)) static void init() {
  char exe[512] = {0};
  if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0 || !strstr(exe, "OLGame.x86_64")) return;
  const char* home = getenv("HOME");
  snprintf(g_root, sizeof g_root, "%s/outlast-portal-gun", home ? home : "/tmp");
  mkdir((std::string(g_root) + "/run").c_str(), 0755);
  g_host.root = g_root;
  g_host.hook_input = hook_input;
  void** vt = (void**)(A_VT_UOLEngine + 16);
  for (int i = 0; i < 200; ++i)
    if (vt[i] == (void*)A_UOLEngine_Tick) {
      g_orig_tick = (TickFn)patch_slot(&vt[i], (void*)hooked_tick);
      break;
    }
  if (!g_orig_tick) { fprintf(stderr, "[olportal] UOLEngine::Tick not found: wrong Outlast build, mod disabled\n"); return; }
  load_mod();
}
