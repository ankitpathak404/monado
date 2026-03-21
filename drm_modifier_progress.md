# Monado DRM Format Modifier Investigation & Progress

## The Core Issue
StereoKit applications running on the Raspberry Pi (V3D GPU) failed with `eglCreateImageKHR failed` and `XR_ERROR_RUNTIME_FAILURE` when creating the swapchain. The fundamental issue is a mismatch in how Vulkan allocates optimally tiled images (using the proprietary V3D UIF format) and EGL's assumption of linear memory unless explicitly told otherwise.

## What Was Done
Instead of forcing linear tiling hacks, we fully plumbed the DRM Format Modifier through the Monado stack so EGL knows exactly how Vulkan allocated the image.

### 1. Struct and IPC Modifications
*   **Added Metadata fields:** `drm_format_modifier` and `row_pitch` were added to `xrt_image_native` and `vk_image` structs.
*   **Modified IPC Protocol:** Edited `50-swapchain.json` to serialize and deserialize the `modifier` and `row_pitch` from the server to the client.

### 2. Monado Server (`vk_image_allocator.c`)
*   Opted into `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` instead of `VK_IMAGE_TILING_OPTIMAL` when creating the Vulkan image.
*   Chained `VkImageDrmFormatModifierListCreateInfoEXT` to tell the driver which modifiers we support.
*   Successfully queried the actual driver-chosen format modifier via `vkGetImageDrmFormatModifierPropertiesEXT`.
*   Successfully queried the correct `row_pitch` using `vkGetImageSubresourceLayout`.

### 3. Monado Client (`comp_gl_eglimage_swapchain.c`)
*   Populated the EGL attributes dynamically utilizing `EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT` and `EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT` to pass the true format modifier we received from Vulkan.

### 4. The "Missing Extension" Bug and Fix
*   Despite all the work in Step 2, the DRM modifier was skipped silently at runtime.
*   Deep investigation tracking down to `ps` / `nm` / `strings` logs revealed that Monado's initialization sequence wasn't even attempting to enable `VK_EXT_image_drm_format_modifier`.
*   **Fix Applied:** We explicitly added `VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME` to the `optional_device_extensions` array in `src/xrt/compositor/main/comp_compositor.c`. We then re-compiled Monado.

### 5. Why Tests Repeatedly Failed
*   Even with the fix compiled, running `dotnet run` yielded the exact same `eglCreateImageKHR` behavior.
*   **Root Cause Discovered:** Monado utilizes an out-of-process background daemon named `monado-service`. We found an old occurrence of `monado-service` statically idling in the background from before our code changes!
*   Because the old service was running, `StereoKitTest` kept connecting to that legacy daemon rather than launching the new code, meaning the new DRM querying logic hasn't been truthfully executed yet.

## ⚠️ CRITICAL DISCOVERY: CMA EXHAUSTION
The recent issues with "Neon Blue" and "Purple" scrambled blocks are caused by **CMA (Contiguous Memory Allocator) Exhaustion**.
- **CmaFree:** 16.0 kB (Essentially empty).
- **Result:** The GPU crashes when trying to map tiled buffers (MMU error), causing color channel corruption.

## 🛠 FINAL FIX (REBOOT REQUIRED)
1. **Increase CMA Pool:**
   Run this once to set the graphics memory pool to 512MB:
   ```bash
   sudo sed -i 's/dtoverlay=vc4-kms-v3d/dtoverlay=vc4-kms-v3d,cma-512/' /boot/firmware/config.txt
   ```
2. **Reboot:**
   ```bash
   sudo reboot
   ```

## 🏁 Post-Reboot Verification
Check memory: `grep -i cma /proc/meminfo` (Should show ~512MB).
Then run the usual StereoKit command:
```bash
pkill -9 -x monado-service; rm -f /run/user/1000/monado_comp_ipc; monado-service & sleep 2 && XR_RUNTIME_JSON=/usr/local/share/openxr/1/openxr_monado.json dotnet run
```
