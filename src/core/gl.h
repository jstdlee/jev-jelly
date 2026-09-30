// OpenGL 3.3 for the core. Linux's libGL exports every function; Windows' opengl32 only has GL 1.1, so the newer
// ones are function pointers, loaded once by gl_load() after the context exists.
#pragma once
#ifndef _WIN32
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
static inline int gl_load(void) { return 1; }
#else
#include <windows.h>
#include <stddef.h>
#include <GL/gl.h>
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE0 0x84C0
#define GL_RGBA8 0x8058
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_BGRA 0x80E1
#define GL_MULTISAMPLE 0x809D

#define GL_FUNCS(X) \
  X(void, glActiveTexture, (GLenum t)) \
  X(void, glAttachShader, (GLuint p, GLuint s)) \
  X(void, glBindBuffer, (GLenum t, GLuint b)) \
  X(void, glBindFramebuffer, (GLenum t, GLuint f)) \
  X(void, glBindRenderbuffer, (GLenum t, GLuint r)) \
  X(void, glBindVertexArray, (GLuint a)) \
  X(void, glBlitFramebuffer, (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)) \
  X(void, glBufferData, (GLenum t, GLsizeiptr n, const void *d, GLenum u)) \
  X(void, glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr n, const void *d)) \
  X(GLenum, glCheckFramebufferStatus, (GLenum t)) \
  X(void, glCompileShader, (GLuint s)) \
  X(GLuint, glCreateProgram, (void)) \
  X(GLuint, glCreateShader, (GLenum t)) \
  X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *f)) \
  X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *r)) \
  X(void, glEnableVertexAttribArray, (GLuint i)) \
  X(void, glFramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint r)) \
  X(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)) \
  X(void, glGenBuffers, (GLsizei n, GLuint *b)) \
  X(void, glGenFramebuffers, (GLsizei n, GLuint *f)) \
  X(void, glGenRenderbuffers, (GLsizei n, GLuint *r)) \
  X(void, glGenVertexArrays, (GLsizei n, GLuint *a)) \
  X(void, glGetProgramInfoLog, (GLuint p, GLsizei n, GLsizei *l, GLchar *s)) \
  X(void, glGetProgramiv, (GLuint p, GLenum e, GLint *v)) \
  X(void, glGetShaderInfoLog, (GLuint s, GLsizei n, GLsizei *l, GLchar *t)) \
  X(void, glGetShaderiv, (GLuint s, GLenum e, GLint *v)) \
  X(GLint, glGetUniformLocation, (GLuint p, const GLchar *n)) \
  X(void, glLinkProgram, (GLuint p)) \
  X(void, glRenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h)) \
  X(void, glRenderbufferStorageMultisample, (GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)) \
  X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *l)) \
  X(void, glUniform1f, (GLint l, GLfloat a)) \
  X(void, glUniform1i, (GLint l, GLint a)) \
  X(void, glUniform2f, (GLint l, GLfloat a, GLfloat b)) \
  X(void, glUniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c)) \
  X(void, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)) \
  X(void, glUseProgram, (GLuint p)) \
  X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum t, GLboolean nm, GLsizei s, const void *p))

#define GL_DECLARE(ret, name, args) typedef ret(APIENTRY *PFN_jl_##name) args; extern PFN_jl_##name jl_##name;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE
#define glActiveTexture jl_glActiveTexture
#define glAttachShader jl_glAttachShader
#define glBindBuffer jl_glBindBuffer
#define glBindFramebuffer jl_glBindFramebuffer
#define glBindRenderbuffer jl_glBindRenderbuffer
#define glBindVertexArray jl_glBindVertexArray
#define glBlitFramebuffer jl_glBlitFramebuffer
#define glBufferData jl_glBufferData
#define glBufferSubData jl_glBufferSubData
#define glCheckFramebufferStatus jl_glCheckFramebufferStatus
#define glCompileShader jl_glCompileShader
#define glCreateProgram jl_glCreateProgram
#define glCreateShader jl_glCreateShader
#define glDeleteFramebuffers jl_glDeleteFramebuffers
#define glDeleteRenderbuffers jl_glDeleteRenderbuffers
#define glEnableVertexAttribArray jl_glEnableVertexAttribArray
#define glFramebufferRenderbuffer jl_glFramebufferRenderbuffer
#define glFramebufferTexture2D jl_glFramebufferTexture2D
#define glGenBuffers jl_glGenBuffers
#define glGenFramebuffers jl_glGenFramebuffers
#define glGenRenderbuffers jl_glGenRenderbuffers
#define glGenVertexArrays jl_glGenVertexArrays
#define glGetProgramInfoLog jl_glGetProgramInfoLog
#define glGetProgramiv jl_glGetProgramiv
#define glGetShaderInfoLog jl_glGetShaderInfoLog
#define glGetShaderiv jl_glGetShaderiv
#define glGetUniformLocation jl_glGetUniformLocation
#define glLinkProgram jl_glLinkProgram
#define glRenderbufferStorage jl_glRenderbufferStorage
#define glRenderbufferStorageMultisample jl_glRenderbufferStorageMultisample
#define glShaderSource jl_glShaderSource
#define glUniform1f jl_glUniform1f
#define glUniform1i jl_glUniform1i
#define glUniform2f jl_glUniform2f
#define glUniform3f jl_glUniform3f
#define glUniform4f jl_glUniform4f
#define glUseProgram jl_glUseProgram
#define glVertexAttribPointer jl_glVertexAttribPointer
int gl_load(void); // 0 if a function is missing (no OpenGL 3.3)
#endif
