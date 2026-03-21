#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    VkInstance instance;
    VkInstanceCreateInfo createInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    vkCreateInstance(&createInfo, NULL, &instance);
    
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    VkPhysicalDevice* devices = malloc(deviceCount * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices);
    if(deviceCount == 0) { printf("No vulkan devices\n"); return 1; }
    VkPhysicalDevice phys = devices[0];
    
    float queuePriorities = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriorities;
    
    VkDeviceCreateInfo devInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;
    
    VkDevice device;
    if (vkCreateDevice(phys, &devInfo, NULL, &device) != VK_SUCCESS) {
        printf("Failed to create device\n");
        return 1;
    }
    
    VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
    imgInfo.extent.width = 1280;
    imgInfo.extent.height = 720;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage image;
    if (vkCreateImage(device, &imgInfo, NULL, &image) != VK_SUCCESS) {
        printf("vkCreateImage failed for width 1280! Wait, let's try without COLOR_ATTACHMENT_BIT.\n");
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &imgInfo, NULL, &image) != VK_SUCCESS) {
            printf("vkCreateImage still failed.\n");
            return 1;
        }
        printf("vkCreateImage succeeded WITHOUT COLOR_ATTACHMENT_BIT.\n");
    } else {
        printf("vkCreateImage succeeded with COLOR_ATTACHMENT_BIT.\n");
    }
    
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, image, &sub, &layout);
    
    printf("Width 1280 (bpp 4, width*bpp = 5120), linear rowPitch = %zu\n", (size_t)layout.rowPitch);
    
    // try width 1000
    imgInfo.extent.width = 1000;
    if (vkCreateImage(device, &imgInfo, NULL, &image) == VK_SUCCESS) {
        vkGetImageSubresourceLayout(device, image, &sub, &layout);
        printf("Width 1000 (bpp 4, width*bpp = 4000), linear rowPitch = %zu\n", (size_t)layout.rowPitch);
    }
    
    return 0;
}
