cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};

struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  const float wave = 0.48 + 0.52 * sin(igrViewportAndTime.z * 1.7 + input.uv.x * 5.2 + igrParam0.x * 0.05);
  const float stripe = smoothstep(0.0, 1.0, input.uv.y) * 0.35;
  const float4 surface = float4(0.10 + input.uv.x * 0.12, 0.20 + stripe, 0.42 + wave * 0.38, 0.82);
  return surface * igrTint * input.color;
}