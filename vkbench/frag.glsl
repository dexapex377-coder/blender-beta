#version 450
layout(location = 0) out vec4 outColor;
void main() {
  vec4 c = vec4(0.0);
  for (int i = 0; i < 64; i++) {
    float x = float(gl_FragCoord.x) + float(i) * 0.5;
    float y = float(gl_FragCoord.y) + float(i) * 0.3;
    c += sin(vec4(x, y, x * y, x + y));
  }
  outColor = c * 0.01;
}
