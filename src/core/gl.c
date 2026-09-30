// Windows: the GL 2.0+ functions the core uses, loaded once the context exists (see gl.h). Nothing on Linux.
#include "gl.h"
#ifdef _WIN32
#include "../platform/plat.h"
#include <stdio.h>

#define GL_DEFINE(ret, name, args) PFN_jl_##name jl_##name;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

int gl_load(void) {
  int ok = 1;
#define GL_LOADFN(ret, name, args)                                                  \
  jl_##name = (PFN_jl_##name)plat_gl_proc(#name);                                 \
  if (!jl_##name) { fprintf(stderr, "OpenGL function missing: %s\n", #name); ok = 0; }
  GL_FUNCS(GL_LOADFN)
#undef GL_LOADFN
  return ok;
}
#endif
