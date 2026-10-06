// Outlast portal gun: the reloadable mod. Loaded by loader.cpp (LD_PRELOAD) into OLGame.x86_64.
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "host.h"
#include "ue.h"
using namespace ue;
#include "mesh.h"
#include "sound.h"

static Host* H;
static FILE* g_log;
static std::string run_dir() { return std::string(H->root) + "/run"; }
void mlog(const char* fmt, ...) {
  if (!g_log) return;
  va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
  fputc('\n', g_log); fflush(g_log);
}
static inline int I(void* o, int off) { return *(int*)((u8*)o + off); }

// ---------------------------------------------------------------- engine helpers
struct OutDev { void** vt; int pad[4]; };
static void od_nop(void*) {}
static void od_serialize(void*, const wchar_t* s, int) {
  std::string a; for (; s && *s; ++s) a += (char)*s;
  mlog("  | %s", a.c_str());
}
static void* od_vt[] = {(void*)od_nop, (void*)od_nop, (void*)od_serialize, (void*)od_nop, (void*)od_nop};
static OutDev g_out = {od_vt, {0, 0, 1, 0}};

static void engine_exec(const std::string& cmd) {
  void* eng = *(void**)A_GEngine;
  std::wstring w(cmd.begin(), cmd.end());
  auto fn = (int (*)(void*, const wchar_t*, OutDev*))(*(void***)eng)[VT_Exec];
  mlog("exec '%s' -> %d", cmd.c_str(), fn(eng, w.c_str(), &g_out));
}

typedef void* (*SCOFn)(void*, void*, FName, unsigned long, void*, void*, void*, void*);
static void* construct(const char* cls, void* outer = nullptr) {
  void* c = find_class(cls);
  if (!c) return nullptr;
  if (!outer) outer = ((void* (*)())A_GetTransientPackage)();
  return ((SCOFn)A_StaticConstructObject)(c, outer, FName{0, 0}, 0, nullptr, &g_out, nullptr, nullptr);
}
static void add_to_root(void* o) { if (o) ((void (*)(void*))A_AddToRoot)(o); }
static void* find_named(const char* cls, const char* name) {  // object by class + object name
  FName c = find_name(cls), n = find_name(name);
  auto& O = Objects();
  for (int i = 0; i < O.Num; ++i)
    if (O.Data[i] && name_is(O.Data[i], n) && name_is(obj_class(O.Data[i]), c)) return O.Data[i];
  return nullptr;
}
void* cdo(const char* cls) { return find_named(cls, ("Default__" + std::string(cls)).c_str()); }

// ---------------------------------------------------------------- player access
static void* local_pc() {
  void* eng = *(void**)A_GEngine;
  auto gp = get<TArray<void*>>(eng, "GamePlayers");
  return gp.Num > 0 && gp.Data[0] ? get<void*>(gp.Data[0], "Actor") : nullptr;
}
static void* local_pawn() { void* pc = local_pc(); return pc ? get<void*>(pc, "Pawn") : nullptr; }

static void view_point(FVector& loc, FRotator& rot) {
  void* pc = local_pc();
  Call c(pc, "GetPlayerViewPoint");
  c.go();
  bool ol = find_field(c.fn, "POVLocation") != nullptr;
  loc = c.ret<FVector>(ol ? "POVLocation" : "out_Location");
  rot = c.ret<FRotator>(ol ? "POVRotation" : "out_Rotation");
}

struct Hit { void* actor; FVector loc, n; bool ok; void *mat, *phys; };
struct TraceHitInfo { void *mat, *phys; int item, level; FName bone; void* comp; };
static Hit trace(void* from_actor, FVector start, FVector end, bool actors = false) {
  Call c(from_actor, "Trace");
  c.arg("TraceEnd", end).arg("TraceStart", start).arg_bool("bTraceActors", actors).arg("Extent", FVector{0, 0, 0});
  c.arg("ExtraTraceFlags", 0).go();
  Hit h;
  h.actor = c.ret<void*>();
  h.loc = c.ret<FVector>("HitLocation");
  h.n = c.ret<FVector>("HitNormal");
  h.ok = h.actor != nullptr;
  TraceHitInfo hi = c.ret<TraceHitInfo>("HitInfo");
  h.mat = hi.mat;
  h.phys = hi.phys;
  return h;
}

// Turn the player's view. Outlast keeps the real view in OLHeroCamera (CamView structs: Loc, Yaw, Pitch in degrees,
// world space ViewWS and pawn-relative ViewCS); the controller's Rotation is recomputed from it every frame.
static void set_view(void* pawn, FRotator r) {
  void* ctrl = get<void*>(pawn, "Controller");
  if (ctrl) set<FRotator>(ctrl, "Rotation", r);
  Call(pawn, "SetRotation").arg("NewRotation", FRotator{0, r.Yaw, 0}).go();
  void* cam = is_a(pawn, find_class("OLHero")) ? get<void*>(pawn, "Camera") : nullptr;
  if (!cam) return;
  float yaw = r.Yaw * 360.f / 65536, pitch = (short)r.Pitch * 360.f / 65536;
  float* ws = field_ptr<float>(cam, "ViewWS");
  float* cs = field_ptr<float>(cam, "ViewCS");
  if (ws) { ws[3] = yaw; ws[4] = pitch; }
  if (cs) { cs[3] = 0; cs[4] = pitch; }
  set<FRotator>(cam, "BaseRotation", FRotator{0, r.Yaw, 0});
}

// ---------------------------------------------------------------- portal resources (created once, rooted)
struct FLinearColor { float R, G, B, A; };
struct Portal {
  bool open = false;
  FVector loc{}, n{}, right{}, up{};
  void* surf = nullptr;   // DynamicSMActor_Spawnable showing the portal surface
  void* frame = nullptr;  // slightly larger rim behind it, in the portal's colour
};
static Portal g_p[2];
static void* g_world;
static void *g_cube, *g_emissive, *g_rim_mic[2], *g_surf_mic[2];
static void *g_view_rt[2], *g_view_mic[2];  // what portal i shows: the world beyond the other portal
static void* g_cap[2];                       // SceneCapture2DComponent rendering g_view_rt[i]
static bool g_showing_view[2];
static int g_flipx = 1, g_flipy = 0, g_capmode = 0;  // tuned in game: image orientation, how the probe is refreshed
static FName g_tex_param;
static FVector g_cube_ext{0, 0, 0};
static void* g_disc;            // EngineMeshes.Sphere rebuilt as a flat disc (radius 160, facing +X, planar UVs)
static const float DISC_R = 160;  // half size of EngineMeshes.Cube, measured from the first spawned component
static const float PW = 145, PH = 255;  // Portal 2's 64x112 portal scaled from Chell (72 tall) to Miles (165 tall)
static const FLinearColor COL[2] = {{0.05f, 0.45f, 1.0f, 1}, {1.0f, 0.45f, 0.03f, 1}};

