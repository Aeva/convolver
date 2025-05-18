
#define LIVE_STREAM_MODE 1
#define BENCHMARKING 1

#define VALIDATION_LAYERS 0

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_events.h>

#include <vulkan/vulkan.h>

#include <print>
#include <format>
#include <vector>
#include <set>
#include <cstring>
#include <string>
#include <ranges>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <thread>

#include "assorted.h"
#include "shader.h"
#include "wave_stuff.h"
#include "shared_memory.h"


#if LIVE_STREAM_MODE
#include "pipewire.h"
#endif


const float IdealMinFrameDurationMs = 8.0f; // Raise this if you have hitching problems.
const int32_t MinGroupsPerFrame = 1;


struct ConvolverParameters
{
    const int SampleRate;
    const int32_t GroupSize;

    const int32_t TargetSamplesPerFrame;
    const int32_t TargetBytesPerFrame;
    const int32_t GroupsPerFrame;
    const int32_t SamplesPerFrame;
    const double FrameSpan;
    const int32_t BytesPerFrame;

    ConvolverParameters(int InSampleRate = 48000, int32_t InGroupSize = GROUP_SIZE)
        : SampleRate(InSampleRate)
        , GroupSize(InGroupSize)
        , TargetSamplesPerFrame(int32_t(float(SampleRate) / 1000.0f * IdealMinFrameDurationMs))
        , TargetBytesPerFrame(TargetSamplesPerFrame * sizeof(float))
        , GroupsPerFrame(std::max(MinGroupsPerFrame, int32_t(DIV_UP(TargetSamplesPerFrame, GroupSize))) * GroupSize)
        , SamplesPerFrame(GroupsPerFrame)
        , FrameSpan(double(SamplesPerFrame) / double(SampleRate) * 1000.0)
        , BytesPerFrame(sizeof(float) * SamplesPerFrame)
    {
    }

    void Reset(int InSampleRate = 48000, int32_t InGroupSize = GROUP_SIZE)
    {
        new(this) ConvolverParameters(InSampleRate, InGroupSize);
    }
};


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


struct PushConstantsUpload
{
    VkDeviceAddress BufferA;
    VkDeviceAddress BufferB;
    VkDeviceAddress BufferC;
    int32_t SizeA;
    int32_t SizeB;
    int32_t SizeC;
    int32_t Start;
};


int main(int argc, char *argv[])
{
#if LIVE_STREAM_MODE
    PipeWireInit(argc, argv);
#endif

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Convolver");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Magic");

    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS))
    {
        std::print("Could not initialize SDL: {}", SDL_GetError());
        return SDL_APP_FAILURE;
    }

#if !LIVE_STREAM_MODE
    SDL_AudioStream* InStream = nullptr;
    SDL_AudioStream* OutStream = nullptr;
    WaveStream* WaveA = nullptr;
#endif
    WaveData WaveB;

    ConvolverParameters Params;
    {
        SDL_AudioSpec OutSpec =
        {
            .format = SDL_AUDIO_F32,
            .channels = 1,
            .freq = 0, // set by WaveB
        };

        int ImportFrequency;
        if (argc >= 2)
        {
            WaveB = WaveData(OutSpec, argv[1], true);
        }
        else
        {
#if LIVE_STREAM_MODE
            WaveB = WaveData(OutSpec, "revolver.wav", true);
#else
            WaveB = WaveData(OutSpec, "chest.wav", true);
#endif
        }

#if !LIVE_STREAM_MODE
        WaveA = new WaveStream(OutSpec, "strange_birds.wav");
        InStream = WaveA->Stream;
        OutStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &OutSpec, nullptr, nullptr);
        if (!OutStream)
        {
            std::print("Could not create output audio stream: {}", SDL_GetError());
            return SDL_APP_FAILURE;
        }
#endif

        Params.Reset(OutSpec.freq);
    }

#if LIVE_STREAM_MODE
    if (WaveB.Samples.size() == 0)
    {
        std::print("Unable to open impulse response file.\n");
        return SDL_APP_FAILURE;
    }
#else
    if (InStream == nullptr || WaveB.Samples.size() == 0)
    {
        std::print("Unable to open input streams and/or files\n");
        return SDL_APP_FAILURE;
    }
