Texture2D texture0   : register(t0, space2);  // fragment texture slot
SamplerState sampler0 : register(s0, space2);  // fragment sampler slot

struct PSInput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

float4 main(PSInput input) : SV_TARGET {
    float4 texColor = texture0.Sample(sampler0, input.texcoord);
    return texColor * input.color;
}
