# Monado & StereoKit on Raspberry Pi (V3D + Wayland)
**Comprehensive Setup & Troubleshooting Guide**

This document serves as the final record of the environment configuration, rendering bugs, and source-code modifications required to get OpenXR (StereoKit) running flawlessly via Monado on a Raspberry Pi using the V3D Vulkan/OpenGL drivers under a Wayland session.

---

## 1. System Environment & Root Causes
The Raspberry Pi graphics stack (Mesa V3D) has specific memory and tiling constraints. When running Monado (Vulkan compositor) and StereoKit (OpenGL ES client) over IPC, several deep stack issues collided to corrupt the display:

1. **CMA Exhaustion:** The Pi allocates contiguous memory for graphics (CMA). The default size is too small for VR swapchains, leading to MMU mapping failures and "neon blue" or scrambled pixel noise.
2. **EGL Tiling Mismatch:** Vulkan optimally allocates memory using V3D non-linear tiling (e.g., `BROADCOM_UIF`). However, when the OpenXR client (StereoKit) imports this memory via EGL (`eglCreateImageKHR`), EGL assumes a linear layout. This mismatch halves or skews the rendering stride.
3. **Wayland 10-bit Color Bug:** When booting into a Wayland session, the display server advertises 10-bit color support. Monado's desktop mirror window prioritizes 10-bit color (`VK_FORMAT_A2B10G10R10_UNORM_PACK32`) to reduce banding. However, the V3D Wayland driver has a bug displaying 10-bit packed formats, resulting in a horizontally smeared, half-black desktop window.

---

## 2. The Applied Fixes

### Fix A: Increasing CMA Memory (Kernel Level)
To prevent the GPU from failing to map buffers and corrupting the color channels:
1. Edit the boot config: `sudo nano /boot/firmware/config.txt`
2. Update the overlay to request 512MB of CMA:
   ```ini
   dtoverlay=vc4-kms-v3d,cma-512
   ```
3. Restart the Pi for the kernel graphics memory pool to expand.

### Fix B: Plumbing DRM Modifiers (Monado IPC)
To allow the Vulkan compositor to communicate memory layouts to clients:
- Modified `src/xrt/ipc/shared/proto/50-swapchain.json` to include `drm_format_modifier` and `row_pitch`.
- Updated `vk_helpers.c` (`vk_create_image_from_native`) to explicitly chain `VkImageDrmFormatModifierExplicitCreateInfoEXT` so client Vulkan apps reconstruct the exact memory plane pitch.

### Fix C: Forcing Linear Allocations for EGL (Monado Source)
Because StereoKit relies on OpenGL ES, the `eglCreateImageKHR` import still struggled with V3D's proprietary modifiers.
- **Modification:** In `src/xrt/auxiliary/vk/vk_image_allocator.c` (`create_image`), we forced the allocator's `supported_modifiers` list to only contain `DRM_FORMAT_MOD_LINEAR`.
- **Result:** The compositor now allocates linear swapchain memory, perfectly matching what the EGL client expects, eliminating the diagonal layout skew.

### Fix D: Disabling 10-bit Wayland Windows (Monado Source)
To stop the Wayland desktop mirror window from tearing into a noisy split-screen:
- **Modification:** In `src/xrt/compositor/main/comp_settings.c`, we moved `VK_FORMAT_A2B10G10R10_UNORM_PACK32` to the bottom of the priority list.
- **Result:** `monado-service` now defaults to standard 8-bit `VK_FORMAT_B8G8R8A8_UNORM`, bypassing the V3D Wayland 10-bit presentation bug.

---

## 3. Running the Stack

Whenever you are testing or running the environment, you must ensure that no old instances of the compositor are holding the IPC socket hostage.

### Step 1: Start the Monado Compositor Service
Run this in **Terminal 1**. It strictly kills existing daemons, wipes the stale socket, and runs our newly patched binary.
```bash
# Kill stale service and wipe IPC socket
killall -9 monado-service
rm -f /run/user/1000/monado_comp_ipc

# Start the patched service
cd /home/ankit/monado
./build/src/xrt/targets/service/monado-service
```

### Step 2: Run the StereoKit Application
Run this in **Terminal 2**. You **must** supply `XR_RUNTIME_JSON` so the OpenXR loader routes to our locally compiled Monado binary instead of the system default.
```bash
cd /home/ankit/monado/StereoKitTest

# Point OpenXR to the patched Monado and launch
XR_RUNTIME_JSON=/home/ankit/monado/build/openxr_monado-dev.json dotnet run
```

*(Note: If testing a specific C# project folder inside StereoKitTest instead of the root, append `--project SKProjectName/SKProjectName.csproj` to the `dotnet run` command).*
