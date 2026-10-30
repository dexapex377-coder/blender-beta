#version 450
layout(location = 0) out vec4 outColor;
void main() {
  /* Cheap but incompressible: the colour varies every pixel, so the attachment
   * traffic cannot be folded away by PowerVR's frame-buffer compression. */
  vec2 p = gl_FragCoord.xy;
  outColor = vec4(fract(p.x * 0.017), fract(p.y * 0.013), fract((p.x + p.y) * 0.007), 1.0);
}
