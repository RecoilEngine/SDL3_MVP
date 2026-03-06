#include "shader_module.hpp"

#include <SDL3/SDL.h>

// Slang C++ API
#include <slang.h>
#include <slang-com-ptr.h>

#include <fstream>
#include <sstream>
#include <cstring>

using Slang::ComPtr;

// ============================================================================
// Implementation Details
// ============================================================================
struct SlangCompiler::Impl {
    ComPtr<slang::IGlobalSession> globalSession;
    SlangCompilerOptions options;
    bool initialized = false;
};

// ============================================================================
// Constructor / Destructor
// ============================================================================
SlangCompiler::SlangCompiler() : m_impl(std::make_unique<Impl>()) {}

SlangCompiler::~SlangCompiler() {
    shutdown();
}

SlangCompiler::SlangCompiler(SlangCompiler&& other) noexcept
    : m_impl(std::move(other.m_impl)) {}

SlangCompiler& SlangCompiler::operator=(SlangCompiler&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

// ============================================================================
// Initialization
// ============================================================================
bool SlangCompiler::initialize() {
    if (m_impl->initialized) {
        return true;
    }

    // Create global session using the C API function
    SlangResult result = slang_createGlobalSession(SLANG_API_VERSION,
        m_impl->globalSession.writeRef());

    if (SLANG_FAILED(result)) {
        SDL_Log("[Slang] Failed to create global session: 0x%08X", (uint32_t)result);
        return false;
    }

    m_impl->initialized = true;
    SDL_Log("[Slang] Compiler initialized successfully");
    return true;
}

void SlangCompiler::shutdown() {
    m_impl->globalSession.setNull();
    m_impl->initialized = false;
}

bool SlangCompiler::isInitialized() const {
    return m_impl->initialized;
}

// ============================================================================
// Options
// ============================================================================
void SlangCompiler::setOptions(const SlangCompilerOptions& options) {
    m_impl->options = options;
}

// ============================================================================
// Target Conversion Utilities
// ============================================================================
SDL_GPUShaderFormat SlangCompiler::targetToSDLFormat(ShaderTarget target) {
    switch (target) {
        case ShaderTarget::SPIRV:
            return SDL_GPU_SHADERFORMAT_SPIRV;
        case ShaderTarget::DXIL:
            return SDL_GPU_SHADERFORMAT_DXIL;
        case ShaderTarget::Metal:
            // SDL3 doesn't have a METAL format flag yet
            // MoltenVK can convert SPIR-V to Metal
            return SDL_GPU_SHADERFORMAT_SPIRV;
    }
    return SDL_GPU_SHADERFORMAT_SPIRV;
}

ShaderTarget SlangCompiler::detectTargetFromDriver(const char* driverName) {
    if (!driverName) {
        return ShaderTarget::SPIRV;  // Default to Vulkan/SPIR-V
    }

    if (strcmp(driverName, "direct3d12") == 0) {
        return ShaderTarget::DXIL;
    }
    if (strcmp(driverName, "metal") == 0) {
        return ShaderTarget::Metal;
    }
    // Vulkan, and any other backend defaults to SPIR-V
    return ShaderTarget::SPIRV;
}

const char* SlangCompiler::getTargetExtension(ShaderTarget target) {
    switch (target) {
        case ShaderTarget::SPIRV:
            return ".spv";
        case ShaderTarget::DXIL:
            return ".dxil";
        case ShaderTarget::Metal:
            return ".metallib";
    }
    return ".spv";
}

// ============================================================================
// Helper: Get Slang target code from ShaderTarget
// ============================================================================
static SlangCompileTarget getSlangTarget(ShaderTarget target) {
    switch (target) {
        case ShaderTarget::SPIRV:
            return SLANG_SPIRV;
        case ShaderTarget::DXIL:
            return SLANG_DXIL;
        case ShaderTarget::Metal:
            return SLANG_METAL_LIB;
    }
    return SLANG_SPIRV;
}

// ============================================================================
// Helper: Extract reflection data from a Slang program
// ============================================================================
static ShaderReflection extractReflection(slang::IComponentType* program, const char* entryPointName) {
    ShaderReflection reflection;

    if (!program) {
        SDL_Log("[Slang] Warning: Null program for reflection");
        return reflection;
    }

    // Get the program layout which contains all reflection info
    slang::ProgramLayout* programLayout = program->getLayout();
    if (!programLayout) {
        SDL_Log("[Slang] Warning: Failed to get program layout for reflection");
        return reflection;
    }

    SDL_Log("[Slang] Performing reflection for entry point: %s", entryPointName);

    // Find the entry point to determine its stage
    slang::EntryPointLayout* entryPointLayout = nullptr;
    int entryPointCount = programLayout->getEntryPointCount();
    SDL_Log("[Slang] Entry point count: %d", (int)entryPointCount);

    for (int e = 0; e < entryPointCount; ++e) {
        slang::EntryPointLayout* ep = programLayout->getEntryPointByIndex(e);
        if (ep) {
            const char* name = ep->getName();
            const char* nameOverride = ep->getNameOverride();
            SDL_Log("[Slang] Entry point %d: name='%s' override='%s'",
                    (int)e, name ? name : "(null)", nameOverride ? nameOverride : "(null)");

            // Match by name or nameOverride
            if ((name && strcmp(name, entryPointName) == 0) ||
                (nameOverride && strcmp(nameOverride, entryPointName) == 0)) {
                entryPointLayout = ep;
                break;
            }
        }
    }

    // Determine if this is a vertex shader based on the entry point name
    // SDL3 doesn't support vertex shader samplers, so we need to exclude them
    bool isVertexShader = (entryPointName &&
        (strstr(entryPointName, "vertex") != nullptr ||
         strstr(entryPointName, "Vertex") != nullptr ||
         strstr(entryPointName, "vert") != nullptr ||
         strstr(entryPointName, "Vert") != nullptr));

    // Also check the entry point's stage from reflection if available
    if (entryPointLayout) {
        SlangStage stage = entryPointLayout->getStage();
        SDL_Log("[Slang] Entry point stage: %d", (int)stage);
        if (stage == SLANG_STAGE_VERTEX) {
            isVertexShader = true;
        }
    }

    if (isVertexShader) {
        SDL_Log("[Slang] Detected vertex shader - excluding sampler/texture resources (SDL3 limitation)");
    }

    // Count global parameters - these are module-level resources
    // Note: Slang reports global resources for all entry points, but SDL3 requires
    // us to only report resources that can actually be bound for each stage
    unsigned int paramCount = programLayout->getParameterCount();
    SDL_Log("[Slang] Global parameter count: %d", (int)paramCount);

    for (unsigned int i = 0; i < paramCount; ++i) {
        slang::VariableLayoutReflection* param = programLayout->getParameterByIndex(i);
        if (!param) continue;

        slang::TypeLayoutReflection* typeLayout = param->getTypeLayout();
        if (!typeLayout) continue;

        const char* paramName = param->getName();
        slang::TypeReflection::Kind kind = typeLayout->getKind();

        SDL_Log("[Slang] Parameter %d: '%s' kind=%d", (int)i, paramName ? paramName : "(unnamed)", (int)kind);

        // Use binding ranges to determine resource types
        SlangInt bindingRangeCount = typeLayout->getBindingRangeCount();

        for (SlangInt b = 0; b < bindingRangeCount; ++b) {
            slang::BindingType bindingType = typeLayout->getBindingRangeType(b);
            SlangInt bindingCount = typeLayout->getBindingRangeBindingCount(b);

            SDL_Log("[Slang]   Binding %d: type=%d, count=%d", (int)b, (int)bindingType, (int)bindingCount);

            switch (bindingType) {
                case slang::BindingType::ConstantBuffer:
                    reflection.numUniformBuffers += bindingCount;
                    SDL_Log("[Slang]   -> Uniform Buffer");
                    break;

                case slang::BindingType::Sampler:
                    // SDL3 doesn't support vertex shader samplers - skip for vertex shaders
                    if (!isVertexShader) {
                        reflection.numSamplers += bindingCount;
                        SDL_Log("[Slang]   -> Sampler");
                    } else {
                        SDL_Log("[Slang]   -> Sampler (skipped - vertex shader)");
                    }
                    break;

                case slang::BindingType::Texture:
                case slang::BindingType::CombinedTextureSampler:
                    // SDL3 doesn't support vertex shader textures - skip for vertex shaders
                    if (!isVertexShader) {
                        reflection.numSampledTextures += bindingCount;
                        SDL_Log("[Slang]   -> Sampled Texture");
                    } else {
                        SDL_Log("[Slang]   -> Sampled Texture (skipped - vertex shader)");
                    }
                    break;

                case slang::BindingType::MutableTexture:
                    reflection.numStorageTextures += bindingCount;
                    SDL_Log("[Slang]   -> Storage Texture");
                    break;

                case slang::BindingType::TypedBuffer:
                    reflection.numSampledTextures += bindingCount;
                    SDL_Log("[Slang]   -> Typed Buffer");
                    break;

                case slang::BindingType::MutableTypedBuffer:
                case slang::BindingType::RawBuffer:
                case slang::BindingType::MutableRawBuffer:
                    reflection.numStorageBuffers += bindingCount;
                    SDL_Log("[Slang]   -> Storage Buffer");
                    break;

                default:
                    SDL_Log("[Slang]   -> Unknown binding type %d", (int)bindingType);
                    break;
            }
        }
    }

    SDL_Log("[Slang] Reflection results: uniformBuffers=%u, storageBuffers=%u, samplers=%u, storageTextures=%u, sampledTextures=%u",
           reflection.numUniformBuffers, reflection.numStorageBuffers,
           reflection.numSamplers, reflection.numStorageTextures, reflection.numSampledTextures);

    return reflection;
}

// ============================================================================
// Shader Compilation
// ============================================================================
CompiledShader SlangCompiler::compileShader(
    const std::string& sourcePath,
    const std::string& entryPoint,
    ShaderTarget target,
    SDL_GPUShaderStage stage)
{
    CompiledShader result;

    if (!m_impl->initialized) {
        result.errorMessage = "Slang compiler not initialized";
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    // Build session descriptor
    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};

    targetDesc.format = getSlangTarget(target);
    targetDesc.profile = m_impl->globalSession->findProfile(
        target == ShaderTarget::DXIL ? "sm_6_0" : "spirv_1_0");

    // Use flags for optimization and debug info
    // Note: SlangTargetFlags are defined in slang.h
    targetDesc.flags = kDefaultTargetFlags;

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    // Search paths
    std::vector<const char*> searchPaths;
    for (const auto& path : m_impl->options.includePaths) {
        searchPaths.push_back(path.c_str());
    }
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());

    // Create session
    ComPtr<slang::ISession> session;
    SlangResult res = m_impl->globalSession->createSession(sessionDesc, session.writeRef());
    if (SLANG_FAILED(res)) {
        result.errorMessage = "Failed to create Slang session";
        SDL_Log("[Slang] %s: 0x%08X", result.errorMessage.c_str(), (uint32_t)res);
        return result;
    }

    // Load the module from file
    ComPtr<slang::IBlob> diagnosticsBlob;
    ComPtr<slang::IModule> module;

    module = session->loadModule(sourcePath.c_str(), diagnosticsBlob.writeRef());

    if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
        std::string diag(
            static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
            diagnosticsBlob->getBufferSize()
        );
        SDL_Log("[Slang] Module load diagnostics: %s", diag.c_str());
    }

    if (!module) {
        result.errorMessage = "Failed to load Slang module: " + sourcePath;
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    // Find the entry point
    ComPtr<slang::IEntryPoint> entryPointObj;
    res = module->findEntryPointByName(entryPoint.c_str(), entryPointObj.writeRef());

    if (SLANG_FAILED(res) || !entryPointObj) {
        result.errorMessage = "Failed to find entry point: " + entryPoint;
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    // Compose the program
    ComPtr<slang::IComponentType> program;
    slang::IComponentType* components[] = { module, entryPointObj };

    res = session->createCompositeComponentType(
        components,
        2,
        program.writeRef()
    );

    if (SLANG_FAILED(res)) {
        result.errorMessage = "Failed to compose shader program";
        SDL_Log("[Slang] %s: 0x%08X", result.errorMessage.c_str(), (uint32_t)res);
        return result;
    }

    // Extract reflection data before compilation
    result.reflection = extractReflection(program, entryPoint.c_str());

    // Compile to target format
    ComPtr<slang::IBlob> codeBlob;
    res = program->getEntryPointCode(
        0,  // entryPointIndex
        0,  // targetIndex
        codeBlob.writeRef(),
        diagnosticsBlob.writeRef()
    );

    if (SLANG_FAILED(res)) {
        if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
            result.errorMessage = std::string(
                static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
                diagnosticsBlob->getBufferSize()
            );
        } else {
            result.errorMessage = "Shader compilation failed";
        }
        SDL_Log("[Slang] Compilation error: %s", result.errorMessage.c_str());
        return result;
    }

    if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
        std::string warnings(
            static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
            diagnosticsBlob->getBufferSize()
        );
        SDL_Log("[Slang] Warnings: %s", warnings.c_str());
    }

    // Copy compiled code
    if (!codeBlob || codeBlob->getBufferSize() == 0) {
        result.errorMessage = "Shader compilation produced no output";
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    const uint8_t* codePtr = static_cast<const uint8_t*>(codeBlob->getBufferPointer());
    size_t codeSize = codeBlob->getBufferSize();
    result.code.assign(codePtr, codePtr + codeSize);
    // SPIR-V always uses "main" as the entry point name in the compiled binary
    result.entryPoint = (target == ShaderTarget::SPIRV) ? "main" : entryPoint;
    result.format = targetToSDLFormat(target);
    result.stage = stage;

    SDL_Log("[Slang] Successfully compiled '%s' entry '%s' (%zu bytes)",
            sourcePath.c_str(), entryPoint.c_str(), codeSize);

    return result;
}

