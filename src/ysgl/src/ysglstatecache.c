#define YSGL_STATECACHE_IMPL
#include "ysglstatecache.h"

#include <string.h>

#define YSGL_MAX_TRACKED_ATTRIB 32

/* Last value written to each (program,location).  Vertex buffer draws re-send
   the same matrices and colours for object after object, and the driver pays
   for every one of them.

   Generation counter instead of clearing the table: a program id can be
   reused after a program is deleted, and stale values would then wrongly
   suppress an upload, so the whole table is retired once per frame. */
#define YSGL_NUNIFORM_SLOT 1024
#define YSGL_MAX_UNIFORM_FLOAT 16

static struct
{
	unsigned int generation;
	GLuint programId;
	GLint location;
	int nFloat;
	GLfloat value[YSGL_MAX_UNIFORM_FLOAT];
} uniformSlot[YSGL_NUNIFORM_SLOT];

static unsigned int uniformGeneration=1;

/* Returns nonzero when the value is already known to be in the program. */
static int YsGLUniformIsUnchanged(GLint location,const GLfloat *value,int nFloat)
{
	unsigned long hash;
	int probe;
	GLuint programId;

	if(YSGL_MAX_UNIFORM_FLOAT<nFloat || 0>location)
	{
		return 0;
	}

	programId=YsGLCacheGetCurrentProgram();
	hash=((unsigned long)programId*131UL+(unsigned long)(location+1)*1299721UL)%YSGL_NUNIFORM_SLOT;

	for(probe=0; probe<4; ++probe)
	{
		unsigned long i=(hash+(unsigned long)probe)%YSGL_NUNIFORM_SLOT;

		if(uniformSlot[i].generation!=uniformGeneration)
		{
			uniformSlot[i].generation=uniformGeneration;
			uniformSlot[i].programId=programId;
			uniformSlot[i].location=location;
			uniformSlot[i].nFloat=nFloat;
			memcpy(uniformSlot[i].value,value,sizeof(GLfloat)*(size_t)nFloat);
			return 0;
		}
		if(uniformSlot[i].programId==programId && uniformSlot[i].location==location)
		{
			if(uniformSlot[i].nFloat==nFloat &&
			   0==memcmp(uniformSlot[i].value,value,sizeof(GLfloat)*(size_t)nFloat))
			{
				return 1;
			}
			uniformSlot[i].nFloat=nFloat;
			memcpy(uniformSlot[i].value,value,sizeof(GLfloat)*(size_t)nFloat);
			return 0;
		}
	}
	return 0;   /* Slot contention.  Upload and stay correct. */
}

static GLuint cachedProgram=0;
static int cachedProgramValid=0;

static GLuint cachedArrayBuffer=0;
static int cachedArrayBufferValid=0;
static GLuint cachedElementArrayBuffer=0;
static int cachedElementArrayBufferValid=0;

/* The vertex buffer draw helpers unbind to 0 after every object, and the next
   object binds again straight away.  The unbind is only observable to a draw
   that feeds vertices from client memory, so it is held back until something
   is about to read the binding. */
static int pendingArrayBufferUnbind=0;

/* desiredAttrib is what the caller asked for, appliedAttrib is what the
   driver has been told.  They are reconciled just before a draw. */
static unsigned int desiredAttrib=0;
static unsigned int appliedAttrib=0;
static int appliedAttribValid=0;

static int nTrackedAttrib=0;

static int YsGLNumVertexAttrib(void)
{
	if(0==nTrackedAttrib)
	{
		GLint n=0;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,&n);
		if(n<=0 || YSGL_MAX_TRACKED_ATTRIB<n)
		{
			n=YSGL_MAX_TRACKED_ATTRIB;
		}
		nTrackedAttrib=(int)n;
	}
	return nTrackedAttrib;
}

void YsGLInvalidateStateCache(void)
{
	cachedProgramValid=0;
	cachedArrayBufferValid=0;
	cachedElementArrayBufferValid=0;
	pendingArrayBufferUnbind=0;
	appliedAttribValid=0;
	++uniformGeneration;
}

void YsGLCacheUseProgram(GLuint programId)
{
	if(0!=cachedProgramValid && programId==cachedProgram)
	{
		return;
	}
	glUseProgram(programId);
	cachedProgram=programId;
	cachedProgramValid=1;
}

GLuint YsGLCacheGetCurrentProgram(void)
{
	if(0==cachedProgramValid)
	{
		GLint cur=0;
		glGetIntegerv(GL_CURRENT_PROGRAM,&cur);
		cachedProgram=(GLuint)cur;
		cachedProgramValid=1;
	}
	return cachedProgram;
}

void YsGLCacheGetIntegerv(GLenum pname,GLint *param)
{
	if(GL_CURRENT_PROGRAM==pname)
	{
		*param=(GLint)YsGLCacheGetCurrentProgram();
		return;
	}
	glGetIntegerv(pname,param);
}

