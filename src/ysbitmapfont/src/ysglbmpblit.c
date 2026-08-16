#include <stdlib.h>
#include <string.h>

#include <ysglheader.h>

#include "ysglbmpblit.h"

/* The bitmap fonts store one glyph row per 4 bytes (GL_UNPACK_ALIGNMENT) with
 * the leftmost pixel in the most significant bit, bottom row first. */
#define YSGL_FONT_ROW_STRIDE 4

static GLuint ysGlBlitTexture = 0;

void YsGlBlitRGBA2D(double x,double rasterY,int wid,int hei,const unsigned char *rgba)
{
	if(0>=wid || 0>=hei || NULL==rgba)
	{
		return;
	}

	const GLboolean texWasEnabled=glIsEnabled(GL_TEXTURE_2D);
	const GLboolean blendWasEnabled=glIsEnabled(GL_BLEND);
	const GLboolean cullWasEnabled=glIsEnabled(GL_CULL_FACE);
	GLfloat prevColor[4]={1.0f,1.0f,1.0f,1.0f};
	glGetFloatv(GL_CURRENT_COLOR,prevColor);

	if(0==ysGlBlitTexture)
	{
		glGenTextures(1,&ysGlBlitTexture);
	}

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D,ysGlBlitTexture);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,wid,hei,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);

	/* Row 0 sits on rasterY and the image grows upward, which is toward
	 * smaller Y in the GUI's top-down ortho projection. */
	const double yBottom=rasterY+1.0;
	const double yTop=rasterY-(double)hei+1.0;

	glBegin(GL_QUADS);
	glTexCoord2f(0.0f,0.0f); glVertex2d(x,yBottom);
	glTexCoord2f(1.0f,0.0f); glVertex2d(x+(double)wid,yBottom);
	glTexCoord2f(1.0f,1.0f); glVertex2d(x+(double)wid,yTop);
	glTexCoord2f(0.0f,1.0f); glVertex2d(x,yTop);
	glEnd();

	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
	glBindTexture(GL_TEXTURE_2D,0);
	if(GL_TRUE!=texWasEnabled)
	{
		glDisable(GL_TEXTURE_2D);
	}
	if(GL_TRUE!=blendWasEnabled)
	{
		glDisable(GL_BLEND);
	}
	if(GL_TRUE==cullWasEnabled)
	{
		glEnable(GL_CULL_FACE);
	}
	glColor4fv(prevColor);
}

void YsGlBlitFontString2D(
    double x,double rasterY,const char str[],
    const unsigned char *const fontPtr[],int fontWid,int fontHei,
    float r,float g,float b)
{
	if(NULL==str || 0==str[0] || NULL==fontPtr || 0>=fontWid || 0>=fontHei)
	{
		return;
	}

	const int nChar=(int)strlen(str);
	const int wid=fontWid*nChar;
	unsigned char *rgba=(unsigned char *)calloc((size_t)wid*(size_t)fontHei,4);
	if(NULL==rgba)
	{
		return;
	}

	const unsigned char rc=(unsigned char)(r*255.0f);
	const unsigned char gc=(unsigned char)(g*255.0f);
	const unsigned char bc=(unsigned char)(b*255.0f);

	for(int i=0; i<nChar; ++i)
	{
		const unsigned char *glyph=fontPtr[((const unsigned char *)str)[i]];
		if(NULL==glyph)
		{
			continue;
		}
		for(int row=0; row<fontHei; ++row)
		{
			const unsigned char *srcRow=glyph+(size_t)row*YSGL_FONT_ROW_STRIDE;
			unsigned char *dstRow=rgba+((size_t)row*(size_t)wid+(size_t)i*(size_t)fontWid)*4;
			for(int col=0; col<fontWid; ++col)
			{
				if(0!=(srcRow[col/8]&(0x80>>(col%8))))
				{
					dstRow[col*4  ]=rc;
					dstRow[col*4+1]=gc;
					dstRow[col*4+2]=bc;
					dstRow[col*4+3]=255;
				}
			}
		}
	}

	YsGlBlitRGBA2D(x,rasterY,wid,fontHei,rgba);
	free(rgba);
}
