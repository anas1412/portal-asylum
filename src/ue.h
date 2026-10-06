// Minimal UE3 (Outlast, build 12048, x86_64 Linux) object access through the engine's own reflection.
// Layout offsets were confirmed by disassembly; see MODLOG.md.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "syms.h"

typedef unsigned char u8;
#ifndef OLPORTAL_FNAME
#define OLPORTAL_FNAME
struct FName { int Index, Number; };
#endif
template <class T> struct TArray { T* Data; int Num, Max; };
struct FVector { float X, Y, Z; };
struct FRotator { int Pitch, Yaw, Roll; };
struct FString { wchar_t* Data; int Num, Max; };  // TCHAR is 4-byte wchar_t on this port

void mlog(const char* fmt, ...);

namespace ue {
enum { O_Outer = 0x40, O_Name = 0x48, O_Class = 0x50, O_Next = 0x60, O_Super = 0x78, O_Children = 0x80,
       O_PropsSize = 0x88, O_ArrayDim = 0x68, O_ElemSize = 0x6c, O_Offset = 0x90, O_BoolMask = 0xb0,
       O_InnerPtr = 0xb0,  // UObjectProperty::PropertyClass / UStructProperty::Struct / UArrayProperty::Inner
       O_FuncFlags = 0xd8, O_iNative = 0xdc,  // UFunction (from UObject::ProcessEvent)
       VT_ProcessEvent = 67, VT_Exec = 77 };

inline void* P(void* o, int off) { return *(void**)((u8*)o + off); }
inline TArray<u8*>& Names() { return *(TArray<u8*>*)A_GNames; }
inline TArray<void*>& Objects() { return *(TArray<void*>*)A_GObjects; }

inline std::string name_str(FName n) {
  auto& N = Names();
  if (n.Index < 0 || n.Index >= N.Num || !N.Data[n.Index]) return "?";
  u8* e = N.Data[n.Index];
  std::string s;
  if (*(int*)(e + 8) & 1) for (wchar_t* w = (wchar_t*)(e + 0x18); *w; ++w) s += (char)*w;
  else s = (char*)(e + 0x18);
  if (n.Number) s += "_" + std::to_string(n.Number - 1);
  return s;
}
inline FName find_name(const char* s) {  // existing names only
  static std::unordered_map<std::string, int> cache;
  auto it = cache.find(s);
  if (it != cache.end()) return {it->second, 0};
  auto& N = Names();
  for (int i = 0; i < N.Num; ++i) {
    u8* e = N.Data[i];
    if (e && !(*(int*)(e + 8) & 1) && strcmp((char*)(e + 0x18), s) == 0) { cache[s] = i; return {i, 0}; }
  }
  return {-1, 0};
}
inline bool name_is(void* o, FName n) { return o && ((FName*)((u8*)o + O_Name))->Index == n.Index; }
inline std::string obj_name(void* o) { return o ? name_str(*(FName*)((u8*)o + O_Name)) : "None"; }
inline void* obj_class(void* o) { return P(o, O_Class); }
inline std::string obj_path(void* o) {
  std::string s = obj_name(o);
  for (void* p = P(o, O_Outer); p; p = P(p, O_Outer)) s = obj_name(p) + "." + s;
  return s;
}
inline std::string full_name(void* o) { return obj_name(obj_class(o)) + " " + obj_path(o); }

// class objects by short name ("OLHero"), cached
inline void* find_class(const char* name) {
  static std::unordered_map<std::string, void*> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  FName cls = find_name("Class"), n = find_name(name);
  auto& O = Objects();
  for (int i = 0; i < O.Num; ++i)
    if (O.Data[i] && name_is(O.Data[i], n) && name_is(obj_class(O.Data[i]), cls)) return cache[name] = O.Data[i];
  return nullptr;
}
inline bool is_a(void* o, void* cls) {
  if (!o || !cls) return false;
  for (void* c = obj_class(o); c; c = P(c, O_Super)) if (c == cls) return true;
  return false;
}
inline bool is_default(void* o) { return obj_name(o).rfind("Default__", 0) == 0; }
// first live (non-template) instance of a class
inline void* find_instance(const char* cls_name) {
  void* cls = find_class(cls_name);
  auto& O = Objects();
  for (int i = 0; i < O.Num; ++i)
    if (O.Data[i] && is_a(O.Data[i], cls) && !is_default(O.Data[i])) return O.Data[i];
  return nullptr;
}

// field (property or function) on a struct/class chain, by name; cached per (struct, name)
inline void* find_field(void* strct, const char* name) {
  static std::unordered_map<std::string, void*> cache;
  std::string key = std::to_string((uintptr_t)strct) + name;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  FName n = find_name(name);
  for (void* s = strct; s; s = P(s, O_Super))
    for (void* c = P(s, O_Children); c; c = P(c, O_Next))
      if (name_is(c, n)) return cache[key] = c;
  mlog("!! field %s not found on %s", name, obj_name(strct).c_str());
  return cache[key] = nullptr;
}
inline int offset_of(void* prop) { return *(int*)((u8*)prop + O_Offset); }

// typed property access on an object: get<FVector>(pawn, "Location")
template <class T> inline T* field_ptr(void* o, const char* name) {
  void* p = find_field(obj_class(o), name);
  return p ? (T*)((u8*)o + offset_of(p)) : nullptr;
}
template <class T> inline T get(void* o, const char* name) {
  T* p = field_ptr<T>(o, name);
  return p ? *p : T{};
}
template <class T> inline void set(void* o, const char* name, const T& v) {
  if (T* p = field_ptr<T>(o, name)) *p = v;
}
inline bool get_bool(void* o, const char* name) {
  void* p = find_field(obj_class(o), name);
  return p && (*(unsigned*)((u8*)o + offset_of(p)) & *(unsigned*)((u8*)p + O_BoolMask));
}
inline void set_bool(void* o, const char* name, bool v) {
  void* p = find_field(obj_class(o), name);
  if (!p) return;
  unsigned* w = (unsigned*)((u8*)o + offset_of(p));
  unsigned m = *(unsigned*)((u8*)p + O_BoolMask);
  *w = v ? (*w | m) : (*w & ~m);
}

// Script/native function call through ProcessEvent with a parameter buffer filled by name.
struct Call {
  void* obj;
  void* fn;
  std::vector<u8> buf;
  Call(void* o, const char* name) : obj(o), fn(o ? find_field(obj_class(o), name) : nullptr) {
    if (fn) buf.assign(*(int*)((u8*)fn + O_PropsSize) + 16, 0);
  }
  template <class T> Call& arg(const char* name, const T& v) {
    if (fn) if (void* p = find_field(fn, name)) memcpy(&buf[offset_of(p)], &v, sizeof(T));
    return *this;
  }
  Call& arg_bool(const char* name, bool v) {
    if (fn) if (void* p = find_field(fn, name)) {
      unsigned* w = (unsigned*)&buf[offset_of(p)];
      unsigned m = *(unsigned*)((u8*)p + O_BoolMask);
      *w = v ? (*w | m) : (*w & ~m);
    }
    return *this;
  }
  Call& go() {
    if (!fn) return *this;
    auto pe = (void (*)(void*, void*, void*, void*))(*(void***)obj)[VT_ProcessEvent];
    // ProcessEvent returns early for functions with a native index (Trace, Spawn, SetLocation...): clear it for the call
    unsigned short* inative = (unsigned short*)((u8*)fn + O_iNative);
    unsigned short saved = *inative;
    *inative = 0;
    pe(obj, fn, buf.data(), nullptr);
    *inative = saved;
    return *this;
  }
  template <class T> T ret(const char* name = "ReturnValue") {
    T v{};
    if (fn) if (void* p = find_field(fn, name)) memcpy(&v, &buf[offset_of(p)], sizeof(T));
    return v;
  }
  bool ret_bool(const char* name = "ReturnValue") {
    void* p = fn ? find_field(fn, name) : nullptr;
    return p && (*(unsigned*)&buf[offset_of(p)] & *(unsigned*)((u8*)p + O_BoolMask));
  }
};

// ---- math ----
inline FVector operator+(FVector a, FVector b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
inline FVector operator-(FVector a, FVector b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
inline FVector operator*(FVector a, float s) { return {a.X * s, a.Y * s, a.Z * s}; }
inline float dot(FVector a, FVector b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline FVector cross(FVector a, FVector b) { return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X}; }
inline float len(FVector a) { return std::sqrt(dot(a, a)); }
inline FVector norm(FVector a) { float l = len(a); return l > 1e-6f ? a * (1.f / l) : FVector{0, 0, 0}; }
const float U2R = 3.14159265f / 32768.f;  // unreal rotation units -> radians
inline FVector rot_dir(FRotator r) {
  float p = r.Pitch * U2R, y = r.Yaw * U2R;
  return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}
inline FRotator dir_rot(FVector d) {
  return {(int)(std::atan2(d.Z, std::sqrt(d.X * d.X + d.Y * d.Y)) / U2R), (int)(std::atan2(d.Y, d.X) / U2R), 0};
}
}  // namespace ue
