// Tiny mixer for the Portal 2 sounds (cache/sounds/*.wav, 44.1 kHz s16 stereo from tools/p2gun.py). The loader
// wraps Outlast's own SDL audio callback (Wwise output) and calls snd::mix after the game has filled the buffer.
#pragma once
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace snd {
struct Voice { const std::vector<short>* pcm; double pos; float vol; };
static std::map<std::string, std::vector<std::vector<short>>> bank;
static std::vector<Voice> voices;
static std::mutex mx;

static std::vector<short> load_wav(const std::string& path) {
  std::vector<short> pcm;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return pcm;
  std::vector<unsigned char> d;
  unsigned char b[65536];
  size_t r;
  while ((r = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + r);
  fclose(f);
  for (size_t p = 12; p + 8 <= d.size();) {  // RIFF chunks: find "data"
    unsigned sz = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (unsigned)d[p + 7] << 24;
    if (!memcmp(&d[p], "data", 4) && p + 8 + sz <= d.size()) {
      pcm.resize(sz / 2);
      memcpy(pcm.data(), &d[p + 8], pcm.size() * 2);
      break;
    }
    p += 8 + sz + (sz & 1);
  }
  return pcm;
}

static void load(const std::string& dir) {
  if (!bank.empty()) return;
  for (const char* n : {"fire0", "fire1", "open0", "open1", "enter", "exit", "invalid", "close"})
    for (int k = 0; k < 8; ++k) {
      auto pcm = load_wav(dir + "/" + n + "_" + std::to_string(k) + ".wav");
      if (pcm.empty()) break;
      bank[n].push_back(std::move(pcm));
    }
}

// mix into the device buffer: s16 (0x8010) or f32 (0x8120), any channel count (first two get L/R), any rate
static void mix(unsigned char* out, int len, int freq, unsigned short format, int channels) {
  bool f32 = format == 0x8120;
  int bps = f32 ? 4 : 2, frames = len / (bps * channels);
  double step = 44100.0 / (freq > 0 ? freq : 44100);
  std::lock_guard<std::mutex> l(mx);
  for (auto& v : voices) {
    for (int i = 0; i < frames; ++i, v.pos += step) {
      size_t s = (size_t)v.pos * 2;
      if (s + 1 >= v.pcm->size()) break;
      for (int c = 0; c < 2 && c < channels; ++c) {
        float x = (*v.pcm)[s + c] * v.vol;
        if (f32) ((float*)out)[i * channels + c] += x / 32768.f;
        else {
          short& o = ((short*)out)[i * channels + c];
          int y = o + (int)x;
          o = (short)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }
      }
    }
  }
  for (size_t k = 0; k < voices.size();)
    if ((size_t)voices[k].pos * 2 + 1 >= voices[k].pcm->size()) voices.erase(voices.begin() + k); else ++k;
}

static void play(const char* name, float vol = 0.7f) {
  auto it = bank.find(name);
  if (it == bank.end() || it->second.empty()) return;
  std::lock_guard<std::mutex> l(mx);
  voices.push_back({&it->second[rand() % it->second.size()], 0, vol});
}
}  // namespace snd
