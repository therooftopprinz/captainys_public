/* Shadow copy of the handful of GL states that the GLSL renderers touch on
   every draw.

   The renderers bind their program and re-enable their vertex attribute
   arrays each time they are activated, and disable them again on
   deactivation.  On a tiled mobile driver each of those calls costs real CPU
   time, and profiling the R36S showed the Mali blob using more than half of
   the frame budget servicing them.  Redundant program and buffer binds are
   dropped here, and attribute array enables are accumulated and applied only
   when a draw actually needs them, so the state seen by any draw call is
   unchanged.

   Only the OpenGL ES 2 build routes through this; desktop GL keeps calling
   the driver directly. */

#ifndef YSGLSTATECACHE_IS_INCLUDED
#define YSGLSTATECACHE_IS_INCLUDED

#include "ysglheader.h"

#ifdef __cplusplus
extern "C" {
#endif

void YsGLCacheUseProgram(GLuint programId);
GLuint YsGLCacheGetCurrentProgram(void);
void YsGLCacheBindBuffer(GLenum target,GLuint bufferId);
void YsGLCacheEnableVertexAttribArray(GLuint index);
void YsGLCacheDisableVertexAttribArray(GLuint index);
void YsGLCacheGetIntegerv(GLenum pname,GLint *param);
void YsGLCacheVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void *pointer);
void YsGLCacheDrawArrays(GLenum mode,GLint first,GLsizei count);
void YsGLCacheDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices);

void YsGLCacheDeleteBuffers(GLsizei n,const GLuint *buffers);
void YsGLCacheDeleteProgram(GLuint programId);

void YsGLCacheUniform1i(GLint location,GLint v);
void YsGLCacheUniform1f(GLint location,GLfloat v);
void YsGLCacheUniform3fv(GLint location,GLsizei count,const GLfloat *v);
void YsGLCacheUniform4fv(GLint location,GLsizei count,const GLfloat *v);
void YsGLCacheUniformMatrix4fv(GLint location,GLsizei count,GLboolean transpose,const GLfloat *v);

/* Must be called whenever GL state may have changed behind the cache's back,
   ie. after a context switch and once per frame for safety. */
void YsGLInvalidateStateCache(void);

#ifdef __cplusplus
}
#endif

/* Define YSGL_NO_STATE_CACHE to send every call straight to the driver, for
   benchmarking against the cache or for bisecting a suspected cache bug. */
#if defined(YS_GL_ES2) && !defined(YSGL_STATECACHE_IMPL) && !defined(YSGL_NO_STATE_CACHE)
#define glUseProgram               YsGLCacheUseProgram
#define glBindBuffer               YsGLCacheBindBuffer
#define glEnableVertexAttribArray  YsGLCacheEnableVertexAttribArray
#define glDisableVertexAttribArray YsGLCacheDisableVertexAttribArray
#define glGetIntegerv              YsGLCacheGetIntegerv
#define glVertexAttribPointer      YsGLCacheVertexAttribPointer
#define glDeleteBuffers            YsGLCacheDeleteBuffers
#define glDeleteProgram            YsGLCacheDeleteProgram
#define glDrawArrays               YsGLCacheDrawArrays
#define glDrawElements             YsGLCacheDrawElements
#define glUniform1i                YsGLCacheUniform1i
#define glUniform1f                YsGLCacheUniform1f
#define glUniform3fv               YsGLCacheUniform3fv
#define glUniform4fv               YsGLCacheUniform4fv
#define glUniformMatrix4fv         YsGLCacheUniformMatrix4fv
#endif

#endif
