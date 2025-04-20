
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_events.h>

#include <vulkan/vulkan.h>
#include <print>
#include <format>
#include <vector>
#include <set>
#include <string>
#include <ranges>
#include <chrono>
#include <algorithm>

#define BENCHMARKING 1
#define REALTIME_MODE 1

#define DIV_UP(X, Y) ((X + Y - 1) / Y)

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


struct PushConstantsUpload
{
    VkDeviceAddress BufferA;
    VkDeviceAddress BufferB;
    VkDeviceAddress BufferC;
    int32_t SizeA;
    int32_t SizeB;
    int32_t SizeC;
    int32_t Start;
    int32_t Range;
    float Gain;
};


struct WaveData
{
    std::vector<float> Samples;

    WaveData()
    {
    }

    WaveData(const SDL_AudioSpec& TargetSpec, const char* Path)
    {
        SDL_AudioStream* Stream = nullptr;

        const std::string FullPath = std::format("{}{}", SDL_GetBasePath(), Path);
        SDL_AudioSpec ImportSpec;
        uint8_t* ImportData = nullptr;
        uint32_t ImportSize = 0;

        bool Error = true;
        if (SDL_LoadWAV(FullPath.c_str(), &ImportSpec, &ImportData, &ImportSize))
        {
            std::print("Opening {}\n", FullPath);
            std::print(" - Frequency: {} -> {}\n", ImportSpec.freq, TargetSpec.freq);
            std::print(" - Channels: {} -> {}\n", ImportSpec.channels, TargetSpec.channels);
            std::print(" - Float: {} -> {}\n",
                       bool(SDL_AUDIO_ISFLOAT(ImportSpec.format)), bool(SDL_AUDIO_ISFLOAT(TargetSpec.format)));
            std::print(" - Word Size: {} -> {}\n",
                       SDL_AUDIO_BYTESIZE(ImportSpec.format), SDL_AUDIO_BYTESIZE(TargetSpec.format));
            std::print("\n");

            size_t SampleCount = ImportSize / SDL_AUDIO_FRAMESIZE(ImportSpec);
            Samples.resize(SampleCount);

            SDL_AudioStream* Converter = SDL_CreateAudioStream(&ImportSpec, &TargetSpec);
            {
                uint32_t ImportFrameSize = SDL_AUDIO_FRAMESIZE(ImportSpec);
                uint32_t TargetFrameSize = SDL_AUDIO_FRAMESIZE(TargetSpec);
                for (int i = 0; i < SampleCount; ++i)
                {
                    uint32_t ImportOffset = ImportFrameSize * i;
                    uint32_t TargetOffset = TargetFrameSize * i;
                    uint8_t* TargetData = (uint8_t*)Samples.data();
                    SDL_PutAudioStreamData(Converter, (ImportData + ImportOffset), ImportFrameSize);
                    SDL_FlushAudioStream(Converter);
                    SDL_GetAudioStreamData(Converter, (TargetData + TargetOffset), TargetFrameSize);
                }
                Error = false;
            }
            SDL_DestroyAudioStream(Converter);
        }

        if (Error)
        {
            Samples.clear();
            std::print("Couldn't load {}: {}\n", FullPath, SDL_GetError());
        }
    }
};


