
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <vulkan/vulkan.h>
#include <print>
#include <format>
#include <vector>
#include <set>
#include <string>
#include <ranges>
#include <chrono>

#define BENCHMARKING 0

#define SOUND_CHECK 1

const char ConvolverShaderSource[] = {
#embed "convolver.cs.spirv"
};

// see https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit
#define FG(COLOR) std::format("\33[38:5:{}m", COLOR)
#define BG(COLOR) std::format("\33[48:5:{}m", COLOR)
#define ANSI_RESET "\33[0m"
#define DEFAULT_FG "\33[39m"
#define DEFAULT_BG "\33[49m"
#define HAS_FLAG(BITS, FLAG) ((BITS & FLAG) == FLAG)

void PrintShader()
{
    std::print("Here is my shader do you like it?\n\n");

    int i = 0;
    std::vector<char> Line;
    const int LastIndex = sizeof(ConvolverShaderSource) - 1;
    for (const char Symbol : ConvolverShaderSource)
    {
        if (i % 4 == 0)
        {
            std::print(" ");
        }
        std::string Color;
        if (Symbol >= 32 && Symbol <= 126)
        {
            Color = FG(5);
            Line.push_back(Symbol);
        }
        else
        {
            Color = (Symbol == 0) ? FG(8) : FG(15);
            Line.push_back('\0');
        }
        std::print("{}{:02x}{}", Color, (uint8_t)Symbol, ANSI_RESET);

        if (i % 16 == 15 || i == LastIndex)
        {
            int Remainder = 16 - Line.size();
            while (Remainder > 0)
            {
                std::print("{}             {}", FG(8), ANSI_RESET);
                Remainder -= 4;
            }
            std::print("  ");
            for (char Text : Line)
            {
                if (Text == '\0')
                {
                    std::print("{}{}{}", FG(8), '.', ANSI_RESET);
                }
                else
                {
                    std::print("{}{}{}{}", BG(0), FG(5), Text, ANSI_RESET);
                }
            }
            Line.clear();
            std::print("\n");
        }
        else
        {
            std::print(" ");
        }
        ++i;
    }
    std::print("{}\n", ANSI_RESET);
    std::print("I made it for you! :3\n");
}


struct CandidateDeviceInfo
{
    VkPhysicalDevice Device;
    std::string Name;
    uint32_t Score;
    uint32_t Queue;
    bool HasAnyCompute;
    bool HasDeviceLocalHostVisible;
};


#define TEARDOWN_FROM_INSTANCE() \
    vkDestroyInstance(Instance, nullptr);

#define TEARDOWN_FROM_DEVICE() \
    vkDestroyDevice(Device, nullptr); \
    TEARDOWN_FROM_INSTANCE()

#define TEARDOWN_FROM_PIPELINE() \
    vkDestroyPipeline(Device, ConvolverPipeline, nullptr); \
    TEARDOWN_FROM_DEVICE()

#define TEARDOWN_FROM_COMMAND_POOL() \
    vkDestroyCommandPool(Device, CommandPool, nullptr); \
    TEARDOWN_FROM_PIPELINE()


#define TEARDOWN_FROM_NOMINAL TEARDOWN_FROM_COMMAND_POOL