void YsGLCacheBindBuffer(GLenum target,GLuint bufferId)
{
	if(GL_ARRAY_BUFFER==target)
	{
		if(0==bufferId && 0!=cachedArrayBufferValid && 0!=cachedArrayBuffer)
		{
			pendingArrayBufferUnbind=1;
			return;
		}
		pendingArrayBufferUnbind=0;
		if(0!=cachedArrayBufferValid && bufferId==cachedArrayBuffer)
		{
			return;
		}
		glBindBuffer(target,bufferId);
		cachedArrayBuffer=bufferId;
		cachedArrayBufferValid=1;
		return;
	}
	if(GL_ELEMENT_ARRAY_BUFFER==target)
	{
		if(0!=cachedElementArrayBufferValid && bufferId==cachedElementArrayBuffer)
		{
			return;
		}
		glBindBuffer(target,bufferId);
		cachedElementArrayBuffer=bufferId;
		cachedElementArrayBufferValid=1;
		return;
	}
	glBindBuffer(target,bufferId);
}

void YsGLCacheEnableVertexAttribArray(GLuint index)
{
	if(YSGL_MAX_TRACKED_ATTRIB<=index)
	{
		glEnableVertexAttribArray(index);
		return;
	}
	desiredAttrib|=(1u<<index);
}

void YsGLCacheDisableVertexAttribArray(GLuint index)
{
	if(YSGL_MAX_TRACKED_ATTRIB<=index)
	{
		glDisableVertexAttribArray(index);
		return;
	}
	desiredAttrib&=~(1u<<index);
}

static void YsGLFlushVertexAttribArray(void)
{
	unsigned int diff;
	int i,n;

	if(0==appliedAttribValid)
	{
		n=YsGLNumVertexAttrib();
		for(i=0; i<n; ++i)
		{
			if(0!=(desiredAttrib&(1u<<i)))
			{
				glEnableVertexAttribArray((GLuint)i);
			}
			else
			{
				glDisableVertexAttribArray((GLuint)i);
			}
		}
		appliedAttrib=desiredAttrib;
		appliedAttribValid=1;
		return;
	}

	diff=appliedAttrib^desiredAttrib;
	if(0==diff)
	{
		return;
	}

	n=YsGLNumVertexAttrib();
	for(i=0; i<n; ++i)
	{
		if(0!=(diff&(1u<<i)))
		{
			if(0!=(desiredAttrib&(1u<<i)))
			{
				glEnableVertexAttribArray((GLuint)i);
			}
			else
			{
				glDisableVertexAttribArray((GLuint)i);
			}
		}
	}
	appliedAttrib=desiredAttrib;
}

static void YsGLFlushArrayBufferUnbind(void)
{
	if(0!=pendingArrayBufferUnbind)
	{
		pendingArrayBufferUnbind=0;
		glBindBuffer(GL_ARRAY_BUFFER,0);
		cachedArrayBuffer=0;
		cachedArrayBufferValid=1;
	}
}

/* The binding in effect when the pointer is specified is the one that decides
   whether it is an offset or a client address, so the deferred unbind has to
   be settled here rather than at the draw. */
void YsGLCacheVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void *pointer)
{
	YsGLFlushArrayBufferUnbind();
	glVertexAttribPointer(index,size,type,normalized,stride,pointer);
}

void YsGLCacheDrawArrays(GLenum mode,GLint first,GLsizei count)
{
	YsGLFlushArrayBufferUnbind();
	YsGLFlushVertexAttribArray();
	glDrawArrays(mode,first,count);
}

void YsGLCacheDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices)
{
	YsGLFlushArrayBufferUnbind();
	YsGLFlushVertexAttribArray();
	glDrawElements(mode,count,type,indices);
}

/* Deleting the object that the shadow copy names would leave the cache
   claiming a binding that GL has already dropped, and buffer and program ids
   get reused. */
void YsGLCacheDeleteBuffers(GLsizei n,const GLuint *buffers)
{
	glDeleteBuffers(n,buffers);
	cachedArrayBufferValid=0;
	cachedElementArrayBufferValid=0;
	pendingArrayBufferUnbind=0;
}

void YsGLCacheDeleteProgram(GLuint programId)
{
	glDeleteProgram(programId);
	cachedProgramValid=0;
	++uniformGeneration;
}

void YsGLCacheUniform1i(GLint location,GLint v)
{
	GLfloat f=(GLfloat)v;
	if(0!=YsGLUniformIsUnchanged(location,&f,1))
	{
		return;
	}
	glUniform1i(location,v);
}

void YsGLCacheUniform1f(GLint location,GLfloat v)
{
	if(0!=YsGLUniformIsUnchanged(location,&v,1))
	{
		return;
	}
	glUniform1f(location,v);
}

void YsGLCacheUniform3fv(GLint location,GLsizei count,const GLfloat *v)
{
	if(1==count && 0!=YsGLUniformIsUnchanged(location,v,3))
	{
		return;
	}
	glUniform3fv(location,count,v);
}

void YsGLCacheUniform4fv(GLint location,GLsizei count,const GLfloat *v)
{
	if(1==count && 0!=YsGLUniformIsUnchanged(location,v,4))
	{
		return;
	}
	glUniform4fv(location,count,v);
}

void YsGLCacheUniformMatrix4fv(GLint location,GLsizei count,GLboolean transpose,const GLfloat *v)
{
	if(1==count && GL_FALSE==transpose && 0!=YsGLUniformIsUnchanged(location,v,16))
	{
		return;
	}
	glUniformMatrix4fv(location,count,transpose,v);
}
