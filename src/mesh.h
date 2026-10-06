// Runtime geometry and textures inside Outlast's UE3 renderer (layouts from disassembly; see MODLOG.md).
#pragma once
#include <vector>
#include "ue.h"

// FStaticMeshBuildVertex, 64 bytes (stride read from FPositionVertexBuffer::Init)
struct BuildVert {
  FVector pos;
  u8 tx[4], ty[4], tz[4];  // FPackedNormal tangent basis
  float uv[4][2];
  u8 color[4];
  unsigned short frag, pad;
};
static_assert(sizeof(BuildVert) == 64, "FStaticMeshBuildVertex");

inline void pack_normal(u8* o, FVector v, u8 w = 255) {
  o[0] = (u8)std::lround(v.X * 127.5f + 127.5f);
  o[1] = (u8)std::lround(v.Y * 127.5f + 127.5f);
  o[2] = (u8)std::lround(v.Z * 127.5f + 127.5f);
  o[3] = w;
}
inline BuildVert make_vert(FVector p, FVector n, FVector t, float u, float v) {
  BuildVert b{};
  b.pos = p;
  pack_normal(b.tx, t);
  pack_normal(b.ty, cross(n, t));
  pack_normal(b.tz, n);
  b.uv[0][0] = u; b.uv[0][1] = v;
  b.color[0] = b.color[1] = b.color[2] = b.color[3] = 255;
  return b;
}

// Replace LOD 0 of an existing UStaticMesh (one material section) with new vertices/triangles.
// FStaticMeshRenderData: VertexBuffer 0x0, PositionVertexBuffer 0x58, ColorVertexBuffer 0xa0, NumVertices 0xe8,
// IndexBuffer 0xf0 (indices TArray<WORD> at 0x128), Elements TArray at 0x190 (FirstIndex +0x24, NumTriangles +0x28,
// MinVertexIndex +0x2c, MaxVertexIndex +0x30). UStaticMesh: LODModels 0x60, Bounds 0x100 (origin, extent, radius).
inline bool rebuild_mesh(void* mesh, std::vector<BuildVert>& verts, std::vector<unsigned short>& idx) {
  auto& lods = *(TArray<u8*>*)((u8*)mesh + 0x60);
  if (lods.Num < 1 || verts.empty() || verts.size() > 65535) return false;
  u8* rd = lods.Data[0];
  if (*(int*)(rd + 0x198) != 1) return false;
  ((void (*)(void*))A_SM_ReleaseResources)(mesh);
  ((void (*)())A_FlushRenderingCommands)();
  TArray<BuildVert> arr{verts.data(), (int)verts.size(), (int)verts.size()};
  ((void (*)(void*, void*, unsigned))A_SMVB_Init)(rd + 0x0, &arr, 1);
  ((void (*)(void*, void*))A_PosVB_Init)(rd + 0x58, &arr);
  *(int*)(rd + 0xa0 + 0x44) = 0;  // no vertex colours
  *(int*)(rd + 0xe8) = (int)verts.size();
  auto& ib = *(TArray<unsigned short>*)(rd + 0x128);
  if ((int)idx.size() > ib.Max) {
    ib.Data = (unsigned short*)((void* (*)(void*, unsigned, unsigned))A_appRealloc)(ib.Data, idx.size() * 2, 8);
    ib.Max = (int)idx.size();
  }
  memcpy(ib.Data, idx.data(), idx.size() * 2);
  ib.Num = (int)idx.size();
  *(int*)(rd + 0x188) = 0;  // no wireframe indices
  u8* el = *(u8**)(rd + 0x190);
  *(int*)(el + 0x24) = 0;
  *(int*)(el + 0x28) = (int)idx.size() / 3;
  *(int*)(el + 0x2c) = 0;
  *(int*)(el + 0x30) = (int)verts.size() - 1;
  FVector lo = verts[0].pos, hi = verts[0].pos;
  for (auto& v : verts) {
    lo = {std::fmin(lo.X, v.pos.X), std::fmin(lo.Y, v.pos.Y), std::fmin(lo.Z, v.pos.Z)};
    hi = {std::fmax(hi.X, v.pos.X), std::fmax(hi.Y, v.pos.Y), std::fmax(hi.Z, v.pos.Z)};
  }
  FVector c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
  *(FVector*)((u8*)mesh + 0x100) = c;
  *(FVector*)((u8*)mesh + 0x10c) = e;
  *(float*)((u8*)mesh + 0x118) = len(e);
  ((void (*)(void*))A_SM_InitResources)(mesh);
  return true;
}

// A rooted UTexture2D (PF_A8R8G8B8, BGRA bytes) filled from memory. Mips is a TIndirectArray at 0x140; each
// FTexture2DMipMap starts with its FUntypedBulkData.
inline void* make_texture(int w, int h, const unsigned* bgra) {
  extern void* cdo(const char*);
  void* t = Call(cdo("Texture2D"), "Create").arg("InSizeX", w).arg("InSizeY", h).arg<u8>("InFormat", 2).go().ret<void*>();
  if (!t) return nullptr;
  extern void add_root(void*);
  add_root(t);
  auto& mips = *(TArray<u8*>*)((u8*)t + 0x140);
  if (mips.Num < 1) return t;
  void* bulk = mips.Data[0];
  void* dst = ((void* (*)(void*, unsigned))A_BulkLock)(bulk, 2 /*LOCK_READ_WRITE*/);
  if (dst) memcpy(dst, bgra, (size_t)w * h * 4);
  ((void (*)(void*))A_BulkUnlock)(bulk);
  ((void (*)(void*))A_Tex2D_UpdateResource)(t);
  return t;
}
