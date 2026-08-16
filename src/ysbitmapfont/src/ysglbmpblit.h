#ifndef YSGLBMPBLIT_IS_INCLUDED
#define YSGLBMPBLIT_IS_INCLUDED
/* { */

/* GLES-backed OpenGL (gl4es) implements neither glDrawPixels nor glBitmap, so
 * 2D bitmaps and the built-in bitmap fonts have to be drawn as textured quads.
 * Both take the raster position that the glDrawPixels/glBitmap call used: the
 * bottom row of the image lands on rasterY and the image extends upward. */

#ifdef __cplusplus
extern "C" {
#endif

void YsGlBlitRGBA2D(double x,double rasterY,int wid,int hei,const unsigned char *rgba);

void YsGlBlitFontString2D(
    double x,double rasterY,const char str[],
    const unsigned char *const fontPtr[],int fontWid,int fontHei,
    float r,float g,float b);

#ifdef __cplusplus
}
#endif

/* } */
#endif
