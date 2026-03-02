cmake_minimum_required(VERSION 3.20)
project(SDL3_MVP VERSION 1.0.0 LANGUAGES CXX)

# C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include FetchContent and find packages
include(FetchContent)

# Fetch SDL3
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.2
    GIT_SHALLOW TRUE
)

# Make SDL3 available
FetchContent_MakeAvailable(SDL3)

# Find DXC for HLSL compilation (Windows only)
if(WIN32)
    find_program(DXC dxc HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
    )
endif()

# Shader compilation function for SPIR-V (Vulkan) using DXC
function(compile_spirv input_file output_file shader_stage)
    if(DXC)
        add_custom_command(
            OUTPUT ${output_file}
            COMMAND ${DXC}
                -T ${shader_stage}_6_0
                -E main
                -spirv
                -Fo ${output_file}
                ${input_file}
            DEPENDS ${input_file}
            COMMENT "Compiling ${input_file} to SPIR-V via DXC"
        )
    else()
        file(WRITE ${output_file} "")
        message(WARNING "No DXC found. SPIR-V shaders must be pre-compiled.")
    endif()
endfunction()

# Shader compilation function for DXIL (D3D12) using DXC
function(compile_dxil input_file output_file shader_stage)
    if(DXC)
        add_custom_command(
            OUTPUT ${output_file}
            COMMAND ${DXC}
                -T ${shader_stage}_6_0
                -E main
                -Fo ${output_file}
                ${input_file}
            DEPENDS ${input_file}
            COMMENT "Compiling ${input_file} to DXIL via DXC"
        )
    else()
        file(WRITE ${output_file} "")
        message(WARNING "No DXC found. DXIL shaders must be pre-compiled.")
    endif()
endfunction()

# Compile shaders
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(BUILT_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${BUILT_SHADER_DIR})

# Use HLSL as source for both backends
set(VERT_SOURCE ${SHADER_DIR}/triangle.vert.hlsl)
set(FRAG_SOURCE ${SHADER_DIR}/triangle.frag.hlsl)

# SPIR-V shaders for Vulkan (compiled from HLSL using DXC)
compile_spirv(${VERT_SOURCE} ${BUILT_SHADER_DIR}/triangle.vert.spv vs)
compile_spirv(${FRAG_SOURCE} ${BUILT_SHADER_DIR}/triangle.frag.spv ps)

# DXIL shaders for D3D12 (Windows only)
if(WIN32)
    compile_dxil(${VERT_SOURCE} ${BUILT_SHADER_DIR}/triangle.vert.dxil vs)
    compile_dxil(${FRAG_SOURCE} ${BUILT_SHADER_DIR}/triangle.frag.dxil ps)
endif()

add_custom_target(shaders ALL DEPENDS
    ${BUILT_SHADER_DIR}/triangle.vert.spv
    ${BUILT_SHADER_DIR}/triangle.frag.spv
)

# Create executable
add_executable(${PROJECT_NAME}
    src/main.cpp
)

# Define shaders_dxil target for DXIL shaders
if(WIN32 AND DXC)
    add_custom_target(shaders_dxil ALL DEPENDS
        ${BUILT_SHADER_DIR}/triangle.vert.dxil
        ${BUILT_SHADER_DIR}/triangle.frag.dxil
    )
endif()

add_dependencies(${PROJECT_NAME} shaders)

# Add DXIL shader dependencies on Windows
if(WIN32 AND DXC)
    add_dependencies(${PROJECT_NAME} shaders_dxil)
endif()

# Link SDL3
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE FALSE)
    
    # Copy SDL3 DLL to output directory on Windows
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
elseif(APPLE)
    # macOS-specific settings
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
    )
endif()

# Enable all warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Set output directories
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Copy assets to build directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying assets to build directory"
)

# Copy compiled shaders to build directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${BUILT_SHADER_DIR}"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Copying shaders to build directory"
)

# Install targets
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

install(DIRECTORY assets DESTINATION .)
