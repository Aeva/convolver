
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <print>


template <typename ElementType>
struct SharedMemory
{
    VkDevice Device;
    VkDeviceMemory DeviceMemory;
    VkBuffer Buffer;
    VkDeviceAddress DeviceAddress;
    ElementType* Mapped = nullptr;
    int InitLevel = 0;
    bool IsValid = false;
    const size_t ElementCount = 0;
    const size_t ByteSize = 0;


    SharedMemory(VkDevice InDevice, uint32_t MemoryTypeIndex, uint32_t QueueFamilyIndex, size_t InElementCount)
        : Device(InDevice)
        , ElementCount(InElementCount)
        , ByteSize(sizeof(ElementType) * ElementCount)
    {
        VkResult Result = VK_SUCCESS;
        {
            VkMemoryAllocateFlagsInfo AllocateFlagsInfo =
            {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .pNext = nullptr,
                .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
                .deviceMask = 0
            };

            VkMemoryAllocateInfo AllocateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &AllocateFlagsInfo,
                .allocationSize = ByteSize,
                .memoryTypeIndex = MemoryTypeIndex
            };
            Result = vkAllocateMemory(Device, &AllocateInfo, nullptr, &DeviceMemory);
            if (Result != VK_SUCCESS)
            {
                std::print("Allocation failed with error code: {}\n", (int)Result);
            }
            else
            {
                ++InitLevel;
            }
        }
        if (Result == VK_SUCCESS)
        {
            void* VoidStar;
            Result = vkMapMemory(Device, DeviceMemory, 0, ByteSize, 0, &VoidStar);
            Mapped = (ElementType*)VoidStar;
            if (Result != VK_SUCCESS)
            {
                std::print("Memory mapping failed: {}\n", (int)Result);
            }
        }
        if (Result == VK_SUCCESS)
        {
            VkBufferCreateInfo CreateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = ByteSize,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 1,
                .pQueueFamilyIndices = &QueueFamilyIndex
            };
            Result = vkCreateBuffer(Device, &CreateInfo, nullptr, &Buffer);
            if (Result != VK_SUCCESS)
            {
                std::print("Buffer creation failed: {}\n", (int)Result);
            }
            else
            {
                ++InitLevel;
            }
        }
        if (Result == VK_SUCCESS)
        {
            Result = vkBindBufferMemory(Device, Buffer, DeviceMemory, 0);
            if (Result != VK_SUCCESS)
            {
                std::print("Failed to bind buffer memory: {}\n", (int)Result);
            }
        }
        if (Result == VK_SUCCESS)
        {
            VkBufferDeviceAddressInfo AddressInfo =
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = Buffer
            };
            DeviceAddress = vkGetBufferDeviceAddress(Device, &AddressInfo);
            ++InitLevel;
        }
        IsValid = (Result == VK_SUCCESS);
        if (!IsValid)
        {
            Free();
        }
    }


    SharedMemory(VkDevice InDevice, uint32_t MemoryTypeIndex, uint32_t QueueFamilyIndex, size_t InElementCount, ElementType Zero)
        : SharedMemory(InDevice, MemoryTypeIndex, QueueFamilyIndex, InElementCount)
    {
        if (IsValid)
        {
            for (int i = 0; i < ElementCount; ++i)
            {
                Mapped[i] = Zero;
            }
        }
    }


    SharedMemory(VkDevice InDevice, uint32_t MemoryTypeIndex, uint32_t QueueFamilyIndex, std::vector<ElementType> Upload)
        : SharedMemory(InDevice, MemoryTypeIndex, QueueFamilyIndex, Upload.size())
    {
        if (IsValid)
        {
            for (int i = 0; i < ElementCount; ++i)
            {
                Mapped[i] = Upload[i];
            }
        }
    }


    void Free()
    {
        if (InitLevel >= 1)
        {
            vkDestroyBuffer(Device, Buffer, nullptr);
        }
        if (InitLevel == 3)
        {
            vkUnmapMemory(Device, DeviceMemory);
        }
        if (InitLevel >= 2)
        {
            vkFreeMemory(Device, DeviceMemory, nullptr);
        }
        InitLevel = 0;
        IsValid = false;
    }


    ~SharedMemory()
    {
        Free();
    }
};