static void* make_rt(int w, int h, FLinearColor c) {
  void* rt = Call(cdo("TextureRenderTarget2D"), "Create")
                 .arg("InSizeX", w).arg("InSizeY", h).arg<u8>("InFormat", 2 /*PF_A8R8G8B8*/)
                 .arg("InClearColor", c).arg_bool("bOnlyRenderOnce", false).go().ret<void*>();
  add_to_root(rt);
  return rt;
}
static void* make_mic(void* parent, void* tex, FName param = {-2, 0}) {
  void* mic = construct("MaterialInstanceConstant");
  if (!mic) return nullptr;
  add_to_root(mic);
  Call(mic, "SetParent").arg("NewParent", parent).go();
  Call(mic, "SetTextureParameterValue").arg("ParameterName", param.Index == -2 ? g_tex_param : param).arg("Value", tex).go();
  return mic;
}

static bool init_resources() {
  if (g_emissive) return true;
  g_cube = find_named("StaticMesh", "Cube");
  g_emissive = find_named("Material", "EmissiveTexturedMaterial");
  if (!g_cube || !g_emissive) { mlog("!! engine cube/material missing"); return false; }
  // the material's texture parameter name, from its expression subobject
  FName ex = find_name("MaterialExpressionTextureSampleParameter2D");
  auto& O = Objects();
  for (int i = 0; i < O.Num; ++i)
    if (O.Data[i] && name_is(obj_class(O.Data[i]), ex) && P(O.Data[i], O_Outer) == g_emissive)
      g_tex_param = get<FName>(O.Data[i], "ParameterName");
  if (void* sphere = find_named("StaticMesh", "Sphere")) {
    std::vector<BuildVert> v;
    std::vector<unsigned short> idx;
    const int N = 72;
    FVector nrm{1, 0, 0}, tan{0, 1, 0};
    v.push_back(make_vert({0, 0, 0}, nrm, tan, 0.5f, 0.5f));
    for (int k = 0; k < N; ++k) {
      float a = k * 2 * 3.14159265f / N, y = std::cos(a), z = std::sin(a);
      v.push_back(make_vert({0, y * DISC_R, z * DISC_R}, nrm, tan, 0.5f + 0.5f * y, 0.5f - 0.5f * z));
    }
    for (int k = 0; k < N; ++k) {  // both windings: visible from either side
      unsigned short a = 1 + k, b = 1 + (k + 1) % N;
      idx.insert(idx.end(), {0, a, b, 0, b, a});
    }
    if (rebuild_mesh(sphere, v, idx)) g_disc = sphere;
    mlog("disc mesh %s", g_disc ? "built" : "FAILED, using cube");
  }
  for (int i = 0; i < 2; ++i) {
    // rim: glowing ring with ragged brightness; closed surface: a slow swirl in the portal colour
    std::vector<unsigned> rim(256 * 256), swirl(256 * 256);
    for (int y = 0; y < 256; ++y)
      for (int x = 0; x < 256; ++x) {
        float dx = (x + 0.5f) / 128 - 1, dy = (y + 0.5f) / 128 - 1, rr = std::sqrt(dx * dx + dy * dy), ang = std::atan2(dy, dx);
        float noise = 0.55f + 0.25f * std::sin(ang * 7 + 1.3f) + 0.2f * std::sin(ang * 19 + rr * 30);
        float ring = std::exp(-std::pow((rr - 0.93f) / 0.05f, 2)) * (0.7f + 0.6f * noise);
        float core = std::exp(-std::pow((rr - 0.9f) / 0.012f, 2)) * 0.9f;  // bright inner edge line
        float rv = std::fmin(1.f, COL[i].R * ring + core), gv = std::fmin(1.f, COL[i].G * ring + core * 0.95f),
              bv = std::fmin(1.f, COL[i].B * ring + core);
        rim[y * 256 + x] = 0xff000000u | (unsigned)(rv * 255) << 16 | (unsigned)(gv * 255) << 8 | (unsigned)(bv * 255);
        float sw = 0.18f + 0.12f * std::sin(ang * 3 + rr * 14) + 0.25f * rr * rr;
        swirl[y * 256 + x] = 0xff000000u | (unsigned)(COL[i].R * sw * 255) << 16 | (unsigned)(COL[i].G * sw * 255) << 8 |
                             (unsigned)(COL[i].B * sw * 255);
      }
    void* rim_tex = g_disc ? make_texture(256, 256, rim.data()) : nullptr;
    void* sw_tex = g_disc ? make_texture(256, 256, swirl.data()) : nullptr;
    g_rim_mic[i] = make_mic(g_emissive, rim_tex ? rim_tex : make_rt(4, 4, COL[i]));
    FLinearColor dim = {COL[i].R * 0.25f, COL[i].G * 0.25f, COL[i].B * 0.25f, 1};
    g_surf_mic[i] = make_mic(g_emissive, sw_tex ? sw_tex : make_rt(4, 4, dim));
    g_view_rt[i] = make_rt(512, 854, {0, 0, 0, 1});
    g_view_mic[i] = make_mic(g_emissive, g_view_rt[i]);
  }
  mlog("resources: texture param '%s'", name_str(g_tex_param).c_str());
  return true;
}

// rotator whose X/Y/Z axes are fwd/right/up (FMatrix::Rotator)
static FRotator basis_rot(FVector X, FVector Y, FVector Z) {
  float pitch = std::atan2(X.Z, std::sqrt(X.X * X.X + X.Y * X.Y)), yaw = std::atan2(X.Y, X.X);
  FVector SY = {-std::sin(yaw), std::cos(yaw), 0};
  float roll = std::atan2(dot(Z, SY), dot(Y, SY));
  return {(int)std::lround(pitch / U2R), (int)std::lround(yaw / U2R), (int)std::lround(roll / U2R)};
}

static void* spawn_mesh(void* mic) {
  void* a = Call(local_pawn(), "Spawn").arg("SpawnClass", find_class("DynamicSMActor_Spawnable"))
                .arg_bool("bNoCollisionFail", true).go().ret<void*>();
  if (!a) { mlog("!! spawn failed"); return nullptr; }
  Call(a, "SetStaticMesh").arg("NewMesh", g_disc ? g_disc : g_cube).arg("NewScale3D", FVector{1, 1, 1}).go();
  Call(a, "SetCollision").arg_bool("bNewColActors", false).arg_bool("bNewBlockActors", false).go();
  if (void* smc = get<void*>(a, "StaticMeshComponent")) {
    if (g_disc) g_cube_ext = {1, DISC_R, DISC_R};  // disc: flat, radius DISC_R
    else if (g_cube_ext.X <= 0) {  // spawned unrotated at scale 1: component bounds = mesh bounds
      float* b = field_ptr<float>(smc, "Bounds");  // FBoxSphereBounds: Origin, BoxExtent, SphereRadius
      g_cube_ext = {b[3], b[4], b[5]};
      mlog("cube extent %.2f %.2f %.2f", b[3], b[4], b[5]);
    }
    Call(smc, "SetMaterial").arg("ElementIndex", 0).arg("Material", mic).go();
    set_bool(smc, "CastShadow", false);
  }
  return a;
}
static void place_mesh(void* a, FVector loc, FRotator rot, FVector size) {
  Call(a, "SetLocation").arg("NewLocation", loc).go();
  Call(a, "SetRotation").arg("NewRotation", rot).go();
  Call(a, "SetDrawScale3D").arg("NewScale3D", FVector{size.X / (2 * g_cube_ext.X), size.Y / (2 * g_cube_ext.Y),
                                                      size.Z / (2 * g_cube_ext.Z)}).go();
  Call(a, "SetHidden").arg_bool("bNewHidden", false).go();
}

