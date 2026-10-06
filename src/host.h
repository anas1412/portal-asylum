// Interface between the LD_PRELOAD loader (loader.cpp) and the reloadable mod (mod.cpp).
#pragma once
#ifndef OLPORTAL_FNAME
#define OLPORTAL_FNAME
struct FName { int Index, Number; };
#endif

typedef void (*TickFn)(void* engine, float dt);
typedef unsigned (*InputKeyFn)(void* self, int ctrl, FName key, int event, float amount, unsigned gamepad);

// mixes into the game's own audio buffer (SDL format code, rate, channels of Outlast's device)
typedef void (*AudioMixFn)(unsigned char* out, int len, int freq, unsigned short format, int channels);

struct Host {
  const char* root;                       // ~/outlast-portal-gun
  bool (*hook_input)(void** slot);        // route a PlayerInput vtable InputKey slot through mod_input
  void (*set_audio_mix)(AudioMixFn fn);   // mix extra sounds into Outlast's audio callback (nullptr to stop)
};

typedef void (*ModInitFn)(Host*);
typedef void (*ModUnloadFn)();
typedef void (*ModTickFn)(void* engine, float dt);
typedef void (*ModSwapFn)(void* window);
typedef unsigned (*ModInputFn)(void* self, int ctrl, FName key, int event, float amount, unsigned gamepad,
                               InputKeyFn orig);