#endif
    else
    {
        std::reverse(WaveB.Samples.begin(), WaveB.Samples.end());
        WaveB.NormalizeImpulseResponse();
    }

    std::set<std::string> RequestedLayers;
    {
#if VALIDATION_LAYERS
        RequestedLayers.emplace("VK_LAYER_KHRONOS_validation");
#endif
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
                    // Matches integrated GPUs.
                    Candidate.Score = 0;
                }
                else if (Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                {
                    // Matches LLVM PIPE and similar.
                    Candidate.Score = 1;
                }
                else
                {
                    // Matches discrete GPUs and more exotic things.
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
            else
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

        VkPhysicalDeviceSubgroupSizeControlFeatures SubgroupSizeControlFeatures =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
            .pNext = nullptr,
            .subgroupSizeControl = VK_TRUE,
            .computeFullSubgroups = VK_FALSE,
        };

        VkPhysicalDeviceVulkan12Features PhysicalDeviceVulkan12Features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &SubgroupSizeControlFeatures,
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
        VkShaderModule ShaderModule;
        {
            VkResult Result = CreateConvolverShader(Device, ShaderModule);
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
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo RequiredSubgroupSizeCreateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
                .pNext = nullptr,
                .requiredSubgroupSize = (uint32_t)Params.GroupSize,
            };

            VkComputePipelineCreateInfo CreateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = &RequiredSubgroupSizeCreateInfo,
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

    const int32_t SizeB = WaveB.Samples.size();
    const int32_t MinSizeC = Params.SamplesPerFrame;

    // History pages needs to be long enough to prevent overlap in the ring buffer between live convolution ranges.
    const int32_t HistoryPages = std::max(DIV_UP(SizeB * 2, MinSizeC), 5);
    const int32_t UploadPages = 1;
    const int32_t SizeA = Params.SamplesPerFrame * (UploadPages + HistoryPages);
#if LIVE_STREAM_MODE
    const int32_t SizeC = SizeA;
#else
    const int32_t SizeC = MinSizeC;
#endif

    SharedMemory<float>* BufferA = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, SizeA, 0.0f);
    SharedMemory<float>* BufferB = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, WaveB.Samples);
    SharedMemory<float>* BufferC = new SharedMemory<float>(Device, MemoryTypeIndex, QueueFamilyIndex, SizeC, 0.0f);

    if (!BufferA->IsValid)
    {
        std::print("Failed to allocate `BufferA`\n");
        TEARDOWN_FROM_DEVICE();
    }
    else if (!BufferB->IsValid)
    {
        std::print("Failed to allocate `BufferB`\n");
        TEARDOWN_FROM_DEVICE();
    }
    else if (!BufferC->IsValid)
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

#if LIVE_STREAM_MODE
    ThreadShared BufferState = ThreadShared(BufferA->Mapped, BufferA->ElementCount, BufferC->Mapped, BufferC->ElementCount);
    PipeWireFilter PipeWireSession(&BufferState, Params.SampleRate);
    PipeWireSession.Run();
#else
    SDL_ResumeAudioStreamDevice(InStream);
    SDL_SetAudioStreamGain(OutStream, 6.0);