static void forget_portals() {
  for (auto& p : g_p) p = Portal();
  for (int i = 0; i < 2; ++i) { g_cap[i] = nullptr; g_showing_view[i] = false; }
}
static void destroy_portals() {
  for (auto& p : g_p) {
    if (p.surf) Call(p.surf, "Destroy").go();
    if (p.frame) Call(p.frame, "Destroy").go();
    p = Portal();
  }
  for (int i = 0; i < 2; ++i) { g_cap[i] = nullptr; g_showing_view[i] = false; }
}

static FVector through(const Portal& A, const Portal& B, FVector v) {
  return B.n * (-dot(v, A.n)) + B.right * (-dot(v, A.right)) + B.up * dot(v, A.up);
}

// ---------------------------------------------------------------- see-through view
// Portal i's surface shows a capture taken from the player's eye moved through portal i to the other portal B,
// looking straight out of B with an off-axis frustum whose window is B's rectangle and whose near plane is B's
// plane. The captured image then maps 1:1 onto the portal rectangle, and the wall behind B is clipped away.
static void* make_capture(void* owner, void* rt) {
  void* c = construct("SceneCapture2DComponent", owner);
  if (!c) return nullptr;
  set<void*>(c, "TextureTarget", rt);
  set<float>(c, "FieldOfView", 90);
  set<float>(c, "NearPlane", 10);
  set<float>(c, "FarPlane", 0);
  set<float>(c, "FrameRate", 1000);
  set_bool(c, "bUpdateMatrices", true);
  set_bool(c, "bSkipUpdateIfOwnerOccluded", false);
  set_bool(c, "bSkipUpdateIfTextureUsersOccluded", false);
  set_bool(c, "bEnabled", true);
  Call(owner, "AttachComponent").arg("NewComponent", c).go();
  float* m = field_ptr<float>(c, "ProjMatrix");
  mlog("engine proj (fov 90 near 10): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", m[0], m[1], m[2], m[3], m[4],
       m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
  set_bool(c, "bUpdateMatrices", false);
  return c;
}

static void update_view(int i, FVector eye) {
  const Portal &A = g_p[i], &B = g_p[1 - i];
  void* c = g_cap[i];
  if (!c) return;
  bool facing = dot(eye - A.loc, A.n) > 1;
  Call(c, "SetEnabled").arg_bool("bEnable", facing).go();
  if (!facing) return;
  FVector E = B.loc + through(A, B, eye - A.loc);
  FVector F = B.n, U = B.up, R = B.right;
  float n = dot(B.loc - E, F);
  if (n < 1) n = 1;
  float cx = dot(B.loc - E, R), cy = dot(B.loc - E, U);
  float l = cx - PW * 0.5f, r = cx + PW * 0.5f, b = cy - PH * 0.5f, t = cy + PH * 0.5f;
  if (g_flipx) std::swap(l, r);
  if (g_flipy) std::swap(b, t);
  float* V = field_ptr<float>(c, "ViewMatrix");
  float* Pm = field_ptr<float>(c, "ProjMatrix");
  float v[16] = {R.X, U.X, F.X, 0, R.Y, U.Y, F.Y, 0, R.Z, U.Z, F.Z, 0, -dot(E, R), -dot(E, U), -dot(E, F), 1};
  float near = n + 2.f;  // just past the exit portal's own meshes (they sit 0.6-1.4 units off the wall)
  float pr[16] = {2 * n / (r - l), 0, 0, 0,
                  0, 2 * n / (t - b), 0, 0,
                  -(r + l) / (r - l), -(t + b) / (t - b), 1, 1,
                  0, 0, -near, 0};
  memcpy(V, v, sizeof v);
  memcpy(Pm, pr, sizeof pr);
  if (g_capmode == 0) Call(c, "SetView").arg("NewLocation", E).arg("NewRotation", dir_rot(F)).go();
  else {
    Call(A.surf, "DetachComponent").arg("ExComponent", c).go();
    Call(A.surf, "AttachComponent").arg("NewComponent", c).go();
  }
}

static void update_views() {
  FVector eye; FRotator rot;
  view_point(eye, rot);
  for (int i = 0; i < 2; ++i) {
    Portal& A = g_p[i];
    bool both = g_p[0].open && g_p[1].open;
    if (both && !g_cap[i] && A.surf) g_cap[i] = make_capture(A.surf, g_view_rt[i]);
    if (both != g_showing_view[i] && A.surf) {
      if (void* smc = get<void*>(A.surf, "StaticMeshComponent"))
        Call(smc, "SetMaterial").arg("ElementIndex", 0).arg("Material", both ? g_view_mic[i] : g_surf_mic[i]).go();
      g_showing_view[i] = both;
    }
    if (both) update_view(i, eye);
  }
}

// ---------------------------------------------------------------- firing
static bool surface_ok(void* pawn, FVector c, FVector n, FVector right, FVector up, FVector& out) {
  // every corner of the portal must sit on the same flat surface; nudge inwards up to a few times
  for (int it = 0; it < 24; ++it) {
    FVector shift{0, 0, 0};
    bool bad = false;
    for (int sr = -1; sr <= 1; sr += 2)
      for (int su = -1; su <= 1; su += 2) {
        FVector p = c + right * (sr * PW * 0.5f) + up * (su * PH * 0.5f);
        Hit h = trace(pawn, p + n * 14, p - n * 14);
        if (!h.ok || std::fabs(dot(h.loc - c, n)) > 10 || dot(h.n, n) < 0.8f) {  // brick, trim, uneven floors are fine
          bad = true;
          shift = shift - right * (sr * 8.f) - up * (su * 8.f);
        }
      }
    if (!bad) { out = c; return true; }
    if (len(shift) < 1) return false;
    c = c + shift;
  }
  return false;
}

// Something the shot should pass through: see-through materials by name, or anything thin (bars, panes, signs)
static bool see_through(void* pawn, const Hit& h, FVector dir) {
  std::string m = h.mat ? obj_name(h.mat) : "", ph = h.phys ? obj_name(h.phys) : "";
  std::string a = m + " " + ph + " " + (h.actor ? obj_name(obj_class(h.actor)) : "");
  for (auto& ch : a) ch = (char)tolower(ch);
  static const char* keys[] = {"glass", "window", "gate", "fence", "grate", "grill", "bars", "chain", "wire", "cage",
                               "railing", "net", "jail", "mesh_metal", "vitre", "door_bar"};
  for (const char* k : keys)
    if (a.find(k) != std::string::npos) { mlog("  shot passes through %s", a.c_str()); return true; }
  // thin: tracing back from a little past the hit finds the far side within a few units
  Hit back = trace(pawn, h.loc + dir * 14, h.loc + dir * 0.5f);
  if (back.ok && len(back.loc - h.loc) < 12) { mlog("  shot passes through thin %s", a.c_str()); return true; }
  mlog("  shot hit %s", a.c_str());
  return false;
}

static void fire(int i) {
  void* pawn = local_pawn();
  if (!pawn || !init_resources()) return;
  FVector eye; FRotator rot;
  view_point(eye, rot);
  FVector dir = rot_dir(rot);
  snd::play(i ? "fire1" : "fire0", 0.6f);
  Hit h;
  FVector from = eye;
  for (int k = 0; k < 8; ++k) {  // shots fly through gates, fences, grates and glass
    h = trace(pawn, from, eye + dir * 30000);
    if (!h.ok || !see_through(pawn, h, dir)) break;
    from = h.loc + dir * 4;
  }
  if (!h.ok) { snd::play("invalid", 0.5f); mlog("fire %d: nothing hit (eye %.0f %.0f %.0f dir %.2f %.2f %.2f)", i, eye.X, eye.Y, eye.Z, dir.X, dir.Y, dir.Z); return; }
  FVector n = norm(h.n), up;
  if (std::fabs(n.Z) < 0.7f) up = norm(FVector{0, 0, 1} - n * n.Z);
  else up = norm(dir - n * dot(dir, n));
  FVector right = cross(up, n), c, at = h.loc;
  if (std::fabs(n.Z) < 0.7f) {  // wall: if the portal's bottom ends up a little above a floor, sit it on the floor
    FVector bottom = at - FVector{0, 0, PH * 0.5f} + n * 20;
    Hit f = trace(pawn, bottom + FVector{0, 0, 40}, bottom - FVector{0, 0, 120});
    if (f.ok && f.n.Z > 0.7f) {
      float gap = bottom.Z - f.loc.Z;
      if (gap > -40 && gap < 110) at.Z -= gap - 1;
    }
  }
  if (!surface_ok(pawn, at, n, right, up, c)) {
    snd::play("invalid", 0.5f); mlog("fire %d: surface too small at %.0f %.0f %.0f", i, h.loc.X, h.loc.Y, h.loc.Z); return; }
  Portal& o = g_p[1 - i];
  if (o.open && dot(o.n, n) > 0.9f && std::fabs(dot(c - o.loc, n)) < 10 && std::fabs(dot(c - o.loc, right)) < PW * 0.95f &&
      std::fabs(dot(c - o.loc, up)) < PH * 0.95f) {
    snd::play("invalid", 0.5f); mlog("fire %d: overlaps other portal", i); return; }
  Portal& p = g_p[i];
  if (!p.surf) p.surf = spawn_mesh(g_surf_mic[i]);
  if (!p.frame) p.frame = spawn_mesh(g_rim_mic[i]);
  if (!p.surf || !p.frame) return;
  FRotator r = basis_rot(n, right, up);
  place_mesh(p.surf, c + n * 1.2f, r, {0.4f, PW, PH});
  place_mesh(p.frame, c + n * 0.6f, r, {0.4f, PW + 20, PH + 26});
  p.open = true; p.loc = c; p.n = n; p.right = right; p.up = up;
  snd::play(i ? "open1" : "open0", 0.55f);
  mlog("portal %d at %.0f %.0f %.0f n %.2f %.2f %.2f", i, c.X, c.Y, c.Z, n.X, n.Y, n.Z);
}

// ---------------------------------------------------------------- teleport
static std::map<void*, float> g_cooldown;
static float extent_along(void* pawn, FVector n) {
  void* cyl = get<void*>(pawn, "CylinderComponent");
  float r = cyl ? get<float>(cyl, "CollisionRadius") : 30, h = cyl ? get<float>(cyl, "CollisionHeight") : 80;
  return std::fabs(n.Z) > 0.7f ? h : r;
}

static void try_teleport(void* pawn, float now) {
  auto cd = g_cooldown.find(pawn);
  if (cd != g_cooldown.end() && now < cd->second) return;
  FVector loc = get<FVector>(pawn, "Location"), vel = get<FVector>(pawn, "Velocity"), acc = get<FVector>(pawn, "Acceleration");
  for (int i = 0; i < 2; ++i) {
    const Portal &A = g_p[i], &B = g_p[1 - i];
    FVector rel = loc - A.loc;
    float d = dot(rel, A.n), r = dot(rel, A.right), u = dot(rel, A.up), e = extent_along(pawn, A.n);
    static float dbg_t = 0;
    if (pawn == local_pawn() && d < e + 60 && std::fabs(r) < PW && now > dbg_t) {
      dbg_t = now + 0.5f;
      mlog("near %d: d %.0f r %.0f u %.0f e %.0f vel.n %.0f acc.n %.0f", i, d, r, u, e, dot(vel, A.n), dot(acc, A.n));
    }
    bool floor = A.n.Z > 0.7f;
    float er = r / (PW * 0.5f), eu = u / (PH * 0.5f);
    // floor/ceiling: centre over the oval; wall: the body overlaps the opening (Outlast floors sit high on the capsule)
    float hh = PH * 0.5f, h = extent_along(pawn, FVector{0, 0, 1});
    bool inside = std::fabs(A.n.Z) > 0.7f ? er * er + eu * eu < 1
                                          : std::fabs(r) < PW * 0.5f - 12 && u > -(hh + h * 0.65f) && u < hh - h * 0.3f;
    if (!inside || d < -20 || d > e + 10) continue;
    bool toward = dot(vel, A.n) < -5 || dot(acc, A.n) < -5;
    if (!floor && !toward) continue;
    // through the portal: mirror the lateral offset, keep momentum, turn the view
    float eB = extent_along(pawn, B.n);
    float rB = std::fmax(-PW * 0.25f, std::fmin(PW * 0.25f, -r)), uB = std::fmax(-PH * 0.2f, std::fmin(PH * 0.2f, u));
    if (std::fabs(B.n.Z) < 0.7f) uB = std::fmax(uB, -(PH * 0.5f - eB) + 2);  // feet stay inside the portal
    FVector out = B.loc + B.right * rB + B.up * uB;
    FVector nv = through(A, B, vel);
    float s = dot(nv, B.n);
    if (s < 200) nv = nv + B.n * (200 - s);
    bool placed = false;
    for (float k = 0; k < 120 && !placed; k += 15) {
      FVector p = out + B.n * (eB + 8 + k);
      if (std::fabs(B.n.Z) < 0.7f && k > 45) p = p + FVector{0, 0, k - 45};
      placed = Call(pawn, "SetLocation").arg("NewLocation", p).go().ret_bool();
    }
    if (!placed) { mlog("teleport blocked at exit"); g_cooldown[pawn] = now + 0.5f; return; }
    set<FVector>(pawn, "Velocity", nv);
    Call(pawn, "SetPhysics").arg<u8>("NewPhysics", 2 /*PHYS_Falling*/).go();
    void* ctrl = get<void*>(pawn, "Controller");
    FRotator cr = ctrl ? get<FRotator>(ctrl, "Rotation") : get<FRotator>(pawn, "Rotation");
    set_view(pawn, dir_rot(through(A, B, rot_dir(cr))));
    if (is_a(pawn, find_class("OLHero"))) Call(pawn, "ResetAfterTeleport").go();
    g_cooldown[pawn] = now + 0.3f;
    if (pawn == local_pawn()) snd::play("enter", 0.6f);
    else if (void* me = local_pawn()) {
      float dist = len(get<FVector>(me, "Location") - loc);
      if (dist < 2500) snd::play("enter", 0.6f * (1 - dist / 2500));
    }
    mlog("teleport %s via %d: vel %.0f -> %.0f", obj_name(pawn).c_str(), i, len(vel), len(nv));
    return;
  }
}

// ---------------------------------------------------------------- the gun (Portal 2's v_portalgun, in engine)
// tools/p2gun.py converts the model from the player's own Portal 2 install into cache/gun.bin; here it replaces
// EngineMeshes.Cube's geometry, gets a lit material (the camcorder's), and is attached to Miles's camera bone.
static void *g_gun_mesh, *g_gun_mic, *g_gun_comp, *g_gun_owner;
static bool g_gun_failed, g_gun_shown = true;
static FVector g_gun_off{33, 11, -10};  // forward, right, up from the eye
static FRotator g_gun_rot{-364, 728, 16384};  // -2 / 4 / 90 degrees: upright, aimed at the crosshair
static void* g_gun_tex;
static float g_gun_scale = 1.8f;  // Source units -> Outlast, sized by eye to sit like Portal 2's viewmodel

static bool load_gun() {
  if (g_gun_mesh) return true;
  if (g_gun_failed) return false;
  g_gun_failed = true;
  FILE* f = fopen((std::string(H->root) + "/cache/gun.bin").c_str(), "rb");
  if (!f) { mlog("no cache/gun.bin: run tools/p2gun.py"); return false; }
  char tag[4];
  int nv = 0, ni = 0, aw = 0, ah = 0;
  bool ok = fread(tag, 1, 4, f) == 4 && fread(&nv, 4, 1, f) && fread(&ni, 4, 1, f) && fread(&aw, 4, 1, f) && fread(&ah, 4, 1, f);
  std::vector<float> raw((size_t)nv * 8);
  std::vector<unsigned short> idx(ni);
  std::vector<unsigned> atlas((size_t)aw * ah);
  ok = ok && fread(raw.data(), 4, raw.size(), f) == raw.size() && fread(idx.data(), 2, idx.size(), f) == idx.size() &&
       fread(atlas.data(), 4, atlas.size(), f) == atlas.size();
  fclose(f);
  if (!ok || memcmp(tag, "OLPG", 4)) { mlog("bad gun.bin"); return false; }
  float lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
  for (int i = 0; i < nv; ++i)
    for (int k = 0; k < 3; ++k) lo[k] = std::fmin(lo[k], raw[i * 8 + k]), hi[k] = std::fmax(hi[k], raw[i * 8 + k]);
  std::vector<BuildVert> v;
  for (int i = 0; i < nv; ++i) {
    float* r = &raw[i * 8];
    float x = r[0] - (lo[0] + hi[0]) / 2, y = r[1] - (lo[1] + hi[1]) / 2, z = r[2] - (lo[2] + hi[2]) / 2;
    // the model points along +Z: turn it to point along +X (a proper rotation, winding unchanged)
    FVector p{z, y, -x}, n = norm(FVector{r[5], r[4], -r[3]});
    FVector t = norm(cross(n, std::fabs(n.Z) < 0.9f ? FVector{0, 0, 1} : FVector{1, 0, 0}));
    v.push_back(make_vert(p, n, t, r[6], r[7]));
  }
  void* cube = find_named("StaticMesh", "Cube");
  if (!cube || !rebuild_mesh(cube, v, idx)) { mlog("gun mesh rebuild failed"); return false; }
  void* tex = g_gun_tex = make_texture(aw, ah, atlas.data());
  void* parent = find_named("Material", "handycam_mat");
  if (parent) g_gun_mic = make_mic(parent, tex, find_name("Diffuse"));
  else g_gun_mic = make_mic(g_emissive, tex);
  g_gun_mesh = cube;
  g_gun_failed = false;
  mlog("gun: %d verts %d tris, atlas %dx%d, material %s", nv, ni / 3, aw, ah, parent ? "handycam_mat" : "emissive");
  return true;
}

static void rot_axes(FRotator r, FVector& X, FVector& Y, FVector& Z) {
  float p = r.Pitch * U2R, y = r.Yaw * U2R, ro = r.Roll * U2R;
  float SP = std::sin(p), CP = std::cos(p), SY = std::sin(y), CY = std::cos(y), SR = std::sin(ro), CR = std::cos(ro);
  X = {CP * CY, CP * SY, SP};
  Y = {SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP};
  Z = {-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP};
}
struct FQuat { float X, Y, Z, W; };
static FVector quat_rot(FQuat q, FVector v) {
  FVector u{q.X, q.Y, q.Z};
  return u * (2 * dot(u, v)) + v * (q.W * q.W - dot(u, u)) + cross(u, v) * (2 * q.W);
}

// Place the gun in camera space (g_gun_off: forward/right/up from the eye, g_gun_rot relative to the view),
// converted into the camera bone's frame it is attached to.
static void pose_gun() {
  void* pawn = local_pawn();
  void* mesh = pawn ? get<void*>(pawn, "Mesh") : nullptr;
  if (!g_gun_comp || !mesh) return;
  FName bone = find_name("Hero-Camera");
  FVector bl = Call(mesh, "GetBoneLocation").arg("BoneName", bone).go().ret<FVector>();
  FQuat bq = Call(mesh, "GetBoneQuaternion").arg("BoneName", bone).go().ret<FQuat>();
  FVector Bx = quat_rot(bq, {1, 0, 0}), By = quat_rot(bq, {0, 1, 0}), Bz = quat_rot(bq, {0, 0, 1});
  FVector eye; FRotator vr;
  view_point(eye, vr);
  FVector Cx, Cy, Cz, gx, gy, gz;
  rot_axes(vr, Cx, Cy, Cz);
  rot_axes(g_gun_rot, gx, gy, gz);
  auto world = [&](FVector a) { return Cx * a.X + Cy * a.Y + Cz * a.Z; };
  auto in_bone = [&](FVector w) { return FVector{dot(w, Bx), dot(w, By), dot(w, Bz)}; };
  FVector P = eye + world(g_gun_off);
  FVector rel = in_bone(P - bl);
  FRotator rr = basis_rot(in_bone(world(gx)), in_bone(world(gy)), in_bone(world(gz)));
  Call(g_gun_comp, "SetTranslation").arg("NewTranslation", rel).go();
  Call(g_gun_comp, "SetRotation").arg("NewRotation", rr).go();
  Call(g_gun_comp, "SetScale").arg("NewScale", g_gun_scale).go();
}

static bool gun_ready();
static void update_gun() {
  void* pawn = local_pawn();
  if (!pawn || !is_a(pawn, find_class("OLHero")) || !init_resources() || !load_gun()) return;
  if (g_gun_owner != pawn) { g_gun_owner = pawn; g_gun_comp = nullptr; }
  if (!g_gun_comp) {
    void* comp = construct("StaticMeshComponent", pawn);
    void* mesh = get<void*>(pawn, "Mesh");
    if (!comp || !mesh) return;
    Call(comp, "SetStaticMesh").arg("NewMesh", g_gun_mesh).go();
    Call(comp, "SetMaterial").arg("ElementIndex", 0).arg("Material", g_gun_mic).go();
    set_bool(comp, "CastShadow", false);
    Call(mesh, "AttachComponent").arg("Component", comp).arg("BoneName", find_name("Hero-Camera"))
        .arg("RelativeLocation", g_gun_off).arg("RelativeRotation", g_gun_rot)
        .arg("RelativeScale", FVector{1, 1, 1}).go();
    Call(comp, "SetDepthPriorityGroup").arg<u8>("NewDepthPriorityGroup", 2 /*SDPG_Foreground*/).go();
    // lit exactly like Miles's own body (a runtime component has no light environment of its own)
    if (void* le = get<void*>(pawn, "LightEnvironment")) Call(comp, "SetLightEnvironment").arg("NewLightEnvironment", le).go();
    g_gun_comp = comp;
    g_gun_shown = true;
    pose_gun();
    mlog("gun attached to %s", obj_name(pawn).c_str());
  }
  bool show = gun_ready();
  if (show != g_gun_shown) { Call(g_gun_comp, "SetHidden").arg_bool("NewHidden", !show).go(); g_gun_shown = show; }
}

// ---------------------------------------------------------------- gun state + input
static bool gun_ready() {
  void* pawn = local_pawn();
  if (!pawn || !is_a(pawn, find_class("OLHero"))) return false;
  void* pc = local_pc();
  if (get_bool(pc, "bCinematicMode")) return false;
  if (get<u8>(pawn, "CamcorderState") != 0) return false;  // camcorder raised or moving
  if (get<u8>(pawn, "SpecialMove") != 0) return false;     // climbing, vaulting, doors, hiding, grabbed...
  return true;
}

// anything Miles could use right in front of him (doors, pickups, beds, lockers): LMB uses instead of firing
static bool usable_in_front() {
  void* pawn = local_pawn();
  FVector eye; FRotator rot;
  view_point(eye, rot);
  FVector dir = rot_dir(rot);
  auto near = [&](void* a, float range) {
    if (!a) return false;
    FVector to = get<FVector>(a, "Location") - eye;
    float dist = len(to);
    return dist < range && dot(norm(to), dir) > 0.6f;
  };
  for (const char* list : {"CachedDoors", "CachedHidingSpots", "CachedBeds"}) {
    auto arr = get<TArray<void*>>(pawn, list);
    for (int i = 0; i < arr.Num; ++i) if (near(arr.Data[i], 190)) return true;
  }
  void* pick = find_class("OLPickableObject");
  auto& O = Objects();
  for (int i = 0; i < O.Num; ++i)
    if (O.Data[i] && is_a(O.Data[i], pick) && !is_default(O.Data[i]) && near(O.Data[i], 170)) return true;
  return false;
}

extern "C" unsigned mod_input(void* self, int ctrl, FName key, int ev, float amount, unsigned gamepad, InputKeyFn orig) {
  static FName L = find_name("LeftMouseButton"), R = find_name("RightMouseButton"), M = find_name("MiddleMouseButton");
  if (key.Index == M.Index) return orig(self, ctrl, R, ev, amount, gamepad);  // camcorder on middle mouse
  bool lmb = key.Index == L.Index, rmb = key.Index == R.Index;
  if ((lmb || rmb) && gun_ready()) {
    static bool lmb_used = false;  // a press that went to "use" keeps its release there too
    if (lmb && ev == 0 /*IE_Pressed*/) lmb_used = usable_in_front();
    if (lmb && lmb_used) return orig(self, ctrl, key, ev, amount, gamepad);
    if (ev == 0) fire(lmb ? 0 : 1);
    return 1;
  }
  return orig(self, ctrl, key, ev, amount, gamepad);
}

// ---------------------------------------------------------------- commands (run/cmd) and diagnostics
static void log_info() {
  void* pc = local_pc();
  void* pawn = local_pawn();
  mlog("pc=%s pawn=%s", pc ? full_name(pc).c_str() : "-", pawn ? full_name(pawn).c_str() : "-");
  if (!pawn) return;
  FVector l = get<FVector>(pawn, "Location"), v = get<FVector>(pawn, "Velocity");
  FRotator r = get<FRotator>(pc, "Rotation");
  mlog("  loc %.1f %.1f %.1f  vel %.1f %.1f %.1f  ctrlrot %d %d %d  phys %d  camstate %d  gun %d", l.X, l.Y, l.Z, v.X,
       v.Y, v.Z, r.Pitch, r.Yaw, r.Roll, get<u8>(pawn, "Physics"), get<u8>(pawn, "CamcorderState"), gun_ready());
}

static void dump_props(void* o) {
  mlog("%s", full_name(o).c_str());
  for (void* s = obj_class(o); s; s = P(s, O_Super))
    for (void* c = P(s, O_Children); c; c = P(c, O_Next)) {
      std::string t = obj_name(obj_class(c));
      if (t.size() < 9 || t.compare(t.size() - 8, 8, "Property")) continue;
      u8* v = (u8*)o + offset_of(c);
      std::string val;
      char b[160];
      if (t == "IntProperty") snprintf(b, sizeof b, "%d", *(int*)v), val = b;
      else if (t == "FloatProperty") snprintf(b, sizeof b, "%g", *(float*)v), val = b;
      else if (t == "ByteProperty") snprintf(b, sizeof b, "%d", *v), val = b;
      else if (t == "BoolProperty") val = (*(unsigned*)v & *(unsigned*)((u8*)c + O_BoolMask)) ? "true" : "false";
      else if (t == "NameProperty") val = name_str(*(FName*)v);
      else if (t == "ObjectProperty" || t == "ComponentProperty" || t == "ClassProperty")
        snprintf(b, sizeof b, "%p ", *(void**)v), val = b + (*(void**)v ? full_name(*(void**)v) : "None");
      else if (t == "StructProperty" && I(c, O_ElemSize) == 12)
        snprintf(b, sizeof b, "(%g %g %g | %d %d %d)", ((float*)v)[0], ((float*)v)[1], ((float*)v)[2], ((int*)v)[0],
                 ((int*)v)[1], ((int*)v)[2]), val = b;
      else continue;
      mlog("  %-14s %s.%s = %s", t.c_str(), obj_name(s).c_str(), obj_name(c).c_str(), val.c_str());
    }
}

static void dump_sdk(const std::string& path) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  auto& O = Objects();
  int n = 0;
  for (int i = 0; i < O.Num; ++i) {
    void* o = O.Data[i];
    if (!o) continue;
    std::string cn = obj_name(obj_class(o));
    if (cn != "Class" && cn != "ScriptStruct") continue;
    void* sup = P(o, O_Super);
    fprintf(f, "%s %s : %s  size=0x%x\n", cn.c_str(), obj_path(o).c_str(), sup ? obj_name(sup).c_str() : "-", I(o, O_PropsSize));
    for (void* c = P(o, O_Children); c; c = P(c, O_Next)) {
      std::string ccn = obj_name(obj_class(c));
      if (ccn.size() > 8 && ccn.compare(ccn.size() - 8, 8, "Property") == 0)
        fprintf(f, "  0x%04x %-18s %s%s\n", I(c, O_Offset), ccn.c_str(), obj_name(c).c_str(),
                I(c, O_ArrayDim) > 1 ? ("[" + std::to_string(I(c, O_ArrayDim)) + "]").c_str() : "");
      else if (ccn == "Function") fprintf(f, "  fn %s\n", obj_name(c).c_str());
    }
    ++n;
  }
  fclose(f);
  mlog("dump: %d types -> %s", n, path.c_str());
}

