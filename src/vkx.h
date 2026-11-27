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
 * projection used this frame (0 if none). */
__attribute__((weak)) void vkx_frame_end(VkCommandBuffer cb, VkImage swap_image,
                                         VkFormat format, VkExtent2D extent,
                                         VkFence fence,
                                         const float proj_tan[4]);

/* View hook: called inside the V_CalcRefdef wrapper after the game computed
 * its view.  `ref_params` is the HL SDK ref_params_t (vieworg floats 0-2,
 * viewangles 3-5, forward/right/up 6-14, frametime 15). */
__attribute__((weak)) void vkx_view(float *ref_params);