int main(int argc, char *argv[])
{
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("Could not initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_AudioStream* Stream = nullptr;
    {
        SDL_AudioSpec AudioSpec = {
            .format = SDL_AUDIO_F32,
            .channels = 1,
            .freq = 22050,
        };

        Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &AudioSpec, nullptr, nullptr);
        if (!Stream)
        {
            SDL_Log("Could not create audio stream: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        SDL_ResumeAudioStreamDevice(Stream);
    }

    std::set<std::string> RequestedLayers;
    {
        RequestedLayers.emplace("VK_LAYER_KHRONOS_validation");
        // RequestedLayers.emplace("VK_LAYER_RENDERDOC_Capture");
    }

    std::vector<VkLayerProperties> AvailableLayers;
    std::vector<char*> EnabledLayerNames;
    {
        uint32_t LayerCount = 0;

        VkResult Result = vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);
        AvailableLayers.resize(LayerCount);
        Result = vkEnumerateInstanceLayerProperties(&LayerCount, AvailableLayers.data());

        std::print("Available Instance Layers:\n");
        for (VkLayerProperties& Layer : AvailableLayers)
        {
            std::string LayerName(Layer.layerName);
            if (RequestedLayers.contains(LayerName))
            {
                EnabledLayerNames.push_back(Layer.layerName);
                std::print(" + [Enabled] {}{}{} ({})\n", FG(46), Layer.layerName, ANSI_RESET, Layer.description);
            }
            else
            {
                std::print(" {}- (Ignored) {} ({}){}\n", FG(240), Layer.layerName, Layer.description, ANSI_RESET);
            }
        }
        std::print("\n");
    }

    VkApplicationInfo ApplicationInfo =
    {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Convolver",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 0),
        .pEngineName = "Molly Time",
        .engineVersion = VK_MAKE_VERSION(0, 0, 0),
        .apiVersion = VK_MAKE_VERSION(1, 4, 0)
    };

    VkInstanceCreateInfo InstanceCreateInfo =
    {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &ApplicationInfo,
        .enabledLayerCount = (uint32_t)EnabledLayerNames.size(),
        .ppEnabledLayerNames = EnabledLayerNames.data(),
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr
    };

    VkInstance Instance;
    {
        VkResult Result = vkCreateInstance(
            &InstanceCreateInfo,
            nullptr,
            &Instance);

        if (Result != VK_SUCCESS)
        {
            std::print("Instance creation failed with error code: {}\n", (int)Result);
            TEARDOWN_FROM_INSTANCE();
            return 1;
        }
    }

    VkPhysicalDevice PhysicalDevice;
    uint32_t QueueFamilyIndex = -1;
    uint32_t HeapIndex = 0;
    uint32_t MemoryTypeIndex = 0;
    {
        uint32_t PhysicalDeviceCount = 0;
        std::vector<VkPhysicalDevice> AvailableDevices;
        VkResult Result = vkEnumeratePhysicalDevices(Instance, &PhysicalDeviceCount, nullptr);
        AvailableDevices.resize(PhysicalDeviceCount);
        Result = vkEnumeratePhysicalDevices(Instance, &PhysicalDeviceCount, AvailableDevices.data());

        std::vector<CandidateDeviceInfo> IdentifiedDevices;
        int32_t BestDevice = -1;
        uint32_t BestScore = (uint32_t)-1;

        for (VkPhysicalDevice AvailableDevice : AvailableDevices)
        {
            const int32_t DeviceIndex = IdentifiedDevices.size();
            CandidateDeviceInfo& Candidate = IdentifiedDevices.emplace_back();
            Candidate.Device = AvailableDevice;
            Candidate.Score = (uint32_t)-1;

            VkPhysicalDeviceProperties Properties;
            vkGetPhysicalDeviceProperties(AvailableDevice, &Properties);
            Candidate.Name = Properties.deviceName;

            Candidate.HasAnyCompute = false;
            std::vector<VkQueueFamilyProperties> QueueFamilyProperties;
            {
                uint32_t QueueFamilyPropertyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(AvailableDevice, &QueueFamilyPropertyCount, nullptr);
                QueueFamilyProperties.resize(QueueFamilyPropertyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(AvailableDevice, &QueueFamilyPropertyCount, QueueFamilyProperties.data());

                int QueueIndex = 0;
                Candidate.Queue = -1;
                for (VkQueueFamilyProperties FamilyProperties : QueueFamilyProperties)
                {
                    if ((FamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
                    {
                        Candidate.Queue = QueueIndex;
                        Candidate.HasAnyCompute = true;
                        break;
                    }
                    ++QueueIndex;
                }
            }

            Candidate.HasDeviceLocalHostVisible = false;
            VkPhysicalDeviceMemoryProperties MemoryProperties;
            vkGetPhysicalDeviceMemoryProperties(AvailableDevice, &MemoryProperties);
            //for (int MemoryTypeIndex = 0; MemoryTypeIndex < MemoryProperties.memoryTypeCount; ++MemoryTypeIndex)
            for (VkMemoryType MemoryType : MemoryProperties.memoryTypes | std::views::take(MemoryProperties.memoryTypeCount))
            {
                // Memory is fast for the GPU to access.
                const bool HasDeviceLocal = HAS_FLAG(MemoryType.propertyFlags, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

                // Memory allocated with this type can be memory mapped for CPU access.
                const bool HasHostVisible = HAS_FLAG(MemoryType.propertyFlags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

                // Memory mapping does not require flush and invalidation commands.
                const bool HasHostCoherent = HAS_FLAG(MemoryType.propertyFlags, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

                // Memory allocated with this type is cached.  Uncached memory is slower but always coherent.
                const bool HasHostCached = HAS_FLAG(MemoryType.propertyFlags, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

                // Seems at minimum we need DL+HV, unclear to what extent I care about HC+HA, and I don't care about the rest.
                // HC and HA are not mutually exclusive, but it seems at least one is required.

                // Of the applicable configurations on my laptop:
                //  - Xe supports DL+HV+HC and DL+HV+HC+HA
                //  - LLVM *only* supports DL+HV+HC+HA

                if (HasDeviceLocal && HasHostVisible)
                {
                    Candidate.HasDeviceLocalHostVisible = true;
                }
            }

            if (Candidate.HasAnyCompute && Candidate.HasDeviceLocalHostVisible)
            {
                if (Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                {
                    Candidate.Score = 0;
                }
                else if (Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                {
                    Candidate.Score = 1;
                }
                else
                {
                    Candidate.Score = 2;
                }

                if (BestDevice == -1 || (Candidate.Score < BestScore))
                {
                    BestDevice = DeviceIndex;
                    BestScore = Candidate.Score;
                }
            }
        }

        if (BestDevice == -1)
        {
            std::print("No suitable GPU available.\n");
            TEARDOWN_FROM_INSTANCE();
            return 1;
        }

        PhysicalDevice = IdentifiedDevices[BestDevice].Device;
        QueueFamilyIndex = IdentifiedDevices[BestDevice].Queue;

        std::print("Available Physical Devices:\n");
        for (uint32_t DeviceIndex = 0; DeviceIndex < IdentifiedDevices.size(); ++DeviceIndex)
        {
            CandidateDeviceInfo DeviceInfo = IdentifiedDevices[DeviceIndex];
            if (DeviceIndex == BestDevice)
            {
                std::print(" + [Selected] {}{}{}\n", FG(46), DeviceInfo.Name, ANSI_RESET);
            }
            else if (DeviceInfo.Score <= 1)
            {
                std::print(" {}~ (Adequate) {}{}\n", FG(240), DeviceInfo.Name, ANSI_RESET);
            }
            else if (DeviceInfo.Score <= 1)
            {
                std::print(" {}- (Rejected) {}{}\n", FG(240), DeviceInfo.Name, ANSI_RESET);
            }
        }

        {
            VkPhysicalDeviceMemoryProperties MemoryProperties;
            vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &MemoryProperties);

            const VkMemoryPropertyFlags TargetFlags = \
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            MemoryTypeIndex = 0;
            for (VkMemoryType MemoryType : MemoryProperties.memoryTypes | std::views::take(MemoryProperties.memoryTypeCount))
            {
                if (MemoryType.propertyFlags == TargetFlags)
                {
                    break;
                }
                ++MemoryTypeIndex;
            }

            HeapIndex = 0;
            for (VkMemoryHeap MemoryHeap : MemoryProperties.memoryHeaps | std::views::take(MemoryProperties.memoryHeapCount))
            {
                if (HAS_FLAG(MemoryHeap.flags, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
                {
                    break;
                }
                ++HeapIndex;
            }
        }
    }
    std::print("\n");

    VkDevice Device;
    {
        float Priority[] = { 1.0 };
        VkDeviceQueueCreateInfo QueueCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = QueueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = Priority,
        };

        VkPhysicalDeviceVulkan12Features PhysicalDeviceVulkan12Features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = nullptr,
            .shaderFloat16 = VK_TRUE,
            .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
            .bufferDeviceAddress = VK_TRUE
        };

        VkDeviceCreateInfo DeviceCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &PhysicalDeviceVulkan12Features,
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &QueueCreateInfo,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = 0,
            .ppEnabledExtensionNames = nullptr,
            .pEnabledFeatures = nullptr
        };

        VkResult Result = vkCreateDevice(PhysicalDevice, &DeviceCreateInfo, nullptr, &Device);
        if (Result != VK_SUCCESS)
        {
            std::print("Logical device creation failed with error code: {}\n", (int)Result);
            TEARDOWN_FROM_INSTANCE();
            return 1;
        }
    }

    VkQueue Queue;
    vkGetDeviceQueue(Device, QueueFamilyIndex, 0, &Queue);

    VkPipeline ConvolverPipeline;
    VkPipelineLayout ConvolverPipelineLayout;
    {
        PrintShader();

        VkShaderModule ShaderModule;
        {
            static_assert(sizeof(ConvolverShaderSource) % sizeof(uint32_t) == 0);
            VkShaderModuleCreateInfo CreateInfo = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = sizeof(ConvolverShaderSource),
                .pCode = (const uint32_t*)ConvolverShaderSource
            };
            VkResult Result = vkCreateShaderModule(Device, &CreateInfo, nullptr, &ShaderModule);
            if (Result != VK_SUCCESS)
            {
                std::print("Shader module creation failed with error code: {}\n", (int)Result);
                TEARDOWN_FROM_DEVICE();
                return 1;
            }
        }

        VkPushConstantRange PushConstantRange = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = 8
        };

        //VkPipelineLayout PipelineLayout;
        {
            VkPipelineLayoutCreateInfo CreateInfo = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 0,
                .pSetLayouts = nullptr,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &PushConstantRange
            };
            VkResult Result = vkCreatePipelineLayout(Device, &CreateInfo, nullptr, &ConvolverPipelineLayout);
            if (Result != VK_SUCCESS)
            {
                std::print("Pipeline layout creation failed with error code: {}\n", (int)Result);
                vkDestroyShaderModule(Device, ShaderModule, nullptr);
                TEARDOWN_FROM_DEVICE();
                return 1;
            }
        }

        {
            VkComputePipelineCreateInfo CreateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = ShaderModule,
                    .pName = "main",
                    .pSpecializationInfo = nullptr
                },
                .layout = ConvolverPipelineLayout,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = 0
            };
            VkResult Result = vkCreateComputePipelines(Device, VK_NULL_HANDLE, 1, &CreateInfo, nullptr, &ConvolverPipeline);
            vkDestroyShaderModule(Device, ShaderModule, nullptr);
            //vkDestroyPipelineLayout(Device, ConvolverPipelineLayout, nullptr);
            if (Result != VK_SUCCESS)
            {
                std::print("Shader pipeline creation failed with error code: {}\n", (int)Result);
                TEARDOWN_FROM_DEVICE();
                return 1;
            }
        }
    }

    VkCommandPool CommandPool;
    {
        VkCommandPoolCreateInfo CommandPoolCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = QueueFamilyIndex
        };
        VkResult Result = vkCreateCommandPool(Device, &CommandPoolCreateInfo, nullptr, &CommandPool);
        if (Result != VK_SUCCESS)
        {
            std::print("Command pool creation failed with error code: {}\n", (int)Result);
            TEARDOWN_FROM_PIPELINE();
            return 1;
        }
    }

    VkCommandBuffer CommandBuffers[2];
    {
        VkCommandBufferAllocateInfo CommandBufferAllocateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = CommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2
        };
        VkResult Result = vkAllocateCommandBuffers(Device, &CommandBufferAllocateInfo, CommandBuffers);
        if (Result != VK_SUCCESS)
        {
            std::print("Command buffer allocation failed with error code: {}\n", (int)Result);
            TEARDOWN_FROM_COMMAND_POOL();
            return 1;
        }
    }

    VkDeviceMemory SomeMemory;
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
            .allocationSize = 64,
            .memoryTypeIndex = MemoryTypeIndex
        };
        VkResult Result = vkAllocateMemory(Device, &AllocateInfo, nullptr, &SomeMemory);
        if (Result != VK_SUCCESS)
        {
            std::print("Allocation failed with error code: {}\n", (int)Result);
            TEARDOWN_FROM_DEVICE();
            return 1;
        }
    }

    void* SomeMappedMemory = nullptr;
    {
        VkResult Result = vkMapMemory(Device, SomeMemory, 0, 64, 0, &SomeMappedMemory);
    }

    VkBuffer SomeBuffer;
    {
        VkBufferCreateInfo CreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 64,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &QueueFamilyIndex
        };
        VkResult Result = vkCreateBuffer(Device, &CreateInfo, nullptr, &SomeBuffer);
        Result = vkBindBufferMemory(Device, SomeBuffer, SomeMemory, 0);
    }

    VkDeviceAddress SomeDeviceaddress;
    {
        VkBufferDeviceAddressInfo AddressInfo =
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext = nullptr,
            .buffer = SomeBuffer
        };
        SomeDeviceaddress = vkGetBufferDeviceAddress(Device, &AddressInfo);
    }

    {
        uint32_t* Fnord = (uint32_t*)SomeMappedMemory;
        Fnord[0] = 1;
        Fnord[1] = 1;
    }

    for (VkCommandBuffer& CommandBuffer : CommandBuffers)
    {
        VkCommandBufferBeginInfo BeginInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pInheritanceInfo = nullptr
        };
        vkBeginCommandBuffer(CommandBuffer, &BeginInfo);
        vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ConvolverPipeline);
        vkCmdPushConstants(CommandBuffer, ConvolverPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &SomeDeviceaddress);
        vkCmdDispatch(CommandBuffer, 1, 1, 1);
        vkEndCommandBuffer(CommandBuffer);
    }

    std::print("\nNow entering \"the cool zone\" (hot loop)...\n");

    VkFence FrameFence;
    {
        VkFenceCreateInfo CreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT
        };
        vkCreateFence(Device, &CreateInfo, nullptr, &FrameFence);
    }

