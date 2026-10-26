#version 450

// AMD FSR frame generation's HUD-less image (Astro Bot, Astro's Playroom): the finished frame with
// every pixel the game's HUD covers changed far enough that FSR takes it from the newest real
// frame instead of interpolating it (its inpainting pass compares this image with the frame:
// a channel 10% apart takes the frame's pixel). The games draw their HUD into a layer of its own
// that their last pass blends over the scene; a pixel of that layer that differs from its corner
// texel (what the games bind when no HUD is shown) is HUD. The layer covers the whole frame.

layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D frame; // the finished frame, at the target's size
layout(binding = 1) uniform sampler2D hud;   // the game's HUD layer (linear filter)
layout(push_constant) uniform Mask {
	vec2  inv_size;  // 1 / the target's size
	vec2  hud_texel; // 1 / the HUD layer's size
	float threshold; // a difference from the empty texel above this is HUD
};

float Difference(vec4 a, vec4 b) {
	const vec4 d = abs(a - b);
	return max(max(d.r, d.g), max(d.b, d.a));
}

void main() {
	vec4       c     = texelFetch(frame, ivec2(gl_FragCoord.xy), 0);
	const vec2 uv    = gl_FragCoord.xy * inv_size;
	const vec4 empty = texelFetch(hud, ivec2(0, 0), 0);
	// (One HUD texel around too: its soft edges are HUD as well.)
	float d = Difference(texture(hud, uv), empty);
	d       = max(d, Difference(texture(hud, uv + vec2(hud_texel.x, 0.0)), empty));
	d       = max(d, Difference(texture(hud, uv - vec2(hud_texel.x, 0.0)), empty));
	d       = max(d, Difference(texture(hud, uv + vec2(0.0, hud_texel.y)), empty));
	d       = max(d, Difference(texture(hud, uv - vec2(0.0, hud_texel.y)), empty));
	if (d > threshold) {
		const float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));
		c.rgb            = luma > 0.5 ? vec3(0.0) : vec3(1.0);
	}
	color = c;
}
