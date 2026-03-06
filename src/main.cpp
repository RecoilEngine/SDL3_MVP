#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <string>
#include <stdlib.h>

#include "shader_module.hpp"

// ============================================================================
// Configuration
// ============================================================================
constexpr int WINDOW_WIDTH  = 800;
constexpr int WINDOW_HEIGHT = 600;

constexpr float COLOR_BLUE[4]   = {0.0f, 0.4f, 0.8f, 1.0f};
constexpr float COLOR_ORANGE[4] = {1.0f, 0.5f, 0.0f, 1.0f};

// ============================================================================
// Vertex Data Structure
// ============================================================================
struct Vertex {
    float position[2];
    float texcoord[2];
    float color[4];
};

// ============================================================================
// Helper Functions
// ============================================================================
inline void lerpColor(float result[4], const float a[4], const float b[4], float t) {
    result[0] = a[0] + (b[0] - a[0]) * t;
    result[1] = a[1] + (b[1] - a[1]) * t;
    result[2] = a[2] + (b[2] - a[2]) * t;
    result[3] = a[3] + (b[3] - a[3]) * t;
}

std::vector<uint8_t> loadShaderFile(const char* filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        SDL_Log("Failed to open shader file: %s", filename);
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        SDL_Log("Failed to read shader file: %s", filename);
        return {};
    }
    return buffer;
}

// ============================================================================
// GPU Backend Selection
// ============================================================================
enum class GPUBackend { AUTO, VULKAN, D3D12 };

struct AppSettings {
    GPUBackend backend    = GPUBackend::AUTO;
    bool       hdrEnabled = false;
};

bool isRunningUnderRenderdoc() {
    return getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr;;
}

AppSettings parseCommandLine(int argc, char* argv[]) {
    AppSettings settings;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--vulkan") == 0) {
            settings.backend = GPUBackend::VULKAN;
        } else if (strcmp(argv[i], "--d3d12") == 0 || strcmp(argv[i], "--dx12") == 0) {
            settings.backend = GPUBackend::D3D12;
        } else if (strcmp(argv[i], "--hdr") == 0) {
            settings.hdrEnabled = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            SDL_Log("Usage: %s [--vulkan|--d3d12] [--hdr]", argv[0]);
            SDL_Log("  --vulkan  : Use Vulkan backend with SPIR-V shaders");
            SDL_Log("  --d3d12   : Use D3D12 backend with DXIL shaders (Windows only)");
            SDL_Log("  --hdr     : Start in HDR mode (cycle modes with F2)");
            SDL_Log("  (no args) : Auto-select backend");
        }
    }
    return settings;
}

// Helper to apply swapchain parameters safely, draining the GPU
// so SDL's internal semaphore pool is clean before swapchain recreation.
static bool setSwapchainParamsSafe(
    SDL_GPUDevice* device,
    SDL_Window* window,
    SDL_GPUSwapchainComposition composition,
    SDL_GPUPresentMode          presentMode)
{
    bool success = SDL_SetGPUSwapchainParameters(device, window, composition, presentMode);
    if (success) {
        // Drain all in-flight GPU/presentation work. This is intentional and
        // correct here: swapchain recreation must not race with semaphore use.
        SDL_WaitForGPUIdle(device);
    }
    return success;
}