#endif

    bool Shutdown = false;

    int32_t FrameNumber = 0;
    while (!Shutdown)
    {
        {
            SDL_Event Event;
            SDL_PollEvent(&Event);
            if (Event.type == SDL_EVENT_QUIT)
            {
                std::print("\nPlayer requested shutdown.\n\n");
                Shutdown = true;
            }
        }

#if LIVE_STREAM_MODE
        if (!PipeWireSession.Live.load())
        {
            Shutdown = true;
        }
        {
            const size_t InReady = BufferState.InReady.load();
            const size_t InProcessed = BufferState.InProcessed.load();
            const size_t InPending = InReady - InProcessed;
            if (InPending < Params.SamplesPerFrame)
            {
                continue;
            }
            else
            {
                BufferState.InProcessed += Params.SamplesPerFrame;
            }
        }
#else
        const int QueuedOutputBytes = SDL_GetAudioStreamQueued(OutStream);
        if (QueuedOutputBytes > Params.TargetBytesPerFrame * 4) // can go as low as * 2
        {
            continue;
        }
#endif

        const int32_t Start = FrameNumber * Params.SamplesPerFrame;
        const int32_t Stop = Start + Params.SamplesPerFrame;

        bool PartialFrame = Start < BufferB->ElementCount;

#if !LIVE_STREAM_MODE
        {
            if (InStream == nullptr)
            {
                std::print("\nWhat?!: {}\n", SDL_GetError());
                break;
            }

            const int32_t BytesReady = int32_t(std::min(SDL_GetAudioStreamAvailable(InStream), int(Params.BytesPerFrame)));
            if (BytesReady < 0)
            {
                std::print("\nError preparing to read from input stream: {}\n", SDL_GetError());
                break;
            }
            while (BytesReady < Params.BytesPerFrame);
            const int32_t SamplesReady = BytesReady / sizeof(float);

            // This is currently guaranteed: (Start % SamplesPerFrame) == 0
            const int WriteStart = (FrameNumber % HistoryPages) * Params.SamplesPerFrame;
            float* WriteHead = BufferA->Mapped + WriteStart;

            if (SamplesReady > 0)
            {
                const int BytesWritten = SDL_GetAudioStreamData(InStream, WriteHead, SamplesReady * sizeof(float));
                if (BytesWritten < 0)
                {
                    std::print("\nError reading input stream: {}\n", SDL_GetError());
                    break;
                }
            }

            const int32_t PaddingSamples = Params.SamplesPerFrame - SamplesReady;

            if (PaddingSamples > 0)
            {
                PartialFrame = true;
                const int32_t PaddingStart = WriteStart + SamplesReady;
                for (int p = 0; p < PaddingSamples; ++p)
                {
                    BufferA->Mapped[(PaddingStart + p) % SizeA] = 0.0f;
                }
            }
        }
#endif

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
                .Start = Start
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
            vkCmdDispatch(CommandBuffer, Params.GroupsPerFrame, 1, 1);
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
#if LIVE_STREAM_MODE
        BufferState.OutReady += Params.SamplesPerFrame;
#endif

#if BENCHMARKING
        if (!PartialFrame)
        {
            const auto FrameStopTime = std::chrono::steady_clock::now();
            const std::chrono::duration<double, std::milli> DeltaTime = FrameStopTime - FrameStartTime;
            TotalTimeMs += DeltaTime;
            ++RecordedSamples;

            if (DeltaTime.count() > Params.FrameSpan)
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

        if (Shutdown)
        {
            for (int i = 0; i < SizeC; ++i)
            {
                const float Alpha = float(SizeC - i - 1) / float(SizeC);
                BufferC->Mapped[i] *= Alpha;
            }
        }

#if !LIVE_STREAM_MODE
        SDL_PutAudioStreamData(OutStream, BufferC->Mapped, sizeof(float) * Params.SamplesPerFrame);
        SDL_ResumeAudioStreamDevice(OutStream);
#endif
        ++FrameNumber;
    }
#endif

#if LIVE_STREAM_MODE
    PipeWireSession.Reset();
#endif

#if BENCHMARKING
    {
        if (RecordedSamples == 0)
        {
            std::print("Not enough samples recorded for benchmarking.\n\n");
        }

        std::print("\t       Input ring: {:.2f} KiB\n", double(SizeA * sizeof(float)) / 1024.0);
        std::print("\t       Convolvand: {:.2f} KiB\n", double(SizeB * sizeof(float)) / 1024.0);
        std::print("\t      Output ring: {:.2f} KiB\n", double(SizeC * sizeof(float)) / 1024.0);
        std::print("\n");

        std::print("\tSamples per frame: {}\n", Params.SamplesPerFrame);
        std::print("\t Groups per frame: {}\n", Params.GroupsPerFrame);
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
            std::print("\t Frames processed: {}\n", FrameNumber);
            std::print("\t Samples recorded: {}\n", RecordedSamples);
            std::print("\t    Average frame: {:.3f} milliseconds\n", AverageTime);
            std::print("\t       Best frame: {:.3f} milliseconds\n", BestFrame.count());
            std::print("\t      Worst frame: {:.3f} milliseconds\n", WorstFrame.count());
            std::print("\t       Total time: {:.3f} {}\n\n", TotalTime, TotalTimeUnit);
        }

        std::print("\t    Audio latency: {:.3f} milliseconds minimum\n\n", Params.FrameSpan);

        if (RecordedSamples > 0)
        {
            if (BestFrame.count() > Params.FrameSpan)
            {
                std::print("The best frame time is higher than the frame's playback duration!!\n");
            }
            else if (AverageTime > Params.FrameSpan)
            {
                std::print("The average frame time is higher than the frame's playback duration!\n");
            }
            else if (WorstFrame.count() > Params.FrameSpan)
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

#if !LIVE_STREAM_MODE
    if (WaveA)
    {
        delete WaveA;
        WaveA = nullptr;
    }

    if (!Shutdown)
    {
        SDL_FlushAudioStream(OutStream);
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
    SDL_DestroyAudioStream(OutStream);
#endif

    vkDestroyPipelineLayout(Device, ConvolverPipelineLayout, nullptr);
    delete BufferA;
    delete BufferB;
    delete BufferC;

    vkDestroyFence(Device, FrameFence, nullptr);


    TEARDOWN_FROM_NOMINAL();

#if LIVE_STREAM_MODE
    PipeWireHalt();
#endif

    std::print("Done!\n");
    return 0;
}