int main(int argc, char *argv[])
{
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        std::print("Could not initialize SDL: {}", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    const int SampleRate = 22050;
    SDL_AudioStream* OutStream = nullptr;
    WaveData WaveA;
    WaveData WaveB;

    {
        SDL_AudioSpec OutSpec = {
            .format = SDL_AUDIO_F32,
            .channels = 1,
            .freq = SampleRate,
        };

        OutStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &OutSpec, nullptr, nullptr);
        if (!OutStream)
        {
            std::print("Could not create audio stream: {}", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        SDL_ResumeAudioStreamDevice(OutStream);

        WaveA = WaveData(OutSpec, "generations_stereo.wav");
        WaveB = WaveData(OutSpec, "bell.wav");
    }

    if (WaveA.Samples.size() == 0 || WaveB.Samples.size() == 0)
    {
        return SDL_APP_FAILURE;
    }
    std::reverse(WaveB.Samples.begin(), WaveB.Samples.end());

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
            .size = sizeof(PushConstantsUpload)
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

    SharedMemory<float>* BufferA = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, WaveA.Samples);
    SharedMemory<float>* BufferB = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, WaveB.Samples);

    if (!BufferA->IsValid)
    {
        std::print("Failed to allocate `BufferA`\n");
        TEARDOWN_FROM_DEVICE();
    }

    if (!BufferB->IsValid)
    {
        std::print("Failed to allocate `BufferB`\n");
        TEARDOWN_FROM_DEVICE();
    }

    // This determines the latency vs throughput tradeoff.

    const int32_t GroupSize = 32;
    const size_t MaxSizeC = BufferA->ElementCount + BufferB->ElementCount - 1;

#if REALTIME_MODE
    // Lowest latency
    const float IdealMinFrameDurationMs = 16.0f; // Raise this if you have hitching problems.
    const int32_t TargetSamplesPerFrame = int32_t(float(SampleRate) / 1000.0f * IdealMinFrameDurationMs);
    const int32_t MinGroupsPerFrame = 1;
    const int32_t GroupsPerFrame = std::max(MinGroupsPerFrame, int32_t(DIV_UP(TargetSamplesPerFrame, GroupSize)));
#else
    // Lowest total time
    const int32_t MaxGroupsPerFrame = 65535;
    const int32_t GroupsPerFrame = std::min(int32_t(DIV_UP(MaxSizeC, GroupSize)), MaxGroupsPerFrame);
#endif

    const int32_t SamplesPerFrame = GroupSize * GroupsPerFrame;
    const int32_t FrameCount = uint32_t(DIV_UP(MaxSizeC, SamplesPerFrame));
    const double FrameSpan = double(SamplesPerFrame) / double(SampleRate) * 1000.0;

    SharedMemory<float>* BufferC = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, SamplesPerFrame);

    if (BufferC->IsValid)
    {
        for (int i = 0; i < BufferC->ElementCount; ++i)
        {
            BufferC->Mapped[i] = 0.0f;
        }
    }
    else
    {
        std::print("Failed to allocate `BufferC`\n");
        TEARDOWN_FROM_DEVICE();
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
    auto TotalTimeMs = std::chrono::duration<double, std::milli>::zero();
    auto WorstFrame = std::chrono::duration<double, std::milli>::zero();
    auto BestFrame = std::chrono::duration<double, std::milli>::max();
    int32_t RecordedSamples = 0;
    int32_t HitchCount = 0;
#endif

#if 1
    bool Shutdown = false;
    int32_t FrameNumber = 0;
    for (; FrameNumber < FrameCount; ++FrameNumber)
    {
        SDL_Event Event;
        SDL_PollEvent(&Event);
        if (Event.type == SDL_EVENT_QUIT)
        {
            std::print("\nUser Requested Quit\n\n");
            Shutdown = true;
            break;
        }

        int32_t Start = FrameNumber * SamplesPerFrame;
        int32_t Range = std::min(std::max(int32_t(MaxSizeC) - Start, 0), SamplesPerFrame);
        int32_t GroupsThisFrame = DIV_UP(Range, GroupSize);
        if (Range == 0)
        {
            std::print("Ran out of data (frame {}/{}), shutting down hot loop.\n", FrameNumber, FrameCount);
            break;
        }
        bool PartialFrame = Range < SamplesPerFrame || Start < BufferB->ElementCount;

#if BENCHMARKING
        const auto FrameStartTime = std::chrono::steady_clock::now();
#endif

        vkResetFences(Device, 1, &FrameFence);
        VkCommandBuffer& CommandBuffer = CommandBuffers[FrameNumber % 2];

        {
            PushConstantsUpload Upload =
            {
                .BufferA = BufferA->DeviceAddress,
                .BufferB = BufferB->DeviceAddress,
                .BufferC = BufferC->DeviceAddress,
                .SizeA = int32_t(BufferA->ElementCount),
                .SizeB = int32_t(BufferB->ElementCount),
                .SizeC = int32_t(BufferC->ElementCount),
                .Start = Start,
                .Range = Range,
                .Gain = 1.0f / 150.0f
            };

            VkCommandBufferBeginInfo BeginInfo =
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = 0,
                .pInheritanceInfo = nullptr
            };
            vkBeginCommandBuffer(CommandBuffer, &BeginInfo);
            vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ConvolverPipeline);
            vkCmdPushConstants(CommandBuffer, ConvolverPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Upload), &Upload);
            vkCmdDispatch(CommandBuffer, GroupsThisFrame, 1, 1);
            vkEndCommandBuffer(CommandBuffer);
        }

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

