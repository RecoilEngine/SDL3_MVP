#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>

// ============================================================================
// Configuration
// ============================================================================
constexpr int WINDOW_WIDTH = 800;
constexpr int WINDOW_HEIGHT = 600;

// Color drift: Blue -> Orange
constexpr float COLOR_BLUE[4] = {0.0f, 0.4f, 0.8f, 1.0f};
constexpr float COLOR_ORANGE[4] = {1.0f, 0.5f, 0.0f, 1.0f};

// ============================================================================
// Vertex Data Structure
// ============================================================================
struct Vertex {
    float position[2];  // x, y
    float texcoord[2];  // u, v
    float color[4];     // r, g, b, a
};

// ============================================================================
// Helper Functions
// ============================================================================

// Linear interpolation between two colors
inline void lerpColor(float result[4], const float a[4], const float b[4], float t) {
    result[0] = a[0] + (b[0] - a[0]) * t;
    result[1] = a[1] + (b[1] - a[1]) * t;
    result[2] = a[2] + (b[2] - a[2]) * t;
    result[3] = a[3] + (b[3] - a[3]) * t;
}

// Load SPIR-V shader from file
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
enum class GPUBackend {
    AUTO,       // Let SDL choose (default)
    VULKAN,     // Force Vulkan
    D3D12       // Force D3D12 (Windows only)
};

// ============================================================================
// Application Settings (parsed from command line)
// ============================================================================
struct AppSettings {
    GPUBackend backend = GPUBackend::AUTO;
    bool hdrEnabled = false;  // HDR mode flag
};

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
            SDL_Log("  --hdr     : Start in HDR mode (toggle with F2)");
            SDL_Log("  (no args) : Auto-select backend (D3D12 on Windows, Vulkan on Linux)");
        }
    }
    return settings;
}

