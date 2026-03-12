#version 450

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D igrTexture0;

layout(std140, set = 0, binding = 1) uniform IgrShaderConstants {
  vec4 igrTint;
  vec4 igrParam0;
  vec4 igrParam1;
  vec4 igrParam2;
  vec4 igrParam3;
  vec4 igrRect;
  vec4 igrViewportAndTime;
  vec4 igrFrameData;
};

void main() {
  vec4 sampled = texture(igrTexture0, inUv);
  float glowEnabled = igrParam0.x;
  float pulse = 0.58 + 0.42 * sin(igrViewportAndTime.z * 2.4 + inUv.x * 6.28318);
  vec3 accent = mix(vec3(0.88, 0.92, 1.0), vec3(0.42, 0.78, 1.0), pulse);
  vec3 shaded = mix(sampled.rgb, sampled.rgb * accent, glowEnabled * 0.65);
  outColor = vec4(shaded, sampled.a) * igrTint * inColor;
}