#if BENCHMARKING
        if (!PartialFrame)
        {
            const auto FrameStopTime = std::chrono::steady_clock::now();
            const std::chrono::duration<double, std::milli> DeltaTime = FrameStopTime - FrameStartTime;
            TotalTimeMs += DeltaTime;
            ++RecordedSamples;

            if (DeltaTime.count() > FrameSpan)
            {
                ++HitchCount;
            }

            if (DeltaTime < BestFrame)
            {
                BestFrame = DeltaTime;
            }
            if (DeltaTime > WorstFrame)
            {
                WorstFrame = DeltaTime;
            }
        }
#endif

        if (Result != VK_SUCCESS)
        {
            std::print("vkWaitForFences returned an error?\n");
            break;
        }

        SDL_PutAudioStreamData(OutStream, BufferC->Mapped, sizeof(float) * Range);
        //SDL_FlushAudioStream(OutStream);
    }
#endif

#if BENCHMARKING
    {
        if (RecordedSamples == 0)
        {
            std::print("Not enough samples recorded for benchmarking.\n\n");
        }

        std::print("\tSamples per frame: {}\n", SamplesPerFrame);
        std::print("\t Groups per frame: {}\n", GroupsPerFrame);
        double AverageTime = 0.0;

        if (RecordedSamples > 0)
        {
            double TotalTime = TotalTimeMs.count();
            AverageTime = TotalTime / double(RecordedSamples);

            std::string TotalTimeUnit = "milliseconds";
            if (TotalTime >= 1000.0)
            {
                TotalTime /= 1000.0;
                TotalTimeUnit = "seconds";
                if (TotalTime >= 60.0)
                {
                    TotalTime /= 60.0;
                    TotalTimeUnit = "minutes";
                    if (TotalTime >= 60.0)
                    {
                        TotalTime /= 60.0;
                        TotalTimeUnit = "hours!?";
                    }
                }
            }

            std::print("\n");
            std::print("\t Frames processed: {}/{}\n", FrameNumber + 1, FrameCount);
            std::print("\t Samples recorded: {}\n", RecordedSamples);
            std::print("\t    Average frame: {:.3f} milliseconds\n", AverageTime);
            std::print("\t       Best frame: {:.3f} milliseconds\n", BestFrame.count());
            std::print("\t      Worst frame: {:.3f} milliseconds\n", WorstFrame.count());
            std::print("\t       Total time: {:.3f} {}\n\n", TotalTime, TotalTimeUnit);
        }

        std::print("\t Frame audio time: {:.3f} milliseconds\n\n", FrameSpan);

        if (RecordedSamples > 0)
        {
            if (BestFrame.count() > FrameSpan)
            {
                std::print("The best frame time is higher than the frame's playback duration!!\n");
            }
            else if (AverageTime > FrameSpan)
            {
                std::print("The average frame time is higher than the frame's playback duration!\n");
            }
            else if (WorstFrame.count() > FrameSpan)
            {
                std::print("The worst frame time is higher than the frame's playback duration!\n");
            }
        }

        if (HitchCount > 0)
        {
            std::print("Over budget frame count: {}\n\n", HitchCount);
        }
    }
#endif

    if (!Shutdown)
    {
        int RemainingBytes = 1;
        do
        {
            SDL_Event Event;
            if (SDL_WaitEventTimeout(&Event, 250))
            {
                if (Event.type == SDL_EVENT_QUIT)
                {
                    break;
                }
            }
            RemainingBytes = SDL_GetAudioStreamQueued(OutStream);
        }
        while (RemainingBytes > 0);
    }

    vkDestroyPipelineLayout(Device, ConvolverPipelineLayout, nullptr);
    delete BufferA;
    delete BufferB;
    delete BufferC;

    vkDestroyFence(Device, FrameFence, nullptr);


    TEARDOWN_FROM_NOMINAL();

    SDL_DestroyAudioStream(OutStream);

    std::print("Done!\n");
    return 0;
}
