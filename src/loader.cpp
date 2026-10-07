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
#include <mutex>
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

static void set_audio_mix(AudioMixFn fn);

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
    set_audio_mix(nullptr);
    if (auto un = (ModUnloadFn)dlsym(g_lib, "mod_unload")) un();
    g_tick = nullptr; g_swap = nullptr; g_input = nullptr;
    dlclose(g_lib);
    g_lib = nullptr;
  }
  // dlopen caches by path, so load a fresh copy each time
  std::string src = std::string(g_root) + "/build/libolportal_mod.so";
  if (access(src.c_str(), F_OK) != 0) src = std::string(g_root) + "/libolportal_mod.so";
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

// Outlast's audio: wrap the Wwise SDL callback so the mod can mix sounds into the same device
struct AudioSpec { int freq; unsigned short format; unsigned char channels, silence; unsigned short samples, pad; unsigned size;
                   void (*cb)(void*, unsigned char*, int); void* user; };
static void (*g_game_audio)(void*, unsigned char*, int);
static AudioSpec g_aspec;
static std::mutex g_amx;
static AudioMixFn g_mix;
static void audio_wrap(void* user, unsigned char* out, int len) {
  g_game_audio(user, out, len);
  std::lock_guard<std::mutex> l(g_amx);
  if (g_mix) g_mix(out, len, g_aspec.freq, g_aspec.format, g_aspec.channels);
}
static void set_audio_mix(AudioMixFn fn) { std::lock_guard<std::mutex> l(g_amx); g_mix = fn; }

extern "C" unsigned SDL_OpenAudioDevice(const char* dev, int capture, const AudioSpec* want, AudioSpec* have, int allowed) {
  static auto real = (unsigned (*)(const char*, int, const AudioSpec*, AudioSpec*, int))dlsym(RTLD_NEXT, "SDL_OpenAudioDevice");
  if (capture || !want || !want->cb || g_game_audio) return real(dev, capture, want, have, allowed);
  AudioSpec w = *want, got{};
  g_game_audio = w.cb;
  w.cb = audio_wrap;
  unsigned id = real(dev, capture, &w, &got, allowed);
  g_aspec = allowed ? got : w;
  if (have) { *have = got; have->cb = want->cb; have->user = want->user; }
  fprintf(stderr, "[olportal] game audio %d Hz fmt 0x%x ch %d\n", g_aspec.freq, g_aspec.format, g_aspec.channels);
  return id;
}

extern "C" void SDL_GL_SwapWindow(void* win) {
  static auto real = (void (*)(void*))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
  if (g_swap) g_swap(win);
  real(win);
}

__attribute__((constructor)) static void init() {
  char exe[512] = {0};
  if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0 || !strstr(exe, "OLGame.x86_64")) return;
  // the mod's folder: where this library lives (<root>/build/libolportal.so or <root>/libolportal.so)
  Dl_info info{};
  dladdr((void*)&init, &info);
  std::string so = info.dli_fname ? info.dli_fname : "";
  std::string dir = so.substr(0, so.find_last_of('/'));
  if (dir.size() >= 6 && dir.compare(dir.size() - 6, 6, "/build") == 0) dir.resize(dir.size() - 6);
  snprintf(g_root, sizeof g_root, "%s", dir.c_str());
  mkdir((std::string(g_root) + "/run").c_str(), 0755);
  g_host.root = g_root;
  g_host.hook_input = hook_input;
  g_host.set_audio_mix = set_audio_mix;
  void** vt = (void**)(A_VT_UOLEngine + 16);
  for (int i = 0; i < 200; ++i)
    if (vt[i] == (void*)A_UOLEngine_Tick) {
      g_orig_tick = (TickFn)patch_slot(&vt[i], (void*)hooked_tick);
      break;
    }
  if (!g_orig_tick) { fprintf(stderr, "[olportal] UOLEngine::Tick not found: wrong Outlast build, mod disabled\n"); return; }
  load_mod();
}
