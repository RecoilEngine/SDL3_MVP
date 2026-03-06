#pragma once

#include <SDL3/SDL_gpu.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>

// Forward declare Slang types to avoid header pollution in user code
namespace slang {
    class IGlobalSession;
    class ISession;
    class IModule;
    class IEntryPoint;
    class IComponentType;
}

// ============================================================================
// Shader Target Enumeration
// ============================================================================
enum class ShaderTarget {
    SPIRV,   // Vulkan
    DXIL,    // D3D12
    Metal    // Metal (macOS/iOS) - future support
};

// ============================================================================
// Compiled Shader Result
// ============================================================================
struct CompiledShader {
    std::vector<uint8_t> code;
    std::string entryPoint;
    SDL_GPUShaderFormat format;
    SDL_GPUShaderStage stage;
    std::string errorMessage;
    
    bool isValid() const { return !code.empty() && errorMessage.empty(); }
};

// ============================================================================
// Slang Compiler Options
// ============================================================================
struct SlangCompilerOptions {
    std::vector<std::string> includePaths;
    std::vector<std::string> preprocessorDefinitions;
    bool enableDebugInfo = false;
    bool optimize = true;
    std::string matrixLayout = "row-major";  // or "column-major"
};

// ============================================================================
// Slang Compiler Class
// ============================================================================
class SlangCompiler {
public:
    SlangCompiler();
    ~SlangCompiler();
    
    // Non-copyable, movable
    SlangCompiler(const SlangCompiler&) = delete;
    SlangCompiler& operator=(const SlangCompiler&) = delete;
    SlangCompiler(SlangCompiler&&) noexcept;
    SlangCompiler& operator=(SlangCompiler&&) noexcept;
    
    // Initialize the Slang compiler. Must be called before compileShader.
    bool initialize();
    
    // Shutdown and release all Slang resources.
    void shutdown();
    
    // Check if the compiler is initialized and ready.
    bool isInitialized() const;
    
    // Compile a shader from a file.
    // @param sourcePath Path to the .slang file
    // @param entryPoint Name of the entry point function (e.g., "vertexMain")
    // @param target Target format (SPIRV, DXIL, Metal)
    // @param stage Shader stage (vertex, fragment, etc.)
    // @return CompiledShader with bytecode or error message
    CompiledShader compileShader(
        const std::string& sourcePath,
        const std::string& entryPoint,
        ShaderTarget target,
        SDL_GPUShaderStage stage
    );
    
    // Compile a shader from source string.
    // Useful for hot-reload or embedded shaders.
    // @param source The Slang source code
    // @param moduleName A name for the module (for error messages)
    // @param entryPoint Name of the entry point function
    // @param target Target format
    // @param stage Shader stage
    // @return CompiledShader with bytecode or error message
    CompiledShader compileFromSource(
        const std::string& source,
        const std::string& moduleName,
        const std::string& entryPoint,
        ShaderTarget target,
        SDL_GPUShaderStage stage
    );
    
    // Set compiler options
    void setOptions(const SlangCompilerOptions& options);
    
    // Utility: Convert ShaderTarget to SDL_GPUShaderFormat
    static SDL_GPUShaderFormat targetToSDLFormat(ShaderTarget target);
    
    // Utility: Detect best shader target from SDL GPU driver name
    static ShaderTarget detectTargetFromDriver(const char* driverName);
    
    // Utility: Get file extension for a shader target
    static const char* getTargetExtension(ShaderTarget target);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    
    // Internal compilation helper
    CompiledShader compileInternal(
        slang::IModule* module,
        const std::string& entryPoint,
        ShaderTarget target,
        SDL_GPUShaderStage stage
    );
};

// ============================================================================
// Utility Functions
// ============================================================================

// Load a file into a string (for shader source loading)
std::optional<std::string> loadShaderSource(const std::string& path);

// Save compiled shader bytecode to a file (for caching)
bool saveShaderBytecode(const std::string& path, const std::vector<uint8_t>& code);