static int g_shot;
static char g_shot_name[128];

static void run_command(const std::string& line) {
  mlog("> %s", line.c_str());
  auto sp = line.find(' ');
  std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
  if (cmd == "exec") engine_exec(arg);
  else if (cmd == "info") log_info();
  else if (cmd == "dump") dump_sdk(run_dir() + "/sdk.txt");
  else if (cmd == "shot") { snprintf(g_shot_name, sizeof g_shot_name, "%s", arg.empty() ? "shot" : arg.c_str()); g_shot = 1; }
  else if (cmd == "fire") fire(atoi(arg.c_str()) ? 1 : 0);
  else if (cmd == "close") destroy_portals();
  else if (cmd == "props") { void* o = (void*)strtoull(arg.c_str(), nullptr, 16); if (o) dump_props(o); }
  else if (cmd == "enum") {
    void* e = (void*)strtoull(arg.c_str(), nullptr, 16);
    auto& n = *(TArray<FName>*)((u8*)e + 0x68);
    for (int i = 0; i < n.Num; ++i) mlog("  %d %s", i, name_str(n.Data[i]).c_str());
  } else if (cmd == "look") {  // look <yaw> <pitch>: aim the camera (tests)
    float y = 0, p = 0;
    sscanf(arg.c_str(), "%f %f", &y, &p);
    if (void* pawn = local_pawn()) set_view(pawn, FRotator{(int)(p / 360 * 65536), (int)(y / 360 * 65536), 0});
  } else if (cmd == "scan") {  // scan <lo> <hi>: int/float fields of pc, pawn, PlayerInput, camera in [lo, hi]
    float lo = 0, hi = 0;
    sscanf(arg.c_str(), "%f %f", &lo, &hi);
    void* pc = local_pc();
    void* objs[4] = {pc, local_pawn(), get<void*>(pc, "PlayerInput"), get<void*>(pc, "PlayerCamera")};
    for (void* o : objs) {
      if (!o) continue;
      int size = I(obj_class(o), O_PropsSize);
      for (int off = 0; off + 4 <= size; off += 4) {
        int iv = *(int*)((u8*)o + off);
        float fv = *(float*)((u8*)o + off);
        if ((iv >= lo && iv <= hi) || (fv >= lo && fv <= hi)) mlog("  %s +0x%x int %d float %g", obj_name(o).c_str(), off, iv, fv);
      }
    }
  } else if (cmd == "meshinfo") {  // meshinfo <StaticMesh name>: render data layout check
    void* m = find_named("StaticMesh", arg.c_str());
    if (!m) { mlog("no mesh"); return; }
    auto& lods = *(TArray<u8*>*)((u8*)m + 0x60);
    mlog("lods %d", lods.Num);
    if (lods.Num < 1) return;
    u8* rd = lods.Data[0];
    mlog("  VB ntex %d stride %d nverts %d | Pos stride %d n %d | Col stride %d n %d | NumVertices %d",
         I(rd, 0x38), I(rd, 0x48), I(rd, 0x4c), I(rd, 0x58 + 0x40), I(rd, 0x58 + 0x44), I(rd, 0xa0 + 0x40), I(rd, 0xa0 + 0x44), I(rd, 0xe8));
    mlog("  Index num %d max %d | wire num %d | elements %d", I(rd, 0x130), I(rd, 0x134), I(rd, 0x188), I(rd, 0x198));
    u8* el = *(u8**)(rd + 0x190);
    for (int k = 0; k < 16; ++k) mlog("    el+0x%02x = %d / %p", k * 4, I(el, k * 4), k % 2 == 0 ? *(void**)(el + k * 4) : nullptr);
    float* pos = *(float**)(rd + 0x58 + 0x38);
    for (int k = 0; k < 3; ++k) mlog("    pos %g %g %g", pos[k * 3], pos[k * 3 + 1], pos[k * 3 + 2]);
    // where is the mesh's own Bounds? print floats of the UStaticMesh between 0x70 and 0x160
    for (int off = 0x70; off < 0x170; off += 16)
      mlog("    mesh+0x%x: %g %g %g %g", off, *(float*)((u8*)m + off), *(float*)((u8*)m + off + 4), *(float*)((u8*)m + off + 8), *(float*)((u8*)m + off + 12));
  } else if (cmd == "matparams") {  // matparams <substring>: texture/vector/scalar parameters of matching materials
    FName t2 = find_name("MaterialExpressionTextureSampleParameter2D"), vp = find_name("MaterialExpressionVectorParameter"),
          sp = find_name("MaterialExpressionScalarParameter");
    auto& O = Objects();
    for (int i = 0; i < O.Num; ++i) {
      void* o = O.Data[i];
      if (!o) continue;
      void* c = obj_class(o);
      if (!(name_is(c, t2) || name_is(c, vp) || name_is(c, sp))) continue;
      void* mat = P(o, O_Outer);
      std::string path = obj_path(mat);
      if (path.find(arg) == std::string::npos || is_default(o)) continue;
      mlog("  %s  %s %s", path.c_str(), obj_name(c).c_str() + 18, name_str(get<FName>(o, "ParameterName")).c_str());
    }
  } else if (cmd == "gunmat") {  // gunmat emissive|handycam|<material name> [dpg]
    char name[128] = {0};
    int dpg = 2;
    sscanf(arg.c_str(), "%127s %d", name, &dpg);
    void* parent = std::string(name) == "emissive" ? g_emissive : find_named("Material", std::string(name) == "handycam" ? "handycam_mat" : name);
    if (parent && g_gun_comp) {
      void* mic = make_mic(parent, g_gun_tex, parent == g_emissive ? g_tex_param : find_name("Diffuse"));
      Call(g_gun_comp, "SetMaterial").arg("ElementIndex", 0).arg("Material", mic).go();
      Call(g_gun_comp, "SetDepthPriorityGroup").arg<u8>("NewDepthPriorityGroup", (u8)dpg).go();
      if (void* le = get<void*>(local_pawn(), "LightEnvironment")) Call(g_gun_comp, "SetLightEnvironment").arg("NewLightEnvironment", le).go();
      mlog("gun material %s dpg %d", obj_name(parent).c_str(), dpg);
    }
  } else if (cmd == "gunpose") {  // gunpose x y z pitch yaw roll scale (degrees): tune the held gun
    float x, y, z, p, yw, r, sc;
    if (sscanf(arg.c_str(), "%f %f %f %f %f %f %f", &x, &y, &z, &p, &yw, &r, &sc) == 7 && g_gun_comp) {
      g_gun_off = {x, y, z};
      g_gun_rot = {(int)(p / 360 * 65536), (int)(yw / 360 * 65536), (int)(r / 360 * 65536)};
      g_gun_scale = sc;
      pose_gun();
    }
  } else if (cmd == "flip") {  // flip <x> <y> <capmode>
    sscanf(arg.c_str(), "%d %d %d", &g_flipx, &g_flipy, &g_capmode);
  } else if (cmd == "approach") {  // approach <i>: stand Miles in front of portal i, moving into it (tests)
    int i = atoi(arg.c_str()) ? 1 : 0;
    void* pawn = local_pawn();
    if (pawn && g_p[i].open) {
      const Portal& A = g_p[i];
      FVector p = A.loc + A.n * (extent_along(pawn, A.n) + 6);
      if (std::fabs(A.n.Z) < 0.7f) p.Z = get<FVector>(pawn, "Location").Z;
      mlog("approach ok=%d", Call(pawn, "SetLocation").arg("NewLocation", p).go().ret_bool());
      set<FVector>(pawn, "Velocity", A.n * -300.f);
      set<FVector>(pawn, "Acceleration", A.n * -300.f);
    }
  } else if (cmd == "walk") {  // walk <x y z>: push the pawn with a velocity (tests)
    FVector v{};
    sscanf(arg.c_str(), "%f %f %f", &v.X, &v.Y, &v.Z);
    if (void* pawn = local_pawn()) { set<FVector>(pawn, "Velocity", v); set<FVector>(pawn, "Acceleration", v); }
  } else if (cmd == "continue") {  // press Continue in the main menu
    if (void* mm = find_instance("OLUIFrontEnd_MainMenu")) Call(mm, "OnContinueButtonPress").go();
  } else if (cmd == "objs") {
    auto& O = Objects();
    int k = 0;
    for (int i = 0; i < O.Num && k < 60; ++i)
      if (O.Data[i]) {
        std::string fn = full_name(O.Data[i]);
        if (fn.find(arg) != std::string::npos) { mlog("  %p %s", O.Data[i], fn.c_str()); ++k; }
      }
  } else mlog("unknown command");
}

