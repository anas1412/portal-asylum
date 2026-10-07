// Release builds: bind libc calls to their original symbol versions so the prebuilt .so files load on older
// distros, not just on the glibc that built them (C23 strto*/sscanf, dl* moved into libc in 2.34, atan2f 2.43).
#pragma once
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoull,strtoull@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_sscanf,sscanf@GLIBC_2.2.5");
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");
__asm__(".symver pthread_once,pthread_once@GLIBC_2.2.5");
__asm__(".symver pow,pow@GLIBC_2.2.5");
__asm__(".symver exp,exp@GLIBC_2.2.5");

#ifdef __cplusplus
#include <new>
// GCC 11+ headers call this (GLIBCXX_3.4.29); define it here so older libstdc++ (GCC 5+) is enough
namespace std {
__attribute__((visibility("hidden"), weak, noreturn)) inline void __throw_bad_array_new_length() { throw std::bad_array_new_length(); }
}
#endif
