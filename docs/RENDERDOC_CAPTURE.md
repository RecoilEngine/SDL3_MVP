# RenderDoc Capture Guide

## Linux Capture Requirements

**Important**: On Linux, RenderDoc only works when the application is run with the X11 video driver. Wayland is not supported for capture.

## Running the Application for RenderDoc

### Basic Run with X11 Driver

```bash
SDL_VIDEO_DRIVER=x11 ./SDL3_MVP
```

### Capture with renderdoccmd

```bash
SDL_VIDEO_DRIVER=x11 renderdoccmd capture <options> ./SDL3_MVP
```

## Full Examples

### Capture a Single Frame

```bash
SDL_VIDEO_DRIVER=x11 renderdoccmd capture --opt-ref-all-resources ./SDL3_MVP
```

### Capture Multiple Frames

```bash
SDL_VIDEO_DRIVER=x11 renderdoccmd capture --frame-count 10 ./SDL3_MVP
```

### Capture with Specific Output Directory

```bash
SDL_VIDEO_DRIVER=x11 renderdoccmd capture --capture-dir ~/captures ./SDL3_MVP
```

## GUI Alternative

You can also use the RenderDoc GUI:

1. Launch `qrenderdoc`
2. Set the executable path to your `./SDL3_MVP` binary
3. In "Environment Variables", add: `SDL_VIDEO_DRIVER=x11`
4. Click Launch to run and capture

## Verification

To verify you're running on X11:

```bash
SDL_VIDEO_DRIVER=x11 ./SDL3_MVP
```

The console output should show it's using the X11 video driver.

## Troubleshooting

### Black Screen or Capture Fails

- Ensure `SDL_VIDEO_DRIVER=x11` is set **before** the executable in the command
- Verify RenderDoc is properly installed: `which renderdoccmd`
- Check RenderDoc version compatibility with your GPU drivers

### Application Runs but No Capture

- Make sure the application actually renders frames (not minimized or hidden)
- Try enabling "Ref All Resources" in capture options
- Check RenderDoc's diagnostic log for errors

## Additional Resources

- RenderDoc Documentation: https://renderdoc.org/docs/
- SDL3 Video Driver Documentation: https://wiki.libsdl.org/SDL3/VideoDrivers
