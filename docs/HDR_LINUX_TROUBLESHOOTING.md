# HDR Troubleshooting Guide for Linux

## Symptom

Upon starting the binary and pressing F2 (or starting with `--hdr`):

```
./SDL3_MVP
GPU Device Driver: vulkan
Shader Format: SPIR-V
Loaded texture: 256x256
Created SDR pipeline with format: 12
Created HDR pipeline with format: R10G10B10A2_UNORM
SDL3 GPU MVP initialized successfully!
Controls: F1=Toggle VSync | F2=Toggle HDR | ESC=Quit
HDR mode: disabled
=== HDR Evaluation Results ===
HDR10 ST2084 (PQ):       NOT AVAILABLE
HDR Extended Linear:     NOT AVAILABLE
SDR:                     AVAILABLE (fallback)
===========================
Selected: SDR - No HDR modes available
HDR not supported. On Windows, enable HDR in Settings -> Display -> HDR.
```

## What You Can Do

### If you're on AMD:

Ensure Mesa ≥ 25.1 is installed. Check with:

```bash
vulkaninfo | grep swapchain_colorspace
```

### If you're on NVIDIA:

1. **Install the vk-hdr-layer** (also called VK_hdr_layer) package if available on your distro. This Vulkan layer injects the missing extensions:

   - **Arch**: `vk-hdr-layer` (AUR or community repos)
   - **Fedora/Bazzite**: Check if it's still available — it was marked obsolete in some repos as of early 2026

2. **Enable it by setting the environment variable** before launching your app:

   ```bash
   ENABLE_HDR_WSI=1 ./SDL3_MVP
   ```

3. **Verify extensions are now present**:

   ```bash
   vulkaninfo | grep -i hdr
   ```

## Additional Checks

### Verify Display HDR Capability

Ensure your display actually supports HDR and that it's enabled in your display settings:

- **KDE Plasma**: System Settings → Display → HDR
- **GNOME**: Settings → Displays → HDR (if available)
- **Command line**: Check your display's EDID for HDR metadata

### Check Vulkan Instance and Device Extensions

```bash
# List all Vulkan extensions
vulkaninfo | grep -E "(VK_EXT|VK_KHR).*swapchain"

# Check specifically for HDR-related extensions
vulkaninfo | grep -i "color_space\|hdr"
```

### Environment Variables

Some compositors and drivers require specific environment variables:

```bash
# For Wayland with HDR support
WLR_DRM_NO_ATOMIC=0

# For NVIDIA with explicit sync
__GL_VRR_ALLOWED=0
```

### Compositor Support

Not all Wayland compositors support HDR yet. As of early 2026:

- **KWin (KDE)**: HDR support is experimental
- **Mutter (GNOME)**: Limited HDR support
- **Sway/wlroots**: No native HDR support yet
- **Gamescope**: Supports HDR for fullscreen applications

## Still Not Working?

If HDR still doesn't work after trying the above:

1. Check your SDL3 version (`sdl3-config --version`) - HDR support requires SDL3 ≥ 3.2.0
2. Verify your GPU drivers are up to date
3. Try running with `SDL_VIDEODRIVER=wayland` or `SDL_VIDEODRIVER=x11` to test different backends
4. Check the SDL3 GPU backend being used - some backends have better HDR support than others

## Reporting Issues

When reporting HDR issues, please include:

- GPU model and driver version
- Display model and connection type (HDMI 2.1, DisplayPort 1.4, etc.)
- Desktop environment and compositor
- Output of `vulkaninfo | grep -i hdr`
- Full application log with `SDL_GPU_DEBUG=1` if available
