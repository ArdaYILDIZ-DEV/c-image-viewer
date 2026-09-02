# c-image-viewer

Minimalist dual-pane image viewer built with modern C11 and SDL2, featuring synchronized viewport transformations, an in-window file tree browser, secure process-isolated clipboard integration, robust EXIF parsing, and high-performance batched text rendering.

## Overview

c-image-viewer is designed for precision visual inspection and side-by-side comparison of imagery. It supports viewing either a single image occupying the full window or two images side-by-side separated by an integrated split divider. View transforms including cursor-centered zoom and image-space panning remain strictly synchronized between both panes, allowing direct pixel-level comparison of two versions of an image, compression artifacts, or rendering passes. A dedicated free mode allows independent positioning and scaling of either pane.

The application includes an embedded in-window directory tree browser with live search filtering, comprehensive EXIF metadata extraction, and desktop integration for standard file manager workflows.

## Key Features

### Synchronized Dual-Pane Inspection
- Side-by-side display of two images with independent clipping rects to prevent visual bleeding across the central divider.
- Multiplicative cursor-centered zooming ranging from 0.05x to 32x. When zooming, the world coordinate under the cursor remains stationary through coordinate compensation.
- Constant-speed panning in image space regardless of active zoom level.
- Flexible synchronization toggle allowing both panes to lock into shared zoom and pan or unlock for independent per-pane adjustments.
- Active pane switching with visual border indicators.
- Drag-and-drop file support allowing users to drop images directly onto either the left or right pane to replace the active image on that side.

### Directory Navigation and In-Window Browser
- Seamless folder navigation to jump forward or backward through images in the active directory, automatically skipping corrupted or unreadable files.
- Modal file browser overlay providing an interactive expandable and collapsible directory tree.
- Live search filtering by file and folder name with case-insensitive matching.
- Cycle detection that guards against recursive symbolic links and enforces a strict directory depth limit.
- Keyboard and mouse wheel scrolling, parent folder traversal, and immediate image loading directly into the active pane.

### High-Performance Rendering Architecture
- Texture caching: decoded once from disk and uploaded directly to GPU textures, with all viewport transformations handled via affine hardware transforms.
- Batched bitmap text engine: glyph pixels are pre-analyzed into contiguous horizontal spans and rendered using batched rectangle drawing calls, reducing draw calls by over ninety-nine percent compared to bit-by-bit blitting.
- Off-screen viewport culling: tiles panned outside the current view bounds are clipped and bypass GPU submission.
- Metadata caching: EXIF and filesystem statistics are cached while the information modal is open, eliminating redundant disk reads and parsing operations on every animation tick.

### Hardened Security and Robustness
- Process isolation: clipboard interactions completely eliminate shell execution, avoiding command injection vulnerabilities by spawning helper utilities directly through system process execution with argument vectors and anonymous UNIX pipes.
- Atomic temporary files: temporary clipboard assets use restrictive file creation permissions restricted to the current user and are cleaned up immediately across all execution branches.
- Input validation: command-line parameters, drag-and-drop payloads, and filter keystrokes undergo rigorous validation against buffer limits, non-printable characters, and invalid file descriptor types.
- Resilient EXIF parsing: embedded JPEG APP1 and TIFF parser detects circular IFD pointers, guards against zero-denominator rational divisions, bounds all memory offsets, and sanitizes ASCII strings.
- Graceful viewport degradation: viewport math prevents division by zero under degenerate or zero-size window dimensions.

### Architecture and Pitchfork Layout
The project strictly implements the Pitchfork Layout standard and out-of-source build principles:
- Implementation source files reside under the src directory.
- Public header interfaces reside under the include directory.
- Single-header third-party dependencies reside isolated under the external directory.
- Unit and regression test suites reside under the tests directory.
- Application desktop metadata and non-code assets reside under the assets directory.
- All compiled object files and output binaries reside in an out-of-source build directory.

## Supported Formats

Image decoding is provided by stb_image, supporting:
- JPEG and JPG (including EXIF orientation and metadata)
- PNG (including 8-bit and 16-bit channels)
- WebP
- BMP
- PPM, PGM, and PBM
- TIFF and TIF
- GIF (single-frame and first frame)
- TGA
- PSD
- HDR

## Prerequisites and System Requirements

