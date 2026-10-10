// OpenGL scalar types as Wine's unix-side opengl32 thunks pass them
// (include/wine/wgl.h). WineMetalGL is called through Wine's function table,
// so these must match Wine, not Apple's <OpenGL/gltypes.h>.
#pragma once

#include <cstddef>
#include <cstdint>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef int8_t GLbyte;
typedef uint8_t GLubyte;
typedef int16_t GLshort;
typedef uint16_t GLushort;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLclampx;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void* GLeglClientBufferEXT;
typedef void* GLeglImageOES;
typedef char GLchar;
typedef char GLcharARB;
typedef unsigned int GLhandleARB;
typedef uint16_t GLhalf;
typedef uint16_t GLhalfARB;
typedef uint16_t GLhalfNV;
typedef int GLfixed;
typedef intptr_t GLintptr;
typedef intptr_t GLintptrARB;
typedef intptr_t GLsizeiptr;
typedef intptr_t GLsizeiptrARB;
typedef int64_t GLint64;
typedef int64_t GLint64EXT;
typedef uint64_t GLuint64;
typedef uint64_t GLuint64EXT;
typedef struct __GLsync* GLsync;
struct _cl_context;
struct _cl_event;
typedef void (*GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                            const GLchar* message, const void* userParam);
typedef GLDEBUGPROC GLDEBUGPROCARB;
typedef GLDEBUGPROC GLDEBUGPROCKHR;
typedef void (*GLDEBUGPROCAMD)(GLuint id, GLenum category, GLenum severity, GLsizei length,
                               const GLchar* message, void* userParam);
typedef intptr_t GLvdpauSurfaceNV;
typedef void (*GLVULKANPROCNV)(void);

#include "gl_enums.gen.h"