static void poll_commands() {
  std::string p = run_dir() + "/cmd";
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || st.st_size == 0) return;
  FILE* f = fopen(p.c_str(), "r+");
  if (!f) return;
  std::vector<std::string> lines;
  char buf[4096];
  while (fgets(buf, sizeof buf, f)) {
    std::string l(buf);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    if (!l.empty()) lines.push_back(l);
  }
  if (ftruncate(fileno(f), 0) != 0) mlog("cmd truncate failed");
  fclose(f);
  for (auto& l : lines) run_command(l);
}

// ---------------------------------------------------------------- exports
extern "C" void mod_init(Host* h) {
  H = h;
  g_log = fopen((run_dir() + "/mod.log").c_str(), "a");
  snd::load(std::string(H->root) + "/cache/sounds");
  H->set_audio_mix(snd::mix);
  mlog("mod loaded, %d sounds", (int)snd::bank.size());
}
extern "C" void mod_unload() {
  destroy_portals();
  if (g_gun_comp && g_gun_owner) if (void* mesh = get<void*>(g_gun_owner, "Mesh")) Call(mesh, "DetachComponent").arg("Component", g_gun_comp).go();
  mlog("mod unloading");
  if (g_log) fclose(g_log);
  g_log = nullptr;
}

extern "C" void mod_tick(void*, float) {
  static long n = 0;
  if (n++ == 0) { mlog("tick live, %d objects", Objects().Num); }
  poll_commands();
  void* pc = local_pc();
  if (!pc) return;
  void* wi = get<void*>(pc, "WorldInfo");
  if (wi != g_world) { g_world = wi; forget_portals(); g_cooldown.clear(); mlog("world %s", wi ? obj_path(wi).c_str() : "-"); }
  if (void* pi = get<void*>(pc, "PlayerInput")) H->hook_input(&(*(void***)pi)[78]);
  update_gun();
  if (wi && (g_p[0].open || g_p[1].open)) update_views();
  if (!wi || !g_p[0].open || !g_p[1].open) return;
  float now = get<float>(wi, "TimeSeconds");
  for (void* p = get<void*>(wi, "PawnList"); p; p = get<void*>(p, "NextPawn"))
    if (!get_bool(p, "bDeleteMe")) try_teleport(p, now);
}