CompiledShader SlangCompiler::compileFromSource(
    const std::string& source,
    const std::string& moduleName,
    const std::string& entryPoint,
    ShaderTarget target,
    SDL_GPUShaderStage stage)
{
    CompiledShader result;

    if (!m_impl->initialized) {
        result.errorMessage = "Slang compiler not initialized";
        return result;
    }

    // Build session descriptor
    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};

    targetDesc.format = getSlangTarget(target);
    targetDesc.profile = m_impl->globalSession->findProfile(
        target == ShaderTarget::DXIL ? "sm_6_0" : "spirv_1_0");

    // Use default flags
    targetDesc.flags = kDefaultTargetFlags;

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    // Create session
    ComPtr<slang::ISession> session;
    SlangResult res = m_impl->globalSession->createSession(sessionDesc, session.writeRef());
    if (SLANG_FAILED(res)) {
        result.errorMessage = "Failed to create Slang session";
        SDL_Log("[Slang] %s: 0x%08X", result.errorMessage.c_str(), (uint32_t)res);
        return result;
    }

    // Load module from source string
    ComPtr<slang::IBlob> diagnosticsBlob;
    ComPtr<slang::IModule> module;

    module = session->loadModuleFromSourceString(
        moduleName.c_str(),
        "slang",  // path hint
        source.c_str(),
        diagnosticsBlob.writeRef()
    );

    if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
        std::string diag(
            static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
            diagnosticsBlob->getBufferSize()
        );
        SDL_Log("[Slang] Module load diagnostics: %s", diag.c_str());
    }

    if (!module) {
        result.errorMessage = "Failed to load Slang module from source";
        if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
            result.errorMessage += "\n";
            result.errorMessage += std::string(
                static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
                diagnosticsBlob->getBufferSize()
            );
        }
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    // Find the entry point
    ComPtr<slang::IEntryPoint> entryPointObj;
    res = module->findEntryPointByName(entryPoint.c_str(), entryPointObj.writeRef());

    if (SLANG_FAILED(res) || !entryPointObj) {
        result.errorMessage = "Failed to find entry point: " + entryPoint;
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    // Compose the program
    ComPtr<slang::IComponentType> program;
    slang::IComponentType* components[] = { module, entryPointObj };

    res = session->createCompositeComponentType(
        components,
        2,
        program.writeRef()
    );

    if (SLANG_FAILED(res)) {
        result.errorMessage = "Failed to compose shader program";
        SDL_Log("[Slang] %s: 0x%08X", result.errorMessage.c_str(), (uint32_t)res);
        return result;
    }

    // Extract reflection data before compilation
    result.reflection = extractReflection(program, entryPoint.c_str());

    // Compile to target format
    ComPtr<slang::IBlob> codeBlob;
    res = program->getEntryPointCode(
        0,  // entryPointIndex
        0,  // targetIndex
        codeBlob.writeRef(),
        diagnosticsBlob.writeRef()
    );

    if (SLANG_FAILED(res)) {
        if (diagnosticsBlob && diagnosticsBlob->getBufferSize() > 0) {
            result.errorMessage = std::string(
                static_cast<const char*>(diagnosticsBlob->getBufferPointer()),
                diagnosticsBlob->getBufferSize()
            );
        } else {
            result.errorMessage = "Shader compilation failed";
        }
        SDL_Log("[Slang] Compilation error: %s", result.errorMessage.c_str());
        return result;
    }

    // Copy compiled code
    if (!codeBlob || codeBlob->getBufferSize() == 0) {
        result.errorMessage = "Shader compilation produced no output";
        SDL_Log("[Slang] %s", result.errorMessage.c_str());
        return result;
    }

    const uint8_t* codePtr = static_cast<const uint8_t*>(codeBlob->getBufferPointer());
    size_t codeSize = codeBlob->getBufferSize();
    result.code.assign(codePtr, codePtr + codeSize);
    // SPIR-V always uses "main" as the entry point name in the compiled binary
    result.entryPoint = (target == ShaderTarget::SPIRV) ? "main" : entryPoint;
    result.format = targetToSDLFormat(target);
    result.stage = stage;

    SDL_Log("[Slang] Successfully compiled module '%s' entry '%s' (%zu bytes)",
            moduleName.c_str(), entryPoint.c_str(), codeSize);

    return result;
}

// ============================================================================
// Utility Functions
// ============================================================================
std::optional<std::string> loadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SDL_Log("[Slang] Failed to open shader source file: %s", path.c_str());
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool saveShaderBytecode(const std::string& path, const std::vector<uint8_t>& code) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        SDL_Log("[Slang] Failed to create bytecode file: %s", path.c_str());
        return false;
    }

    file.write(reinterpret_cast<const char*>(code.data()), code.size());
    return true;
}