// Query available HDR swapchain compositions and return the best available one.
// Priority: HDR10_ST2084 > HDR_EXTENDED_LINEAR > SDR (if none available)
static SDL_GPUSwapchainComposition queryBestHDRComposition(
    SDL_GPUDevice* device,
    SDL_Window* window)
{
    bool hdr10Supported = SDL_WindowSupportsGPUSwapchainComposition(device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
    bool hdrExtendedSupported = SDL_WindowSupportsGPUSwapchainComposition(device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);

    SDL_Log("=== HDR Evaluation Results ===");
    SDL_Log("HDR10 ST2084 (PQ):       %s", hdr10Supported ? "AVAILABLE" : "NOT AVAILABLE");
    SDL_Log("HDR Extended Linear:     %s", hdrExtendedSupported ? "AVAILABLE" : "NOT AVAILABLE");
    SDL_Log("SDR:                     AVAILABLE (fallback)");
    SDL_Log("===========================");

    // Check HDR10 ST2084 first (PQ, higher peak brightness)
    if (hdr10Supported) {
        SDL_Log("Selected: HDR10 ST2084 (PQ) - Highest quality HDR");
        return SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
    }

    // Check HDR Extended Linear (scRGB, wider color gamut, extended range)
    if (hdrExtendedSupported) {
        SDL_Log("Selected: HDR Extended Linear (scRGB) - Wide color gamut with extended range");
        return SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
    }

    // No HDR available
    SDL_Log("Selected: SDR - No HDR modes available");
    return SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
}

// Get the texture format for a given swapchain composition
static SDL_GPUTextureFormat getColorTargetFormatForComposition(
    SDL_GPUSwapchainComposition composition)
{
    switch (composition) {
        case SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084:
            // R10G10B10A2_UNORM is the standard format for HDR10 PQ
            return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
        case SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR:
            // R16G16B16A16_FLOAT is typically used for HDR Extended Linear (scRGB)
            return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        case SDL_GPU_SWAPCHAINCOMPOSITION_SDR:
        case SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR:
        default:
            // SDR uses standard RGBA8
            return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}

// ============================================================================
// Main Application
// ============================================================================
int main(int argc, char* argv[]) {
    AppSettings settings = parseCommandLine(argc, argv);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "SDL3 GPU MVP - Textured Rectangle",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // ------------------------------------------------------------------------
    // Create GPU Device
    // For AUTO mode, advertise all shader formats so SDL can pick any backend.
    // We resolve the actual format AFTER creation by querying the driver name.
    // ------------------------------------------------------------------------
    const char* requestedDriverName = nullptr;
    SDL_GPUShaderFormat requestedFormats;

    switch (settings.backend) {
        case GPUBackend::VULKAN:
            requestedDriverName = "vulkan";
            requestedFormats    = SDL_GPU_SHADERFORMAT_SPIRV;
            break;
        case GPUBackend::D3D12:
            requestedDriverName = "direct3d12";
            requestedFormats    = SDL_GPU_SHADERFORMAT_DXIL;
            break;
        case GPUBackend::AUTO:
        default:
            // Offer everything; SDL picks the backend
            requestedFormats = SDL_GPU_SHADERFORMAT_SPIRV |
                               SDL_GPU_SHADERFORMAT_DXBC  |
                               SDL_GPU_SHADERFORMAT_DXIL;
            break;
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(requestedFormats, !isRunningUnderRenderdoc(), requestedDriverName);
    if (!device) {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // ------------------------------------------------------------------------
    // FIX: Detect actual backend AFTER device creation and set shader
    // format/extension accordingly. In AUTO mode the original code always
    // defaulted to .spv even when SDL picked D3D12, which would fail to load
    // shaders entirely.
    // ------------------------------------------------------------------------
    SDL_GPUShaderFormat shaderFormat;
    const char*         shaderFormatName;
    ShaderTarget        shaderTarget;  // For Slang compiler

    const char* detectedDriver = SDL_GetGPUDeviceDriver(device);
    SDL_Log("GPU Device Driver: %s", detectedDriver);

    if (strcmp(detectedDriver, "direct3d12") == 0) {
        shaderFormat     = SDL_GPU_SHADERFORMAT_DXIL;
        shaderFormatName = "DXIL";
        shaderTarget     = ShaderTarget::DXIL;
    } else {
        // Vulkan (or any other SPIR-V capable backend such as Metal via MoltenVK)
        shaderFormat     = SDL_GPU_SHADERFORMAT_SPIRV;
        shaderFormatName = "SPIR-V";
        shaderTarget     = ShaderTarget::SPIRV;
    }
    SDL_Log("Shader Format: %s", shaderFormatName);

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        SDL_Log("Failed to claim window for GPU: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // ------------------------------------------------------------------------
    // Load Texture
    // ------------------------------------------------------------------------
    SDL_Surface* surface = SDL_LoadBMP("assets/placeholder.bmp");
    if (!surface) {
        SDL_Log("Failed to load texture: %s, using fallback checkerboard", SDL_GetError());
        surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA8888);
        if (surface) {
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    bool   white = ((x + y) % 2) == 0;
                    Uint32 color = white ? 0xFFFFFFFF : 0x000000FF;
                    SDL_WriteSurfacePixel(surface, x, y,
                        (color >> 24) & 0xFF,
                        (color >> 16) & 0xFF,
                        (color >>  8) & 0xFF,
                        (color      ) & 0xFF
                    );
                }
            }
        }
    }

    if (!surface) {
        SDL_Log("Failed to create fallback surface");
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Log("Loaded texture: %dx%d", surface->w, surface->h);

    SDL_Surface* rgbaSurface = surface;
    if (surface->format != SDL_PIXELFORMAT_RGBA8888) {
        rgbaSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        if (!rgbaSurface) {
            SDL_Log("Failed to convert surface: %s", SDL_GetError());
            SDL_DestroySurface(surface);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    SDL_GPUTexture*        texture               = nullptr;
    SDL_GPUTransferBuffer* textureTransferBuffer = nullptr;

    {
        SDL_GPUTextureCreateInfo textureInfo = {};
        textureInfo.type                  = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format                = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.width                 = static_cast<Uint32>(rgbaSurface->w);
        textureInfo.height                = static_cast<Uint32>(rgbaSurface->h);
        textureInfo.layer_count_or_depth  = 1;
        textureInfo.num_levels            = 1;
        textureInfo.usage                 = SDL_GPU_TEXTUREUSAGE_SAMPLER;

        texture = SDL_CreateGPUTexture(device, &textureInfo);
        if (!texture) {
            SDL_Log("Failed to create GPU texture: %s", SDL_GetError());
            if (rgbaSurface != surface) SDL_DestroySurface(rgbaSurface);
            SDL_DestroySurface(surface);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        size_t textureSize = static_cast<size_t>(rgbaSurface->w) * rgbaSurface->h * 4;
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size  = static_cast<Uint32>(textureSize);

        textureTransferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!textureTransferBuffer) {
            SDL_Log("Failed to create texture transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUTexture(device, texture);
            if (rgbaSurface != surface) SDL_DestroySurface(rgbaSurface);
            SDL_DestroySurface(surface);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        void* mapPtr = SDL_MapGPUTransferBuffer(device, textureTransferBuffer, false);
        if (mapPtr) {
            std::memcpy(mapPtr, rgbaSurface->pixels, textureSize);
            SDL_UnmapGPUTransferBuffer(device, textureTransferBuffer);
        }

        SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
        if (cmdBuf) {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);

            SDL_GPUTextureTransferInfo srcInfo = {};
            srcInfo.transfer_buffer = textureTransferBuffer;
            srcInfo.offset          = 0;

            SDL_GPUTextureRegion dstRegion = {};
            dstRegion.texture = texture;
            dstRegion.w       = static_cast<Uint32>(rgbaSurface->w);
            dstRegion.h       = static_cast<Uint32>(rgbaSurface->h);
            dstRegion.d       = 1;

            SDL_UploadToGPUTexture(copyPass, &srcInfo, &dstRegion, false);
            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmdBuf);
        }

        SDL_ReleaseGPUTransferBuffer(device, textureTransferBuffer);
    }

    if (rgbaSurface != surface) SDL_DestroySurface(rgbaSurface);
    SDL_DestroySurface(surface);

    // Sampler
    SDL_GPUSampler* sampler = nullptr;
    {
        SDL_GPUSamplerCreateInfo samplerInfo = {};
        samplerInfo.min_filter      = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter      = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

        sampler = SDL_CreateGPUSampler(device, &samplerInfo);
        if (!sampler) {
            SDL_Log("Failed to create sampler: %s", SDL_GetError());
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // ------------------------------------------------------------------------
    // Compile Shaders with Slang (Runtime Compilation)
    // ------------------------------------------------------------------------
    SDL_GPUShader* vertexShader   = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    {
        // Initialize Slang compiler
        SlangCompiler slangCompiler;
        if (!slangCompiler.initialize()) {
            SDL_Log("Failed to initialize Slang compiler");
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Detect shader target from detected driver
        ShaderTarget shaderTarget = SlangCompiler::detectTargetFromDriver(detectedDriver);
        SDL_Log("Slang target: %s",
            shaderTarget == ShaderTarget::DXIL ? "DXIL" :
            shaderTarget == ShaderTarget::Metal ? "Metal" : "SPIR-V");

        // Compile vertex shader from Slang source
        CompiledShader vertCompiled = slangCompiler.compileShader(
            "shaders/triangle.slang",
            "vertexMain",
            shaderTarget,
            SDL_GPU_SHADERSTAGE_VERTEX
        );

        if (!vertCompiled.isValid()) {
            SDL_Log("Failed to compile vertex shader: %s", vertCompiled.errorMessage.c_str());
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Create vertex shader from compiled code
        SDL_GPUShaderCreateInfo vertShaderInfo = {};
        vertShaderInfo.code               = vertCompiled.code.data();
        vertShaderInfo.code_size          = vertCompiled.code.size();
        vertShaderInfo.entrypoint         = vertCompiled.entryPoint.c_str();
        vertShaderInfo.format             = vertCompiled.format;
        vertShaderInfo.stage              = SDL_GPU_SHADERSTAGE_VERTEX;
        vertShaderInfo.num_samplers       = 0;
        vertShaderInfo.num_uniform_buffers = 0;

        vertexShader = SDL_CreateGPUShader(device, &vertShaderInfo);
        if (!vertexShader) {
            SDL_Log("Failed to create vertex shader: %s", SDL_GetError());
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Compile fragment shader from Slang source
        CompiledShader fragCompiled = slangCompiler.compileShader(
            "shaders/triangle.slang",
            "fragmentMain",
            shaderTarget,
            SDL_GPU_SHADERSTAGE_FRAGMENT
        );

        if (!fragCompiled.isValid()) {
            SDL_Log("Failed to compile fragment shader: %s", fragCompiled.errorMessage.c_str());
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Create fragment shader from compiled code
        SDL_GPUShaderCreateInfo fragShaderInfo = {};
        fragShaderInfo.code               = fragCompiled.code.data();
        fragShaderInfo.code_size          = fragCompiled.code.size();
        fragShaderInfo.entrypoint         = fragCompiled.entryPoint.c_str();
        fragShaderInfo.format             = fragCompiled.format;
        fragShaderInfo.stage              = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragShaderInfo.num_samplers       = 1;
        fragShaderInfo.num_uniform_buffers = 0;

        fragmentShader = SDL_CreateGPUShader(device, &fragShaderInfo);
        if (!fragmentShader) {
            SDL_Log("Failed to create fragment shader: %s", SDL_GetError());
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // ------------------------------------------------------------------------
    // Create Graphics Pipelines
    //
    // FIX: Both pipelines are created BEFORE SDL_SetGPUSwapchainParameters is
    // ever called. This guarantees:
    //   - pipelineSDR is built from the real SDR swapchain format (e.g.
    //     B8G8R8A8_UNORM), not from whatever format HDR might switch it to.
    //   - pipelineHDR is hardcoded to R10G10B10A2_UNORM, which is the
    //     format SDL3 defines for HDR10 ST2084 on both Vulkan and DX12.
    // ------------------------------------------------------------------------
    SDL_GPUGraphicsPipeline* pipelineSDR = nullptr;
    SDL_GPUGraphicsPipeline* pipelineHDR = nullptr;
    SDL_GPUGraphicsPipeline* pipelineHDRExtended = nullptr;

    {
        SDL_GPUVertexAttribute vertexAttributes[3] = {};
        vertexAttributes[0].location    = 0;
        vertexAttributes[0].buffer_slot = 0;
        vertexAttributes[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        vertexAttributes[0].offset      = 0;

        vertexAttributes[1].location    = 1;
        vertexAttributes[1].buffer_slot = 0;
        vertexAttributes[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        vertexAttributes[1].offset      = sizeof(float) * 2;

        vertexAttributes[2].location    = 2;
        vertexAttributes[2].buffer_slot = 0;
        vertexAttributes[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        vertexAttributes[2].offset      = sizeof(float) * 4;

        SDL_GPUVertexBufferDescription vertexBufferDesc = {};
        vertexBufferDesc.slot               = 0;
        vertexBufferDesc.pitch              = sizeof(Vertex);
        vertexBufferDesc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vertexBufferDesc.instance_step_rate = 0;

        SDL_GPUVertexInputState vertexInputState = {};
        vertexInputState.num_vertex_buffers          = 1;
        vertexInputState.vertex_buffer_descriptions  = &vertexBufferDesc;
        vertexInputState.num_vertex_attributes       = 3;
        vertexInputState.vertex_attributes           = vertexAttributes;

        SDL_GPURasterizerState rasterizerState = {};
        rasterizerState.fill_mode  = SDL_GPU_FILLMODE_FILL;
        rasterizerState.cull_mode  = SDL_GPU_CULLMODE_NONE;
        rasterizerState.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;

        SDL_GPUColorTargetBlendState blendState = {};
        blendState.enable_blend      = false;
        blendState.color_write_mask  = SDL_GPU_COLORCOMPONENT_R |
                                       SDL_GPU_COLORCOMPONENT_G |
                                       SDL_GPU_COLORCOMPONENT_B |
                                       SDL_GPU_COLORCOMPONENT_A;

        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.vertex_shader                         = vertexShader;
        pipelineInfo.fragment_shader                       = fragmentShader;
        pipelineInfo.vertex_input_state                    = vertexInputState;
        pipelineInfo.primitive_type                        = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipelineInfo.rasterizer_state                      = rasterizerState;
        pipelineInfo.target_info.num_color_targets         = 1;
        pipelineInfo.target_info.depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_INVALID;
        pipelineInfo.target_info.has_depth_stencil_target  = false;

        // --- SDR pipeline ---
        // Query swapchain format NOW, before HDR has been touched.
        // This is guaranteed to return the real SDR format.
        {
            SDL_GPUColorTargetDescription colorTargetDesc = {};
            colorTargetDesc.format      = SDL_GetGPUSwapchainTextureFormat(device, window);
            colorTargetDesc.blend_state = blendState;
            pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;

            pipelineSDR = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
            if (!pipelineSDR) {
                SDL_Log("Failed to create SDR pipeline: %s", SDL_GetError());
                SDL_ReleaseGPUShader(device, fragmentShader);
                SDL_ReleaseGPUShader(device, vertexShader);
                SDL_ReleaseGPUSampler(device, sampler);
                SDL_ReleaseGPUTexture(device, texture);
                SDL_ReleaseWindowFromGPUDevice(device, window);
                SDL_DestroyGPUDevice(device);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            SDL_Log("Created SDR pipeline with format: %d", (int)colorTargetDesc.format);
        }

        // --- HDR pipeline ---
        // R10G10B10A2_UNORM is the format SDL3 assigns to HDR10 ST2084
        // on all backends. No runtime query needed here.
        {
            SDL_GPUColorTargetDescription colorTargetDesc = {};
            colorTargetDesc.format      = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
            colorTargetDesc.blend_state = blendState;
            pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;

            pipelineHDR = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
            if (!pipelineHDR) {
                SDL_Log("Failed to create HDR pipeline: %s", SDL_GetError());
                SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
                SDL_ReleaseGPUShader(device, fragmentShader);
                SDL_ReleaseGPUShader(device, vertexShader);
                SDL_ReleaseGPUSampler(device, sampler);
                SDL_ReleaseGPUTexture(device, texture);
                SDL_ReleaseWindowFromGPUDevice(device, window);
                SDL_DestroyGPUDevice(device);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            SDL_Log("Created HDR pipeline with format: R10G10B10A2_UNORM");
        }

        // --- HDR Extended Linear pipeline ---
        // R16G16B16A16_FLOAT is the format for HDR Extended Linear (scRGB)
        {
            SDL_GPUColorTargetDescription colorTargetDesc = {};
            colorTargetDesc.format      = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            colorTargetDesc.blend_state = blendState;
            pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;

            pipelineHDRExtended = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
            if (!pipelineHDRExtended) {
                SDL_Log("Failed to create HDR Extended Linear pipeline: %s", SDL_GetError());
                SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
                SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
                SDL_ReleaseGPUShader(device, fragmentShader);
                SDL_ReleaseGPUShader(device, vertexShader);
                SDL_ReleaseGPUSampler(device, sampler);
                SDL_ReleaseGPUTexture(device, texture);
                SDL_ReleaseWindowFromGPUDevice(device, window);
                SDL_DestroyGPUDevice(device);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            SDL_Log("Created HDR Extended Linear pipeline with format: R16G16B16A16_FLOAT");
        }
    }

    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // ------------------------------------------------------------------------
    // Configure HDR Swapchain (AFTER pipeline creation)
    //
    // FIX: HDR setup is now deferred until after both pipelines exist.
    // FIX: SDL_WindowSupportsGPUSwapchainComposition is checked first.
    //      On DX12 this fails when the display doesn't support the color space
    //      or Windows HDR hasn't been enabled in Display Settings.
    // ------------------------------------------------------------------------
    bool hdrEnabled = false;
    SDL_GPUSwapchainComposition currentHDRComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

    if (settings.hdrEnabled) {
        // Query available HDR modes and get the best one
        SDL_GPUSwapchainComposition bestHDR = queryBestHDRComposition(device, window);

        if (bestHDR == SDL_GPU_SWAPCHAINCOMPOSITION_SDR) {
            SDL_Log("HDR not supported on this display/system.");
            SDL_Log("On Windows: ensure HDR is enabled in Settings -> Display -> HDR.");
        } else {
            // Get the appropriate color target format for this composition
            SDL_GPUTextureFormat colorFormat = getColorTargetFormatForComposition(bestHDR);

            bool success = setSwapchainParamsSafe(device, window,
                bestHDR,
                SDL_GPU_PRESENTMODE_VSYNC);
            if (success) {
                hdrEnabled = true;
                currentHDRComposition = bestHDR;
                const char* modeName = (bestHDR == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084)
                    ? "HDR10 ST2084"
                    : "HDR Extended Linear";
                SDL_Log("HDR mode enabled: %s (Color format: %u)", modeName, colorFormat);
            } else {
                SDL_Log("HDR setup failed despite support check: %s", SDL_GetError());
            }
        }
    }

    // ------------------------------------------------------------------------
    // Create Static Index Buffer
    // ------------------------------------------------------------------------
    SDL_GPUBuffer* indexBuffer = nullptr;

    {
        const uint16_t indices[] = {0, 1, 2, 0, 2, 3};

        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        bufferInfo.size  = sizeof(indices);

        indexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);
        if (!indexBuffer) {
            SDL_Log("Failed to create index buffer: %s", SDL_GetError());
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size  = sizeof(indices);

        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!transferBuffer) {
            SDL_Log("Failed to create index transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUBuffer(device, indexBuffer);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        void* mapPtr = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        if (mapPtr) {
            std::memcpy(mapPtr, indices, sizeof(indices));
            SDL_UnmapGPUTransferBuffer(device, transferBuffer);
        }

        SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
        if (cmdBuf) {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);

            SDL_GPUTransferBufferLocation src = {};
            src.transfer_buffer = transferBuffer;
            src.offset          = 0;

            SDL_GPUBufferRegion dst = {};
            dst.buffer = indexBuffer;
            dst.offset = 0;
            dst.size   = sizeof(indices);

            SDL_UploadToGPUBuffer(copyPass, &src, &dst, false);
            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmdBuf);
        }

        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    // ------------------------------------------------------------------------
    // Create Dynamic Vertex Buffer
    // ------------------------------------------------------------------------
    SDL_GPUBuffer*         vertexBuffer         = nullptr;
    SDL_GPUTransferBuffer* vertexTransferBuffer = nullptr;
    const size_t           vertexBufferSize     = sizeof(Vertex) * 4;
    std::vector<Vertex>    cpuVertexData(4);

    {
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size  = static_cast<Uint32>(vertexBufferSize);

        vertexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);
        if (!vertexBuffer) {
            SDL_Log("Failed to create vertex buffer: %s", SDL_GetError());
            SDL_ReleaseGPUBuffer(device, indexBuffer);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDRExtended);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size  = static_cast<Uint32>(vertexBufferSize);

        vertexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!vertexTransferBuffer) {
            SDL_Log("Failed to create vertex transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUBuffer(device, vertexBuffer);
            SDL_ReleaseGPUBuffer(device, indexBuffer);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
            SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDRExtended);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    SDL_Log("SDL3 GPU MVP initialized successfully!");
    SDL_Log("Controls: F1=Toggle VSync | F2=Cycle HDR Modes | ESC=Quit");
    const char* modeStr;
    if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084) {
        modeStr = "HDR10 PQ";
    } else if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) {
        modeStr = "HDR Extended";
    } else {
        modeStr = "SDR";
    }
    SDL_Log("Mode: %s", modeStr);

    // ------------------------------------------------------------------------
    // Render Loop
    // ------------------------------------------------------------------------
    bool   running      = true;
    Uint64 startTime    = SDL_GetTicks();
    Uint64 fpsLastTime  = SDL_GetTicks();
    Uint32 frameCount   = 0;
    bool   vsyncEnabled = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        running = false;
                    }
                    else if (event.key.key == SDLK_F1) {
                        vsyncEnabled = !vsyncEnabled;
                        SDL_GPUSwapchainComposition composition = hdrEnabled
                            ? currentHDRComposition
                            : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
                        SDL_GPUPresentMode presentMode = vsyncEnabled
                            ? SDL_GPU_PRESENTMODE_VSYNC
                            : SDL_GPU_PRESENTMODE_IMMEDIATE;
                        setSwapchainParamsSafe(device, window, composition, presentMode);
                        SDL_Log("VSync %s", vsyncEnabled ? "enabled" : "disabled");
                    }
                    else if (event.key.key == SDLK_F2) {
                        // Cycle through modes: SDR -> HDR10 ST2084 -> HDR Extended Linear -> SDR
                        SDL_GPUSwapchainComposition nextMode;
                        const char* nextModeName = nullptr;

                        // Check which HDR modes are supported
                        bool hdr10Supported = SDL_WindowSupportsGPUSwapchainComposition(device, window,
                            SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
                        bool hdrExtendedSupported = SDL_WindowSupportsGPUSwapchainComposition(device, window,
                            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);

                        if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR) {
                            // Currently SDR, try HDR10 ST2084 first
                            if (hdr10Supported) {
                                nextMode = SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
                                nextModeName = "HDR10 ST2084 (PQ)";
                            } else if (hdrExtendedSupported) {
                                nextMode = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
                                nextModeName = "HDR Extended Linear";
                            } else {
                                SDL_Log("No HDR modes available. On Windows, enable HDR in Settings -> Display -> HDR.");
                                continue;
                            }
                        } else if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084) {
                            // Currently HDR10, try HDR Extended Linear next
                            if (hdrExtendedSupported) {
                                nextMode = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
                                nextModeName = "HDR Extended Linear";
                            } else {
                                // Fall back to SDR
                                nextMode = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
                                nextModeName = "SDR";
                            }
                        } else {
                            // Currently HDR Extended Linear (or any other HDR), go to SDR
                            nextMode = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
                            nextModeName = "SDR";
                        }

                        // Apply the next mode
                        bool success = setSwapchainParamsSafe(device, window,
                            nextMode,
                            vsyncEnabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
                        if (success) {
                            currentHDRComposition = nextMode;
                            hdrEnabled = (nextMode != SDL_GPU_SWAPCHAINCOMPOSITION_SDR);
                            SDL_Log("Mode switched to: %s", nextModeName);
                        } else {
                            SDL_Log("Failed to switch to %s: %s", nextModeName, SDL_GetError());
                        }
                    }
                    break;

                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_MOVED: {
                    // Swapchain will be recreated on next acquire.
                    // Drain the GPU now so the presentation engine releases
                    // all semaphores before SDL reuses them.
                    SDL_WaitForGPUIdle(device);
                } break;
            }
        }

        // Update vertex data
        {
            float time   = static_cast<float>(SDL_GetTicks() - startTime) / 1000.0f;
            float offset = std::sin(time) * 0.05f;

            float tintColor[4];
            float t = (std::sin(time * 0.5f) + 1.0f) * 0.5f;
            lerpColor(tintColor, COLOR_BLUE, COLOR_ORANGE, t);

            float hdrScale = hdrEnabled ? 2.0f : 1.0f;

            cpuVertexData[0] = Vertex{{-1.0f + offset, -1.0f + offset}, {0.0f, 0.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[1] = Vertex{{ 1.0f - offset, -1.0f + offset}, {1.0f, 0.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[2] = Vertex{{ 1.0f - offset,  1.0f - offset}, {1.0f, 1.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[3] = Vertex{{-1.0f + offset,  1.0f - offset}, {0.0f, 1.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};

            void* mapPtr = SDL_MapGPUTransferBuffer(device, vertexTransferBuffer, true);
            if (mapPtr) {
                std::memcpy(mapPtr, cpuVertexData.data(), vertexBufferSize);
                SDL_UnmapGPUTransferBuffer(device, vertexTransferBuffer);
            }
        }

        // FIX: SDL_WaitForGPUIdle removed from here. Calling it every frame
        // serialises CPU and GPU, destroying pipelining. SDL3 handles
        // swapchain synchronisation internally. It belongs only at shutdown.

        SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
        if (!cmdBuf) {
            SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_GPUTexture* swapchainTexture = nullptr;
        Uint32          swapchainWidth, swapchainHeight;

        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdBuf, window,
                &swapchainTexture, &swapchainWidth, &swapchainHeight)) {
            SDL_Log("Failed to acquire swapchain texture: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmdBuf);
            continue;
        }

        if (!swapchainTexture) {
            // Swapchain was just recreated or window is not ready.
            // Drain so next acquire starts with clean semaphores.
            SDL_WaitForGPUIdle(device);
            SDL_CancelGPUCommandBuffer(cmdBuf);
            continue;
        }

        // Upload vertex data
        {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);

            SDL_GPUTransferBufferLocation vertSrc = {};
            vertSrc.transfer_buffer = vertexTransferBuffer;
            vertSrc.offset          = 0;

            SDL_GPUBufferRegion vertDst = {};
            vertDst.buffer = vertexBuffer;
            vertDst.offset = 0;
            vertDst.size   = static_cast<Uint32>(vertexBufferSize);

            SDL_UploadToGPUBuffer(copyPass, &vertSrc, &vertDst, false);
            SDL_EndGPUCopyPass(copyPass);
        }

        // FIX: Select pipeline from the ACTUAL current swapchain format rather
        // than the hdrEnabled bool. SDL_SetGPUSwapchainParameters may defer
        // swapchain recreation, so the format in-flight can lag the desired
        // state by one or more frames. Matching on the real format prevents
        // the Vulkan VUID-vkCmdDrawIndexed-renderPass-02684 validation error.
        SDL_GPUGraphicsPipeline* currentPipeline;
        {
            SDL_GPUTextureFormat currentFmt = SDL_GetGPUSwapchainTextureFormat(device, window);
            if (currentFmt == SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM) {
                currentPipeline = pipelineHDR;  // HDR10 ST2084
            } else if (currentFmt == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
                currentPipeline = pipelineHDRExtended;  // HDR Extended Linear
            } else {
                currentPipeline = pipelineSDR;  // SDR (any other format)
            }
        }

        SDL_GPUColorTargetInfo colorTarget = {};
        colorTarget.texture     = swapchainTexture;
        colorTarget.clear_color = SDL_FColor{0.1f, 0.1f, 0.1f, 1.0f};
        colorTarget.load_op     = SDL_GPU_LOADOP_CLEAR;
        colorTarget.store_op    = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdBuf, &colorTarget, 1, nullptr);
        if (renderPass) {
            SDL_BindGPUGraphicsPipeline(renderPass, currentPipeline);

            SDL_GPUBufferBinding vertexBinding = {};
            vertexBinding.buffer = vertexBuffer;
            vertexBinding.offset = 0;
            SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

            SDL_GPUBufferBinding indexBinding = {};
            indexBinding.buffer = indexBuffer;
            indexBinding.offset = 0;
            SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

            SDL_GPUTextureSamplerBinding texSamplerBinding = {};
            texSamplerBinding.texture = texture;
            texSamplerBinding.sampler = sampler;
            SDL_BindGPUFragmentSamplers(renderPass, 0, &texSamplerBinding, 1);

            SDL_DrawGPUIndexedPrimitives(renderPass, 6, 1, 0, 0, 0);
            SDL_EndGPURenderPass(renderPass);
        }

        SDL_SubmitGPUCommandBuffer(cmdBuf);

        // FPS counter
        frameCount++;
        Uint64 now     = SDL_GetTicks();
        Uint64 elapsed = now - fpsLastTime;
        if (elapsed >= 1000) {
            float fps = static_cast<float>(frameCount) * 1000.0f / static_cast<float>(elapsed);
            char title[128];
            const char* modeStr;
            if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084) {
                modeStr = "HDR10 PQ";
            } else if (currentHDRComposition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) {
                modeStr = "HDR Extended";
            } else {
                modeStr = "SDR";
            }
            SDL_snprintf(title, sizeof(title),
                "SDL3 GPU MVP - %.1f FPS | VSync: %s (F1) | Mode: %s (F2)",
                fps,
                vsyncEnabled ? "ON" : "OFF",
                modeStr);
            SDL_SetWindowTitle(window, title);
            frameCount  = 0;
            fpsLastTime = now;
        }
    }

    // ------------------------------------------------------------------------
    // Cleanup
    // FIX: SDL_WaitForGPUIdle belongs HERE, not in the render loop.
    // It ensures all in-flight GPU work is finished before releasing resources.
    // ------------------------------------------------------------------------
    SDL_Log("Shutting down...");
    SDL_WaitForGPUIdle(device);

    SDL_ReleaseGPUTransferBuffer(device, vertexTransferBuffer);
    SDL_ReleaseGPUBuffer(device, vertexBuffer);
    SDL_ReleaseGPUBuffer(device, indexBuffer);
    SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
    SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
    SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDRExtended);
    SDL_ReleaseGPUSampler(device, sampler);
    SDL_ReleaseGPUTexture(device, texture);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