The application is targeted for modern Linux distributions running X11 or Wayland display servers:
- C11 compliant compiler such as GCC version 11 or higher.
- POSIX make build tool.
- SDL2 development libraries (libsdl2-dev) version 2.0.18 or newer.
- Standard C math library.
- For optional system clipboard integration on Wayland: wl-clipboard utilities (wl-copy and wl-paste).
- For optional system clipboard integration on X11: xclip or xsel utilities.
- Embedded font and decoder headers are vendored within the external directory, requiring no additional external image or font libraries.

## Setup and Build Steps

Building the application follows standard POSIX procedures:
1. Ensure the SDL2 development package is installed through your system package manager.
2. From the repository root, invoke the default target of the make tool. The build system automatically constructs the build directory tree, compiles all source modules with standard optimizations, strict warnings, and C11 compliance, and outputs the final executable binary inside the build directory while placing a convenient symlink in the repository root.
3. To remove all compilation artifacts, object files, and generated binaries, invoke the clean target of the make tool.
4. To install the viewer binary and its desktop integration file into system application directories, invoke the install target with administrative privileges. The default installation prefix points to usr local, which can be overridden by specifying a custom prefix parameter for user-local installations in your home directory.
5. To uninstall the binary and its desktop launcher, invoke the uninstall target with the corresponding prefix parameter.

## Testing and Quality Verification

The repository contains an automated unit and regression test suite covering memory safety, process execution security, directory navigation, EXIF parsing, text rendering, and boundary edge cases:
- To run the complete test suite, invoke the test target of the makefile. This compiles the test runner with AddressSanitizer and UndefinedBehaviorSanitizer enabled and executes all verification test cases.
- For automated testing in headless environments where no display server is attached, SDL can be executed with its dummy video driver under a timeout wrapper to verify initialization and clean teardown.

## Command-Line Usage

The executable accepts either one or two image paths as arguments:
- Supplying a single image path launches the viewer in full-window single-pane mode.
- Supplying two image paths launches the viewer in dual-pane synchronized comparison mode, loading the first image on the left and the second image on the right.
- Arguments can be absolute or relative filesystem paths. Paths are validated before loading to ensure they point to regular, supported image files.

## Controls and Keybindings

### Viewer Controls
- Mouse Wheel: Zoom in or out centered on current cursor position.
- Left Mouse Drag: Pan the active image viewport.
- Key 0 or Capital F: Fit image to the current pane dimensions while preserving aspect ratio.
- Key 1: Reset zoom to one-to-one pixel scale centered in the pane.
- Plus and Minus Keys: Zoom in and zoom out centered in the window.
- Key F or F11: Toggle fullscreen display mode, preserving previous windowed geometry.
- Key I: Toggle bottom information bar showing image dimensions, color depth, format, zoom percentage, and navigation position.
- Key E: Toggle EXIF metadata overlay showing camera make, model, exposure settings, ISO speed, and capture timestamp.
- Key H or Question Mark: Toggle keyboard shortcut help modal.
- Key S: Toggle synchronization mode between synchronized transform and free transform.
- Tab Key: Switch the active pane when in free transform mode.
- Key N, Right Arrow, or Page Down: Navigate to the next valid image in the current folder.
- Key P, Left Arrow, or Page Up: Navigate to the previous valid image in the current folder.
- Escape Key: Close active overlays, exit fullscreen mode, or toggle the directory browser.
- Key Q: Terminate and exit the viewer.
- Drag and Drop: Drop an image file onto the left or right half of the window to replace the image in that pane.

### Directory Browser Controls
- Up Arrow, Down Arrow, Key K, Key J: Move selection highlight up or down.
- Right Arrow or Enter on Folder: Expand directory inline.
- Left Arrow or Backspace: Collapse expanded folder or navigate up to parent directory.
- Enter on Image File: Load chosen image directly into the active pane, dismiss browser, and fit view.
- Home and End: Jump to first or last directory item.
- Mouse Click: Select tree entry.
- Mouse Double-Click: Expand or collapse folder, or open selected image.
- Mouse Wheel: Scroll through directory list.
- Click Outside Browser Panel or Escape Key: Dismiss the browser overlay.
- Text Typing: Instantly filters directory entries by typed characters.

## License

This project is licensed under the terms of the MIT License. Refer to the LICENSE file for details.
