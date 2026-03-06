# Slang Shader Language Integration Plan

## Overview

This document outlines the integration of [Slang shader language](https://github.com/shader-slang/slang) into the SDL3_MVP project, replacing the current HLSL-based offline compilation workflow with runtime Slang shader compilation.

## Current State Analysis

### Existing Shader Pipeline

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  HLSL Source    │     │     DXC         │     │  Compiled       │
│  .vert.hlsl     │────▶│  Offline Build  │────▶│  .spv / .dxil   │
│  .frag.hlsl     │     │  Time Compile   │     │  Binaries       │
└─────────────────┘     └─────────────────┘     └─────────────────┘
                                                        │
                                                        ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  SDL3 GPU       │     │  loadShaderFile │     │  Runtime        │
│  Pipeline       │◀────│  Binary Load    │◀────│  File Read      │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

### Current Files
- [`shaders/triangle.vert.hlsl`](shaders/triangle.vert.hlsl) - Vertex shader in HLSL
- [`shaders/triangle.frag.hlsl`](shaders/triangle.frag.hlsl) - Fragment shader in HLSL
- [`CMakeLists.txt`](CMakeLists.txt:34-70) - DXC-based offline compilation
- [`src/main.cpp`](src/main.cpp:41-55) - Binary shader loading

### Backend Support
| Backend | Shader Format | Extension | Platform |
|---------|--------------|-----------|----------|
| Vulkan  | SPIR-V       | `.spv`    | Linux, Windows |
| D3D12   | DXIL         | `.dxil`   | Windows only |

---

## Target Architecture

### Proposed Slang Pipeline

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Slang Source   │     │  Slang Runtime  │     │  Target Format  │
│  .vert.slang    │────▶│  Compiler       │────▶│  SPIR-V/DXIL    │
│  .frag.slang    │     │  JIT Compile    │     │  In-Memory      │
└─────────────────┘     └─────────────────┘     └─────────────────┘
                                                        │
                                                        ▼
                        ┌─────────────────┐     ┌─────────────────┐
                        │  SDL3 GPU       │     │  Direct         │
                        │  Pipeline       │◀────│  Submission     │
                        └─────────────────┘     └─────────────────┘
```

### Key Benefits
1. **Single Source**: One Slang shader file instead of maintaining HLSL
2. **Runtime Compilation**: No build-time shader compilation step
3. **Cross-Platform**: Slang outputs SPIR-V, DXIL, and Metal Shaders
4. **Hot Reloading**: Potential for runtime shader hot-reloading
5. **Modern Features**: Parameter blocks, specialization constants, etc.

---

## Implementation Plan

### Phase 1: Slang Library Integration

#### 1.1 CMake Changes

Add Slang as a dependency via FetchContent or find_package:

```cmake
# Option A: FetchContent from GitHub release
FetchContent_Declare(
    slang
    GIT_REPOSITORY https://github.com/shader-slang/slang.git
    GIT_TAG        v2026.3.1  # Use latest stable release
    GIT_SHALLOW    TRUE
)

# Option B: Use pre-built SDK if available
# find_package(slang REQUIRED)
```

#### 1.2 Link Slang Library

```cmake
target_link_libraries(${PROJECT_NAME} PRIVATE 
    SDL3::SDL3
    slang::slang  # or just 'slang' depending on how its imported
)
```

### Phase 2: Runtime Shader Compiler Module

#### 2.1 Create Shader Module Structure

Create a new file `src/shader_module.hpp` and `src/shader_module.cpp`:

```cpp
// src/shader_module.hpp
#pragma once

#include <SDL3/SDL_gpu.h>
#include <string>
#include <vector>
#include <memory>

// Forward declare Slang types to avoid header pollution
namespace slang {
    class IGlobalSession;
    class IModule;
    class IEntryPoint;
    class IComponentType;
    class ICompileRequest;
}

enum class ShaderTarget {
    SPIRV,   // Vulkan
    DXIL,    // D3D12
    Metal    // Metal (macOS/iOS)
};

struct CompiledShader {
    std::vector<uint8_t> code;
    std::string entryPoint;
    SDL_GPUShaderFormat format;
    SDL_GPUShaderStage stage;
};

class SlangCompiler {
public:
    SlangCompiler();
    ~SlangCompiler();
    
    bool initialize();
    void shutdown();
    
    CompiledShader compileShader(
        const std::string& sourcePath,
        const std::string& entryPoint,
        ShaderTarget target,
        SDL_GPUShaderStage stage
    );
    
    // Compile from string for hot-reload support
    CompiledShader compileFromSource(
        const std::string& source,
        const std::string& moduleName,
        const std::string& entryPoint,
        ShaderTarget target,
        SDL_GPUShaderStage stage
    );
    
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
```

#### 2.2 Slang Compiler Implementation

Key implementation details for `src/shader_module.cpp`:

```cpp
#include "shader_module.hpp"
#include <slang.h>
#include <slang-gfx.h>

struct SlangCompiler::Impl {
    slang::IGlobalSession* globalSession = nullptr;
    slang::ISession* session = nullptr;
};

SlangCompiler::SlangCompiler() : m_impl(std::make_unique<Impl>()) {}

bool SlangCompiler::initialize() {
    // Create global Slang session
    SlangResult result = slang::createGlobalSession(&m_impl->globalSession);
    if (SLANG_FAILED(result)) {
        SDL_Log("Failed to create Slang global session");
        return false;
    }
    
    // Configure session for target
    slang::SessionDesc sessionDesc = {};
    // ... configure targets, search paths, etc.
    
    m_impl->globalSession->createSession(sessionDesc, &m_impl->session);
    return true;
}

CompiledShader SlangCompiler::compileShader(
    const std::string& sourcePath,
    const std::string& entryPoint,
    ShaderTarget target,
    SDL_GPUShaderStage stage)
{
    CompiledShader result;
    
    // Load module from file
    slang::IModule* module = m_impl->session->loadModule(sourcePath.c_str());
    if (!module) {
        SDL_Log("Failed to load Slang module: %s", sourcePath.c_str());
        return result;
    }
    
    // Find entry point
    slang::IEntryPoint* entry = nullptr;
    module->findEntryPointByName(entryPoint.c_str(), &entry);
    
    // Compose and compile
    slang::IComponentType* program = nullptr;
    // ... compose program and compile to target
    
    // Get compiled code
    void* data = nullptr;
    size_t size = 0;
    // ... get code from program
    
    result.code.assign(static_cast<uint8_t*>(data), 
                       static_cast<uint8_t*>(data) + size);
    result.entryPoint = entryPoint;
    result.format = targetToSDLFormat(target);
    result.stage = stage;
    
    return result;
}
```

### Phase 3: Main.cpp Integration

#### 3.1 Replace Shader Loading

Replace the current [`loadShaderFile()`](src/main.cpp:41-55) approach with Slang compilation:

```cpp
// Current approach
std::vector<uint8_t> vertCode = loadShaderFile("shaders/triangle.vert.spv");

// New approach
SlangCompiler compiler;
compiler.initialize();

CompiledShader vertShader = compiler.compileShader(
    "shaders/triangle.slang",
    "vertexMain",
    detectShaderTarget(),
    SDL_GPU_SHADERSTAGE_VERTEX
);
```

#### 3.2 Target Detection

Map detected backend to Slang target:

```cpp
ShaderTarget detectShaderTarget(const char* driverName) {
    if (strcmp(driverName, "direct3d12") == 0) {
        return ShaderTarget::DXIL;
    } else if (strcmp(driverName, "metal") == 0) {
        return ShaderTarget::Metal;
    }
    return ShaderTarget::SPIRV;  // Default for Vulkan
}
```

### Phase 4: Shader Migration

#### 4.1 Convert HLSL to Slang

The existing HLSL shaders are largely compatible with Slang. Key changes:

**Before (HLSL):**
```hlsl
// triangle.vert.hlsl
struct VSInput {
    float2 position : TEXCOORD0;
    float2 texcoord : TEXCOORD1;
    float4 color : TEXCOORD2;
};

VSOutput main(VSInput input) { ... }
```

**After (Slang):**
```slang
// triangle.slang
struct VSInput {
    float2 position : TEXCOORD0;
    float2 texcoord : TEXCOORD1;
    float4 color : TEXCOORD2;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input) { ... }

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET { ... }
```

#### 4.2 Single File Approach

Slang supports multiple entry points in one file:

```slang
// shaders/triangle.slang

// Shared structures
struct VSInput { ... };
struct VSOutput { ... };
struct PSInput { ... };

// Vertex shader
[shader("vertex")]
VSOutput vertexMain(VSInput input) { ... }

// Fragment shader  
[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET { ... }
```

### Phase 5: Error Handling and Fallback

#### 5.1 Compilation Error Handling

```cpp
CompiledShader SlangCompiler::compileShader(...) {
    // ... compilation code ...
    
    if (SLANG_FAILED(result)) {
        // Get diagnostic output
        const char* diagnostics = program->getDiagnosticMessage();
        SDL_Log("Slang compilation error: %s", diagnostics);
        
        // Optionally: fall back to pre-compiled shaders
        return loadFallbackShader(entryPoint);
    }
}
```

#### 5.2 Fallback Strategy

1. **Primary**: Runtime Slang compilation
2. **Fallback**: Pre-compiled SPIR-V/DXIL shipped with binary
3. **Error**: Graceful failure with clear error message

---

## File Structure Changes

```
SDL3_MVP/
├── CMakeLists.txt           # Updated: Add Slang dependency
├── shaders/
│   ├── triangle.slang       # NEW: Unified Slang shader
│   ├── triangle.vert.hlsl   # Keep for reference/fallback
│   └── triangle.frag.hlsl   # Keep for reference/fallback
├── src/
│   ├── main.cpp             # Updated: Use SlangCompiler
│   ├── shader_module.hpp    # NEW: Slang wrapper header
│   └── shader_module.cpp    # NEW: Slang wrapper implementation
└── plans/
    └── slang_integration_plan.md  # This document
```

---

## CMakeLists.txt Changes Summary

### Remove
- DXC finding and configuration
- `compile_spirv()` and `compile_dxil()` functions
- Shader compilation custom commands
- `shaders` and `shaders_dxil` targets

### Add
- Slang library FetchContent or find_package
- Link slang library to executable
- Optional: Copy .slang files to build directory

---

## Testing Strategy

### Unit Tests
1. SlangCompiler initialization/shutdown
2. Shader compilation for each target (SPIR-V, DXIL)
3. Error handling for invalid shaders

### Integration Tests
1. Vulkan backend with SPIR-V output
2. D3D12 backend with DXIL output (Windows)
3. Shader hot-reload functionality

### Visual Validation
1. Triangle renders identically to HLSL version
2. Texture sampling works correctly
3. Color interpolation is correct

---

## Migration Checklist

- [ ] Add Slang library dependency to CMakeLists.txt
- [ ] Create shader_module.hpp header
- [ ] Implement shader_module.cpp with Slang API
- [ ] Convert triangle.vert.hlsl to Slang
- [ ] Convert triangle.frag.hlsl to Slang
- [ ] Update main.cpp to use SlangCompiler
- [ ] Remove DXC-based offline compilation
- [ ] Test on Vulkan backend (Linux)
- [ ] Test on D3D12 backend (Windows)
- [ ] Add error handling and logging
- [ ] Update documentation

---

## References

- [Slang GitHub Repository](https://github.com/shader-slang/slang)
- [Slang Documentation](https://shader-slang.com/slang/user-guide/)
- [Slang API Reference](https://shader-slang.com/slang/api-reference/)
- [SDL3 GPU Documentation](https://wiki.libsdl.org/SDL3/CategoryGPU)