// ============================================================================
// Main Application
// ============================================================================
int main(int argc, char* argv[]) {
    AppSettings settings = parseCommandLine(argc, argv);

    // --------------------------------------------------------------------
    // SDL Initialization
    // --------------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // --------------------------------------------------------------------
    // Create Window
    // --------------------------------------------------------------------
    SDL_Window* window = SDL_CreateWindow(
        "SDL3 GPU MVP - Textured Rectangle",
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // --------------------------------------------------------------------
    // Create GPU Device with selected backend
    // --------------------------------------------------------------------
    const char* driverName = nullptr;  // nullptr = auto-select
    SDL_GPUShaderFormat shaderFormat;
    const char* backendName = "Auto";
    const char* shaderExtension = ".spv";
    const char* shaderFormatName = "SPIR-V";

    switch (settings.backend) {
        case GPUBackend::VULKAN:
            driverName = "vulkan";
            shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
            backendName = "Vulkan";
            shaderExtension = ".spv";
            shaderFormatName = "SPIR-V";
            break;

        case GPUBackend::D3D12:
            driverName = "direct3d12";
            shaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
            backendName = "D3D12";
            shaderExtension = ".dxil";
            shaderFormatName = "DXIL";
            break;

        case GPUBackend::AUTO:
        default:
            // Support all formats and let SDL choose
            shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL;
            backendName = "Auto";
            shaderExtension = ".spv";  // Default to SPIR-V for auto
            shaderFormatName = "SPIR-V";
            break;
    }

    SDL_Log("Requested GPU Backend: %s", backendName);
    SDL_Log("Shader Format: %s", shaderFormatName);

    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        shaderFormat,
        true,      // debug mode
        driverName // specific driver or nullptr for auto
    );

    if (!device) {
        SDL_Log("Failed to create GPU device: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Log("GPU Device: %s", SDL_GetGPUDeviceDriver(device));

    // Claim window for GPU rendering
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        SDL_Log("Failed to claim window for GPU: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // --------------------------------------------------------------------
    // Configure HDR Swapchain (if requested)
    // --------------------------------------------------------------------
    bool hdrEnabled = settings.hdrEnabled;  // Runtime HDR state (can be toggled)

    if (hdrEnabled) {
        // Try to enable HDR mode
        bool hdrSuccess = SDL_SetGPUSwapchainParameters(device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR,
            SDL_GPU_PRESENTMODE_VSYNC);

        if (hdrSuccess) {
            SDL_Log("HDR mode enabled: HDR Extended Linear");
        } else {
            SDL_Log("HDR mode not available, falling back to SDR: %s", SDL_GetError());
            hdrEnabled = false;
        }
    }

    // --------------------------------------------------------------------
    // Load Texture (BMP file) using SDL_LoadBMP
    // --------------------------------------------------------------------
    SDL_Surface* surface = SDL_LoadBMP("assets/placeholder.bmp");
    if (!surface) {
        SDL_Log("Failed to load texture: %s, using fallback checkerboard", SDL_GetError());
        // Create a simple 8x8 checkerboard fallback surface
        surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA8888);
        if (surface) {
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    bool white = ((x + y) % 2) == 0;
                    Uint32 color = white ? 0xFFFFFFFF : 0x000000FF;
                    SDL_WriteSurfacePixel(surface, x, y,
                        (color >> 24) & 0xFF,
                        (color >> 16) & 0xFF,
                        (color >> 8) & 0xFF,
                        color & 0xFF);
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

    // Convert surface to RGBA format if needed
    SDL_Surface* rgbaSurface = surface;
    if (surface->format != SDL_PIXELFORMAT_RGBA8888) {
        rgbaSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        if (!rgbaSurface) {
            SDL_Log("Failed to convert surface to RGBA: %s", SDL_GetError());
            SDL_DestroySurface(surface);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // Create GPU texture
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUTransferBuffer* textureTransferBuffer = nullptr;

    {
        SDL_GPUTextureCreateInfo textureInfo = {};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.width = static_cast<Uint32>(rgbaSurface->w);
        textureInfo.height = static_cast<Uint32>(rgbaSurface->h);
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

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

        // Create transfer buffer for texture upload
        size_t textureSize = static_cast<size_t>(rgbaSurface->w) * rgbaSurface->h * 4;
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(textureSize);

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

        // Upload texture data
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
            srcInfo.offset = 0;

            SDL_GPUTextureRegion dstRegion = {};
            dstRegion.texture = texture;
            dstRegion.x = 0;
            dstRegion.y = 0;
            dstRegion.z = 0;
            dstRegion.w = static_cast<Uint32>(rgbaSurface->w);
            dstRegion.h = static_cast<Uint32>(rgbaSurface->h);
            dstRegion.d = 1;

            SDL_UploadToGPUTexture(copyPass, &srcInfo, &dstRegion, false);
            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmdBuf);
        }

        SDL_ReleaseGPUTransferBuffer(device, textureTransferBuffer);
    }

    // Clean up surface(s)
    if (rgbaSurface != surface) {
        SDL_DestroySurface(rgbaSurface);
    }
    SDL_DestroySurface(surface);

    // Create texture sampler
    SDL_GPUSampler* sampler = nullptr;
    {
        SDL_GPUSamplerCreateInfo samplerInfo = {};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

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

    // --------------------------------------------------------------------
    // Compile Shaders
    // --------------------------------------------------------------------
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;

    {
        // Load vertex shader with correct extension for selected backend
        std::string vertPath = std::string("shaders/triangle.vert") + shaderExtension;
        std::vector<uint8_t> vertCode = loadShaderFile(vertPath.c_str());
        if (vertCode.empty()) {
            SDL_Log("Failed to load vertex shader from: %s", vertPath.c_str());
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        SDL_GPUShaderCreateInfo vertShaderInfo = {};
        vertShaderInfo.code = vertCode.data();
        vertShaderInfo.code_size = vertCode.size();
        vertShaderInfo.entrypoint = "main";
        vertShaderInfo.format = shaderFormat;
        vertShaderInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vertShaderInfo.num_samplers = 0;
        vertShaderInfo.num_storage_buffers = 0;
        vertShaderInfo.num_storage_textures = 0;
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

        // Load fragment shader with correct extension for selected backend
        std::string fragPath = std::string("shaders/triangle.frag") + shaderExtension;
        std::vector<uint8_t> fragCode = loadShaderFile(fragPath.c_str());
        if (fragCode.empty()) {
            SDL_Log("Failed to load fragment shader from: %s", fragPath.c_str());
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUSampler(device, sampler);
            SDL_ReleaseGPUTexture(device, texture);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        SDL_GPUShaderCreateInfo fragShaderInfo = {};
        fragShaderInfo.code = fragCode.data();
        fragShaderInfo.code_size = fragCode.size();
        fragShaderInfo.entrypoint = "main";
        fragShaderInfo.format = shaderFormat;
        fragShaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragShaderInfo.num_samplers = 1;  // 1 texture sampler
        fragShaderInfo.num_storage_buffers = 0;
        fragShaderInfo.num_storage_textures = 0;
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

    // --------------------------------------------------------------------
    // Create Graphics Pipelines (SDR and HDR)
    // We need separate pipelines because HDR uses a different swapchain format
    // --------------------------------------------------------------------
    SDL_GPUGraphicsPipeline* pipelineSDR = nullptr;
    SDL_GPUGraphicsPipeline* pipelineHDR = nullptr;

    {
        // Vertex input state - now 3 attributes (position, texcoord, color)
        SDL_GPUVertexAttribute vertexAttributes[3] = {};

        // Position attribute
        vertexAttributes[0].location = 0;
        vertexAttributes[0].buffer_slot = 0;
        vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        vertexAttributes[0].offset = 0;

        // Texcoord attribute
        vertexAttributes[1].location = 1;
        vertexAttributes[1].buffer_slot = 0;
        vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        vertexAttributes[1].offset = sizeof(float) * 2;

        // Color attribute
        vertexAttributes[2].location = 2;
        vertexAttributes[2].buffer_slot = 0;
        vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        vertexAttributes[2].offset = sizeof(float) * 4;

        SDL_GPUVertexBufferDescription vertexBufferDesc = {};
        vertexBufferDesc.slot = 0;
        vertexBufferDesc.pitch = sizeof(Vertex);
        vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vertexBufferDesc.instance_step_rate = 0;

        SDL_GPUVertexInputState vertexInputState = {};
        vertexInputState.num_vertex_buffers = 1;
        vertexInputState.vertex_buffer_descriptions = &vertexBufferDesc;
        vertexInputState.num_vertex_attributes = 3;
        vertexInputState.vertex_attributes = vertexAttributes;

        // Primitive type
        SDL_GPUPrimitiveType primitiveType = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        // Rasterizer state
        SDL_GPURasterizerState rasterizerState = {};
        rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
        rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE;
        rasterizerState.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;

        // Blend state
        SDL_GPUColorTargetBlendState blendState = {};
        blendState.enable_blend = false;
        blendState.color_write_mask = SDL_GPU_COLORCOMPONENT_A |
                                       SDL_GPU_COLORCOMPONENT_B |
                                       SDL_GPU_COLORCOMPONENT_G |
                                       SDL_GPU_COLORCOMPONENT_R;

        // Common target info structure
        SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
        targetInfo.num_color_targets = 1;
        targetInfo.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        targetInfo.has_depth_stencil_target = false;

        // Pipeline create info
        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.vertex_shader = vertexShader;
        pipelineInfo.fragment_shader = fragmentShader;
        pipelineInfo.vertex_input_state = vertexInputState;
        pipelineInfo.primitive_type = primitiveType;
        pipelineInfo.rasterizer_state = rasterizerState;
        pipelineInfo.target_info = targetInfo;

        // Create SDR pipeline (uses current swapchain format - should be B8G8R8A8 or similar)
        {
            SDL_GPUColorTargetDescription colorTargetDesc = {};
            colorTargetDesc.format = SDL_GetGPUSwapchainTextureFormat(device, window);
            colorTargetDesc.blend_state = blendState;

            pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;

            pipelineSDR = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
            if (!pipelineSDR) {
                SDL_Log("Failed to create SDR graphics pipeline: %s", SDL_GetError());
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

        // Create HDR pipeline (uses R16G16B16A16_SFLOAT for HDR swapchain)
        {
            SDL_GPUColorTargetDescription colorTargetDesc = {};
            colorTargetDesc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            colorTargetDesc.blend_state = blendState;

            pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;

            pipelineHDR = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
            if (!pipelineHDR) {
                SDL_Log("Failed to create HDR graphics pipeline: %s", SDL_GetError());
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
            SDL_Log("Created HDR pipeline with format: R16G16B16A16_FLOAT");
        }
    }

    // We can release shaders after pipeline creation
    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // --------------------------------------------------------------------
    // Create Static Index Buffer (GPU only) - Rectangle (2 triangles)
    // --------------------------------------------------------------------
    SDL_GPUBuffer* indexBuffer = nullptr;

    {
        // Rectangle indices (2 triangles: 0-1-2 and 0-2-3)
        const uint16_t indices[] = {0, 1, 2, 0, 2, 3};

        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        bufferInfo.size = sizeof(indices);

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

        // Upload index data using transfer buffer
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = sizeof(indices);

        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!transferBuffer) {
            SDL_Log("Failed to create transfer buffer: %s", SDL_GetError());
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

        // Map and copy data
        void* mapPtr = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        if (mapPtr) {
            std::memcpy(mapPtr, indices, sizeof(indices));
            SDL_UnmapGPUTransferBuffer(device, transferBuffer);
        }

        // Upload to GPU
        SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
        if (cmdBuf) {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);

            SDL_GPUTransferBufferLocation src = {};
            src.transfer_buffer = transferBuffer;
            src.offset = 0;

            SDL_GPUBufferRegion dst = {};
            dst.buffer = indexBuffer;
            dst.offset = 0;
            dst.size = sizeof(indices);

            SDL_UploadToGPUBuffer(copyPass, &src, &dst, false);
            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmdBuf);
        }

        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    // --------------------------------------------------------------------
    // Create Dynamic Vertex Buffer (CPU-side staging, GPU-side buffer)
    // --------------------------------------------------------------------
    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* vertexTransferBuffer = nullptr;
    const size_t vertexBufferSize = sizeof(Vertex) * 4;  // 4 vertices for rectangle

    // CPU-side vertex data (will be updated each frame)
    std::vector<Vertex> cpuVertexData(4);

    {
        // Create GPU vertex buffer
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size = static_cast<Uint32>(vertexBufferSize);

        vertexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);
        if (!vertexBuffer) {
            SDL_Log("Failed to create vertex buffer: %s", SDL_GetError());
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

        // Create transfer buffer for dynamic updates
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(vertexBufferSize);

        vertexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!vertexTransferBuffer) {
            SDL_Log("Failed to create vertex transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUBuffer(device, vertexBuffer);
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
    }

    SDL_Log("SDL3 GPU MVP initialized successfully!");
    SDL_Log("Textured rectangle with color drift effect");
    SDL_Log("Color drifts: Blue -> Orange");
    SDL_Log("Controls: F1=Toggle VSync | F2=Toggle HDR | ESC=Quit");
    SDL_Log("HDR mode: %s", hdrEnabled ? "ENABLED (2x brightness)" : "disabled");

    // --------------------------------------------------------------------
    // SDL2-Style Event Loop
    // --------------------------------------------------------------------
    bool running = true;
    Uint64 startTime = SDL_GetTicks();

    // FPS tracking variables
    Uint64 fpsLastTime = SDL_GetTicks();
    Uint32 frameCount = 0;

    // VSync state
    bool vsyncEnabled = true;

    while (running) {
        // Poll events (SDL2-style)
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
                        // Toggle VSync
                        vsyncEnabled = !vsyncEnabled;
                        {
                            SDL_GPUSwapchainComposition composition = hdrEnabled
                                ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR
                                : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
                            SDL_GPUPresentMode presentMode = vsyncEnabled
                                ? SDL_GPU_PRESENTMODE_VSYNC
                                : SDL_GPU_PRESENTMODE_IMMEDIATE;
                            SDL_SetGPUSwapchainParameters(device, window, composition, presentMode);
                        }
                        SDL_Log("VSync %s", vsyncEnabled ? "enabled" : "disabled");
                    }
                    else if (event.key.key == SDLK_F2) {
                        // Toggle HDR mode
                        hdrEnabled = !hdrEnabled;
                        {
                            SDL_GPUSwapchainComposition composition = hdrEnabled
                                ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR
                                : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
                            SDL_GPUPresentMode presentMode = vsyncEnabled
                                ? SDL_GPU_PRESENTMODE_VSYNC
                                : SDL_GPU_PRESENTMODE_IMMEDIATE;
                            bool success = SDL_SetGPUSwapchainParameters(device, window, composition, presentMode);
                            if (!success) {
                                SDL_Log("Failed to toggle HDR mode: %s", SDL_GetError());
                                hdrEnabled = !hdrEnabled;  // Revert state
                            } else {
                                SDL_Log("HDR mode %s", hdrEnabled ? "enabled (HDR Extended Linear)" : "disabled (SDR)");
                            }
                        }
                    }
                    break;

                case SDL_EVENT_WINDOW_RESIZED:
                    // Window resized - swapchain will be recreated automatically
                    break;
            }
        }

        // --------------------------------------------------------------------
        // Update Vertex Buffer (CPU-side, uploaded to GPU each frame)
        // Rectangle with colors that drift from Blue to Orange
        // In HDR mode, colors can exceed 1.0 for extended brightness
        // --------------------------------------------------------------------
        {
            float time = static_cast<float>(SDL_GetTicks() - startTime) / 1000.0f;
            float offset = std::sin(time) * 0.05f;  // Small oscillation

            // Calculate drifted color (tint)
            float tintColor[4];
            float t = (std::sin(time * 0.5f) + 1.0f) * 0.5f;
            lerpColor(tintColor, COLOR_BLUE, COLOR_ORANGE, t);

            // In HDR mode, boost colors to demonstrate extended range
            // HDR allows values > 1.0 for brighter highlights
            float hdrScale = hdrEnabled ? 2.0f : 1.0f;  // 2x brightness in HDR

            // Rectangle vertices covering the window (-1 to 1)
            // 0: top-left, 1: top-right, 2: bottom-right, 3: bottom-left
            cpuVertexData[0] = Vertex{{-1.0f + offset, -1.0f + offset}, {0.0f, 0.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[1] = Vertex{{ 1.0f - offset, -1.0f + offset}, {1.0f, 0.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[2] = Vertex{{ 1.0f - offset,  1.0f - offset}, {1.0f, 1.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};
            cpuVertexData[3] = Vertex{{-1.0f + offset,  1.0f - offset}, {0.0f, 1.0f}, {tintColor[0] * hdrScale, tintColor[1] * hdrScale, tintColor[2] * hdrScale, 1.0f}};

            // Map transfer buffer and copy CPU data
            void* mapPtr = SDL_MapGPUTransferBuffer(device, vertexTransferBuffer, true);
            if (mapPtr) {
                std::memcpy(mapPtr, cpuVertexData.data(), vertexBufferSize);
                SDL_UnmapGPUTransferBuffer(device, vertexTransferBuffer);
            }
        }

        // --------------------------------------------------------------------
        // Wait for previous GPU work to complete to avoid swapchain semaphore issues
        // --------------------------------------------------------------------
        SDL_WaitForGPUIdle(device);

        // --------------------------------------------------------------------
        // Acquire Command Buffer and Swapchain Texture
        // --------------------------------------------------------------------
        SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
        if (!cmdBuf) {
            SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_GPUTexture* swapchainTexture = nullptr;
        Uint32 swapchainWidth, swapchainHeight;

        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdBuf, window, &swapchainTexture, &swapchainWidth, &swapchainHeight)) {
            SDL_Log("Failed to acquire swapchain texture: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmdBuf);
            continue;
        }

        if (!swapchainTexture) {
            // Window minimized or not ready
            SDL_CancelGPUCommandBuffer(cmdBuf);
            continue;
        }

        // --------------------------------------------------------------------
        // Upload Vertex Data
        // --------------------------------------------------------------------
        {
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);

            SDL_GPUTransferBufferLocation vertSrc = {};
            vertSrc.transfer_buffer = vertexTransferBuffer;
            vertSrc.offset = 0;

            SDL_GPUBufferRegion vertDst = {};
            vertDst.buffer = vertexBuffer;
            vertDst.offset = 0;
            vertDst.size = static_cast<Uint32>(vertexBufferSize);

            SDL_UploadToGPUBuffer(copyPass, &vertSrc, &vertDst, false);
            SDL_EndGPUCopyPass(copyPass);
        }

        // --------------------------------------------------------------------
        // Begin Render Pass
        // --------------------------------------------------------------------
        SDL_GPUColorTargetInfo colorTarget = {};
        colorTarget.texture = swapchainTexture;
        colorTarget.clear_color = SDL_FColor{0.1f, 0.1f, 0.1f, 1.0f};  // Dark gray background
        colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTarget.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdBuf, &colorTarget, 1, nullptr);
        if (renderPass) {
            // Bind pipeline
            // Select the correct pipeline based on current HDR mode
            SDL_GPUGraphicsPipeline* currentPipeline = hdrEnabled ? pipelineHDR : pipelineSDR;
            SDL_BindGPUGraphicsPipeline(renderPass, currentPipeline);

            // Bind vertex buffer
            SDL_GPUBufferBinding vertexBinding = {};
            vertexBinding.buffer = vertexBuffer;
            vertexBinding.offset = 0;
            SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

            // Bind index buffer
            SDL_GPUBufferBinding indexBinding = {};
            indexBinding.buffer = indexBuffer;
            indexBinding.offset = 0;
            SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

            // Bind texture and sampler (fragment shader stage)
            SDL_GPUTextureSamplerBinding textureSamplerBinding = {};
            textureSamplerBinding.texture = texture;
            textureSamplerBinding.sampler = sampler;
            SDL_BindGPUFragmentSamplers(renderPass, 0, &textureSamplerBinding, 1);

            // Draw indexed rectangle (2 triangles = 6 indices)
            SDL_DrawGPUIndexedPrimitives(renderPass, 6, 1, 0, 0, 0);

            SDL_EndGPURenderPass(renderPass);
        }

        // --------------------------------------------------------------------
        // Submit and Present
        // --------------------------------------------------------------------
        SDL_SubmitGPUCommandBuffer(cmdBuf);

        // --------------------------------------------------------------------
        // FPS Counter
        // --------------------------------------------------------------------
        frameCount++;
        Uint64 currentTime = SDL_GetTicks();
        Uint64 elapsedTime = currentTime - fpsLastTime;

        // Update window title every second
        if (elapsedTime >= 1000) {
            float fps = static_cast<float>(frameCount) * 1000.0f / static_cast<float>(elapsedTime);
            char title[128];
            SDL_snprintf(title, sizeof(title), "SDL3 GPU MVP - %.1f FPS | VSync: %s (F1) | HDR: %s (F2)",
                fps,
                vsyncEnabled ? "ON" : "OFF",
                hdrEnabled ? "ON" : "OFF");
            SDL_SetWindowTitle(window, title);

            // Reset counters
            frameCount = 0;
            fpsLastTime = currentTime;
        }
    }

    // --------------------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------------------
    SDL_Log("Shutting down...");

    SDL_ReleaseGPUTransferBuffer(device, vertexTransferBuffer);
    SDL_ReleaseGPUBuffer(device, vertexBuffer);
    SDL_ReleaseGPUBuffer(device, indexBuffer);
    SDL_ReleaseGPUGraphicsPipeline(device, pipelineSDR);
    SDL_ReleaseGPUGraphicsPipeline(device, pipelineHDR);
    SDL_ReleaseGPUSampler(device, sampler);
    SDL_ReleaseGPUTexture(device, texture);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
