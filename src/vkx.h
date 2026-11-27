/*
 * vkx — optional extension hooks for modules linked into vkskeleton.so
 * (e.g. the VR module of vkskeleton-vr).  All hooks are weak: without a
 * module they are NULL and the renderer behaves exactly as before.
 */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

/* Extra device extensions to enable (NULL-terminated array or NULL). */
__attribute__((weak)) const char *const *vkx_device_extensions(void);

/* Device created (called once, on the game thread). */
__attribute__((weak)) void vkx_on_device(VkInstance instance,
                                         VkPhysicalDevice physdev,
                                         VkDevice device, VkQueue queue,
                                         uint32_t queue_family,
                                         PFN_vkGetDeviceProcAddr gdpa);

/* A frame is about to be submitted.  The render pass has ended; `cb` is
 * still recording.  `swap_image` (layout PRESENT_SRC_KHR, TRANSFER_SRC usage
 * when swap_can_copy) holds the finished frame and must be left in
 * PRESENT_SRC_KHR.  `fence` signals when this frame's GPU work completes.
 * `proj_tan` = {left, right, down, up} tangents of the last perspective
 * projection used this frame (0 if none).  `eye_image` is VK_NULL_HANDLE, or
 * the eye views asked for by vkx_eyes, side by side (`eye_target` in total),
 * format `format`, layout TRANSFER_SRC_OPTIMAL (leave it so). */
__attribute__((weak)) void vkx_frame_end(VkCommandBuffer cb, VkImage swap_image,
                                         VkFormat format, VkExtent2D extent,
                                         VkFence fence,
                                         const float proj_tan[4],
                                         VkImage eye_image,
                                         VkExtent2D eye_target);

/* Eye views.  Called on the game thread when a frame is handed to the GPU.
 * Return 0, 1 or 2: the frame is replayed that many extra times, eye i into
 * the area x = i * eye_extent.width of eye_image.  Perspective draws use the
 * eye's frustum (keeping the game's near/far) and modelview eye * MV, where
 * `eye` (column-major) maps the game's eye space to this eye's eye space;
 * 2D (orthographic) draws are replayed unchanged. */
typedef struct vkx_eye {
    float tan[4];       /* left, right, down, up tangents (left/down < 0) */
    float eye[16];
} vkx_eye_t;
__attribute__((weak)) int vkx_eyes(VkExtent2D *eye_extent, vkx_eye_t eyes[2]);

/* View hook: called inside the V_CalcRefdef wrapper after the game computed
 * its view.  `ref_params` is the HL SDK ref_params_t (vieworg floats 0-2,
 * viewangles 3-5, forward/right/up 6-14, frametime 15). */
__attribute__((weak)) void vkx_view(float *ref_params);
