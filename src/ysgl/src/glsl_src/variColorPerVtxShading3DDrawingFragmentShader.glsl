#include "YS_GLSL_HEADER.glsl"

varying  LOWP  vec4 colorOut;

#include "YS_GLSL_TEXTURE_VARIABLE.glsl"
#ifndef YSGLSL_OPAQUE
	#include "YS_GLSL_ALPHA_CUTOFF_PARAMETER.glsl"
#endif
#include "YS_GLSL_FOG_VARIABLES.glsl"

void main()
{
	gl_FragColor=colorOut;

	#include "YS_GLSL_TEXTURE_IN_FRAGMENTSHADER.glsl"
#ifndef YSGLSL_OPAQUE
	#include "YS_GLSL_ALPHA_CUTOFF_IN_FRAGMENT_SHADER.glsl"
#endif
	#include "YS_GLSL_FOGCALCULATION_IN_FRAGMENTSHADER.glsl"
}
