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

GPUBackend parseBackend(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--vulkan") == 0) {
            return GPUBackend::VULKAN;
        } else if (strcmp(argv[i], "--d3d12") == 0 || strcmp(argv[i], "--dx12") == 0) {
            return GPUBackend::D3D12;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            SDL_Log("Usage: %s [--vulkan|--d3d12]", argv[0]);
            SDL_Log("  --vulkan  : Use Vulkan backend with SPIR-V shaders");
            SDL_Log("  --d3d12   : Use D3D12 backend with DXIL shaders (Windows only)");
            SDL_Log("  (no args) : Auto-select backend (D3D12 on Windows, Vulkan on Linux)");
        }
    }
    return GPUBackend::AUTO;
}

// ============================================================================
// Main Application
// ============================================================================
int main(int argc, char* argv[]) {
    GPUBackend backend = parseBackend(argc, argv);

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
        "SDL3 GPU MVP - Colored Triangle",
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
    
    switch (backend) {
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
        fragShaderInfo.num_samplers = 0;
        fragShaderInfo.num_storage_buffers = 0;
        fragShaderInfo.num_storage_textures = 0;
        fragShaderInfo.num_uniform_buffers = 0;

        fragmentShader = SDL_CreateGPUShader(device, &fragShaderInfo);
        if (!fragmentShader) {
            SDL_Log("Failed to create fragment shader: %s", SDL_GetError());
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // --------------------------------------------------------------------
    // Create Graphics Pipeline
    // --------------------------------------------------------------------
    SDL_GPUGraphicsPipeline* pipeline = nullptr;

    {
        // Vertex input state
        SDL_GPUVertexAttribute vertexAttributes[2] = {};
        
        // Position attribute
        vertexAttributes[0].location = 0;
        vertexAttributes[0].buffer_slot = 0;
        vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        vertexAttributes[0].offset = 0;
        
        // Color attribute
        vertexAttributes[1].location = 1;
        vertexAttributes[1].buffer_slot = 0;
        vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        vertexAttributes[1].offset = sizeof(float) * 2;

        SDL_GPUVertexBufferDescription vertexBufferDesc = {};
        vertexBufferDesc.slot = 0;
        vertexBufferDesc.pitch = sizeof(Vertex);
        vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vertexBufferDesc.instance_step_rate = 0;

        SDL_GPUVertexInputState vertexInputState = {};
        vertexInputState.num_vertex_buffers = 1;
        vertexInputState.vertex_buffer_descriptions = &vertexBufferDesc;
        vertexInputState.num_vertex_attributes = 2;
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

        SDL_GPUColorTargetDescription colorTargetDesc = {};
        colorTargetDesc.format = SDL_GetGPUSwapchainTextureFormat(device, window);
        colorTargetDesc.blend_state = blendState;

        SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
        targetInfo.num_color_targets = 1;
        targetInfo.color_target_descriptions = &colorTargetDesc;
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

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
        if (!pipeline) {
            SDL_Log("Failed to create graphics pipeline: %s", SDL_GetError());
            SDL_ReleaseGPUShader(device, fragmentShader);
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // We can release shaders after pipeline creation
    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // --------------------------------------------------------------------
    // Create Static Index Buffer (GPU only)
    // --------------------------------------------------------------------
    SDL_GPUBuffer* indexBuffer = nullptr;
    
    {
        // Triangle indices
        const uint16_t indices[] = {0, 1, 2};

        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        bufferInfo.size = sizeof(indices);

        indexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);
        if (!indexBuffer) {
            SDL_Log("Failed to create index buffer: %s", SDL_GetError());
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
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
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
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
    const size_t vertexBufferSize = sizeof(Vertex) * 3;  // 3 vertices for triangle

    // CPU-side vertex data (will be updated each frame)
    std::vector<Vertex> cpuVertexData(3);

    {
        // Create GPU vertex buffer
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size = static_cast<Uint32>(vertexBufferSize);

        vertexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);
        if (!vertexBuffer) {
            SDL_Log("Failed to create vertex buffer: %s", SDL_GetError());
            SDL_ReleaseGPUBuffer(device, indexBuffer);
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
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
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    SDL_Log("SDL3 GPU MVP initialized successfully!");
    SDL_Log("Triangle vertices will be updated each frame (CPU-side)");
    SDL_Log("Color drifts: Blue -> Orange");

    // --------------------------------------------------------------------
    // SDL2-Style Event Loop
    // --------------------------------------------------------------------
    bool running = true;
    Uint64 startTime = SDL_GetTicks();
    
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
                    break;
                
                case SDL_EVENT_WINDOW_RESIZED:
                    // Window resized - swapchain will be recreated automatically
                    break;
            }
        }

        // --------------------------------------------------------------------
        // Update Vertex Buffer (CPU-side, uploaded to GPU each frame)
        // Triangle with colors that drift from Blue to Orange
        // --------------------------------------------------------------------
        {
            float time = static_cast<float>(SDL_GetTicks() - startTime) / 1000.0f;
            float offset = std::sin(time) * 0.05f;  // Small oscillation
            
            // Calculate drifted color
            float tintColor[4];
            float t = (std::sin(time * 0.5f) + 1.0f) * 0.5f;
            lerpColor(tintColor, COLOR_BLUE, COLOR_ORANGE, t);

            // Triangle vertices (position + color)
            cpuVertexData[0] = Vertex{{-0.5f + offset, -0.5f + offset}, {tintColor[0], tintColor[1], tintColor[2], 1.0f}};
            cpuVertexData[1] = Vertex{{ 0.5f - offset, -0.5f + offset}, {tintColor[0], tintColor[1], tintColor[2], 1.0f}};
            cpuVertexData[2] = Vertex{{ 0.0f,          0.5f - offset}, {tintColor[0], tintColor[1], tintColor[2], 1.0f}};

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
            SDL_BindGPUGraphicsPipeline(renderPass, pipeline);

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

            // Draw indexed triangle
            SDL_DrawGPUIndexedPrimitives(renderPass, 3, 1, 0, 0, 0);

            SDL_EndGPURenderPass(renderPass);
        }

        // --------------------------------------------------------------------
        // Submit and Present
        // --------------------------------------------------------------------
        SDL_SubmitGPUCommandBuffer(cmdBuf);
    }

    // --------------------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------------------
    SDL_Log("Shutting down...");

    SDL_ReleaseGPUTransferBuffer(device, vertexTransferBuffer);
    SDL_ReleaseGPUBuffer(device, vertexBuffer);
    SDL_ReleaseGPUBuffer(device, indexBuffer);
    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