#if BENCHMARKING
    const auto StartTime = std::chrono::steady_clock::now();
    const uint64_t FrameCount = 10000;
#else
    const uint64_t FrameCount = 64;
#endif

    for (uint64_t FrameNumber = 0; FrameNumber < FrameCount; ++FrameNumber)
    {
        vkResetFences(Device, 1, &FrameFence);
        VkCommandBuffer& CommandBuffer = CommandBuffers[FrameNumber % 2];
        VkSubmitInfo SubmitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = 0,
            .commandBufferCount = 1,
            .pCommandBuffers = &CommandBuffer,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr
        };
        vkQueueSubmit(Queue, 1, &SubmitInfo, FrameFence);

        VkResult Result = VK_TIMEOUT;
        while (Result == VK_TIMEOUT)
        {
            Result = vkWaitForFences(Device, 1, &FrameFence, VK_TRUE, 0);
        }
#if !BENCHMARKING
        {
            uint32_t* Fnord = (uint32_t*)SomeMappedMemory;
            std::print("Frame {}: {} {}\n", FrameNumber, Fnord[0], Fnord[1]);
        }
#endif
        if (Result != VK_SUCCESS)
        {
            break;
        }
    }

#if BENCHMARKING
    const auto StopTime = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> DeltaTime = StopTime - StartTime;
    double AverageTime = DeltaTime.count() / double(FrameCount);
    std::print("Iterations: {}\n", FrameCount);
    std::print("Average Time: {} milliseconds\n", AverageTime);
