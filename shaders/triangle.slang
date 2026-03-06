// ============================================================================
// Triangle Shader - Slang Version
// ============================================================================
// This is a unified shader file containing both vertex and fragment entry points.
// Slang allows multiple entry points in a single file.
// ============================================================================
//
// SDL3 HLSL Resource Layout (from SDL_gpu.h):
// For pixel shaders:
// - (t[n], space2): Sampled textures
// - (s[n], space2): Samplers with indices corresponding to sampled textures
// - (b[n], space3): Uniform buffers
//
// For Vulkan, this translates to:
// - Set 2, Binding N: Combined texture-samplers
// ============================================================================

// Texture and sampler bindings for SDL3
// SDL3 expects fragment samplers in space2 (Vulkan set 2)
// Using separate texture and sampler to match SDL3's expected layout
[[vk::binding(0, 2)]] Texture2D texture0 : register(t0, space2);
[[vk::binding(0, 2)]] SamplerState sampler0 : register(s0, space2);

// ============================================================================
// Vertex Shader
// ============================================================================

struct VSInput {
    float2 position : TEXCOORD0;
    float2 texcoord : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = input.texcoord;
    output.color = input.color;
    return output;
}

// ============================================================================
// Fragment Shader
// ============================================================================

struct PSInput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR;
};

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET {
    float4 texColor = texture0.Sample(sampler0, input.texcoord);
    return texColor * input.color;
}