extern "C" void mod_swap(void* win) {
  if (!g_shot) return;
  g_shot = 0;
  static auto getproc = (void* (*)(const char*))dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
  static auto drawsize = (void (*)(void*, int*, int*))dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize");
  auto readpx = (void (*)(int, int, int, int, unsigned, unsigned, void*))getproc("glReadPixels");
  auto geti = (void (*)(unsigned, int*))getproc("glGetIntegerv");
  auto bindfb = (void (*)(unsigned, unsigned))getproc("glBindFramebuffer");
  auto store = (void (*)(unsigned, int))getproc("glPixelStorei");
  int w = 0, h = 0, prev = 0;
  drawsize(win, &w, &h);
  geti(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &prev);
  bindfb(0x8CA8 /*GL_READ_FRAMEBUFFER*/, 0);
  store(0x0D05 /*GL_PACK_ALIGNMENT*/, 1);
  std::vector<u8> px((size_t)w * h * 3);
  readpx(0, 0, w, h, 0x1907 /*GL_RGB*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px.data());
  bindfb(0x8CA8, prev);
  std::string path = run_dir() + "/" + g_shot_name + ".ppm";
  if (FILE* f = fopen(path.c_str(), "wb")) {
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = h - 1; y >= 0; --y) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    fclose(f);
  }
  mlog("shot %dx%d -> %s", w, h, path.c_str());
}
