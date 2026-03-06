cmake_minimum_required(VERSION 3.20)
project(SDL3_MVP VERSION 1.0.0 LANGUAGES CXX)

# C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include FetchContent and find packages
include(FetchContent)

# ============================================================================
# SDL3
# ============================================================================
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# ============================================================================
# Slang Shader Compiler - Download pre-built binaries
# ============================================================================
set(SLANG_VERSION "2026.3.1")

if(WIN32)
    set(SLANG_PLATFORM "windows-x64")
    set(SLANG_ARCHIVE_EXT "zip")
elseif(APPLE)
    set(SLANG_PLATFORM "macos-x64")
    set(SLANG_ARCHIVE_EXT "zip")
else()
    set(SLANG_PLATFORM "linux-x86_64")
    set(SLANG_ARCHIVE_EXT "tar.gz")
endif()

set(SLANG_ARCHIVE_NAME "slang-${SLANG_VERSION}-${SLANG_PLATFORM}.${SLANG_ARCHIVE_EXT}")
set(SLANG_DOWNLOAD_URL "https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/${SLANG_ARCHIVE_NAME}")

FetchContent_Declare(
    slang
    URL ${SLANG_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(slang)

# Set up Slang paths from fetched content
set(SLANG_ROOT "${slang_SOURCE_DIR}")
set(SLANG_INCLUDE_DIR "${SLANG_ROOT}/include")
set(SLANG_LIB_DIR "${SLANG_ROOT}/lib")
set(SLANG_BIN_DIR "${SLANG_ROOT}/bin")

message(STATUS "Slang SDK found at: ${SLANG_ROOT}")
message(STATUS "Slang include: ${SLANG_INCLUDE_DIR}")
message(STATUS "Slang lib: ${SLANG_LIB_DIR}")

# Create imported target for Slang
add_library(slang::slang SHARED IMPORTED GLOBAL)

if(WIN32)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_BIN_DIR}/slang.dll"
        IMPORTED_IMPLIB "${SLANG_LIB_DIR}/slang.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
    )
elseif(APPLE)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIB_DIR}/libslang.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
    )
else()
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIB_DIR}/libslang.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
    )
endif()

# ============================================================================
# Executable
# ============================================================================
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/shader_module.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE 
    SDL3::SDL3
    slang::slang
)

# ============================================================================
# Platform-specific settings
# ============================================================================
if(WIN32)
    set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE FALSE)
    
    # Copy SDL3 DLL to output directory
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
    
    # Copy Slang DLL to output directory
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLANG_BIN_DIR}/slang.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
elseif(APPLE)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
    )
else()
    # Copy Slang shared library to output directory on Linux
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLANG_LIB_DIR}/libslang.so"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()

# ============================================================================
# Compiler warnings
# ============================================================================
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Output directories
# ============================================================================
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# ============================================================================
# Copy assets and shaders to build directory
# ============================================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying assets to build directory"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/shaders/triangle.slang"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/triangle.slang"
    COMMENT "Copying Slang shaders to build directory"
)

# ============================================================================
# Install targets
# ============================================================================
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

install(DIRECTORY assets DESTINATION .)
install(DIRECTORY shaders DESTINATION . FILES_MATCHING PATTERN "*.slang")
