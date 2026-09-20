#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// Vulkan sharpen pass.
//
// Filters the presentable image this vkQueuePresentKHR is about to hand to the
// WSI, before inject capture copies it and before the overlay draws into it, so
// the recording and the screen show the same filtered frame and CE's own
// overlay pixels are never sharpened.
//
// The work is submitted on the queue the application presents from, chained
// into the present's own wait list exactly like the capture and overlay
// submissions are.

// Returns true when the filter was submitted, in which case `*signaledSemaphore`
// is the semaphore the next stage (or the present) must wait on.
bool SharpenPresentedFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage image,
                           uint32_t imageIndex, VkFormat format, VkColorSpaceKHR colorSpace, VkExtent2D extent,
                           uint32_t imageCount, const VkImage* images, const VkSemaphore* waitSemaphores,
                           uint32_t waitSemaphoreCount, VkSemaphore* signaledSemaphore);

// Releases everything the pass owns for `device`. Called from the same places
// that tear the overlay down, because the pass holds views of the swapchain's
// presentable images and a copy of the frame.
void CleanupSharpen(VkDevice device);

// Drops the pass's state for a swapchain that is being destroyed while the
// device stays alive.
void CleanupSharpenForSwapchain(VkDevice device, VkSwapchainKHR swapchain);
