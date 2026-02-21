$input v_color0, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_texcoord5

#include "common.h"

SAMPLER2D(s_texBase,   0);
SAMPLER2D(s_texMask, 5);
uniform vec4 u_maskParams;

void main()
{
	if (u_maskParams.w > 0.5) {
		float mu = (v_texcoord5.x - u_maskParams.x) / u_maskParams.z;
		float mv = (v_texcoord5.y - u_maskParams.y) / u_maskParams.z;
		if (texture2D(s_texMask, vec2(mu, mv)).r < 0.5)
			discard;
	}
	vec4 c = texture2D(s_texBase, v_texcoord0);
	gl_FragColor = vec4(saturate(c.xyz * v_color0.xyz), 1.0);
}