#endif

#if SOUND_CHECK
    {
        float Samples[22050];
        const int LastSample = 22050 - 1;
        const int Attack = int(float(LastSample) * 0.25);
        const int Decay = int(float(LastSample) * 0.5);
        const int DecayRange = LastSample - Decay;

        int Cursor = 0;

        for (int Index = 0; Index < SDL_arraysize(Samples); Index++) {
            const float Phase = (float)Cursor * 440.0f / 22050.0f;
            float Amplitude = 0.5f;

            if (Index <= Attack)
            {
                float Alpha = float(Index) / float(Attack);
                Amplitude *= Alpha;
            }
            else if (Index >= Decay)
            {
                float Alpha = 1.0 - (float(Index - Decay) / float(DecayRange));
                Amplitude *= Alpha * Alpha;
            }

            Samples[Index] = SDL_sinf(Phase * 2.0f * SDL_PI_F) * Amplitude;
            Cursor = (Cursor + 1) % 22050;
        }

        SDL_PutAudioStreamData(Stream, Samples, sizeof(Samples));

        SDL_FlushAudioStream(Stream);

        SDL_Event Event;
        int RemainingBytes = 1;
        do
        {
            RemainingBytes = SDL_GetAudioStreamQueued(Stream);
            SDL_PollEvent(&Event);
        }
        while (RemainingBytes > 0);
    }
#endif

    vkDestroyPipelineLayout(Device, ConvolverPipelineLayout, nullptr);
    vkDestroyBuffer(Device, SomeBuffer, nullptr);
    vkUnmapMemory(Device, SomeMemory);
    vkFreeMemory(Device, SomeMemory, nullptr);

    vkDestroyFence(Device, FrameFence, nullptr);


    TEARDOWN_FROM_NOMINAL();

    SDL_DestroyAudioStream(Stream);

    std::print("Done!\n");
    return 0;
}
