
#include <vulkan/vulkan.h>
#include <print>
#include <format>
#include <vector>
#include <set>
#include <string>
#include <ranges>


const char Shader[] = {
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
    const int LastIndex = sizeof(Shader) - 1;
    for (const char Symbol : Shader)
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
            std::print("  ");
            std::print("{}  ", Line.size());
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
    std::print("It does nothing :3\n");
}

int main()
{
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
            return 1;
        }
    }

    VkPhysicalDevice PhysicalDevice;
    {
        uint32_t PhysicalDeviceCount = 0;
        std::vector<VkPhysicalDevice> AvailableDevices;
        VkResult Result = vkEnumeratePhysicalDevices(Instance, &PhysicalDeviceCount, nullptr);
        AvailableDevices.resize(PhysicalDeviceCount);
        Result = vkEnumeratePhysicalDevices(Instance, &PhysicalDeviceCount, AvailableDevices.data());

        std::print("Available Physical Devices:\n");

        for (VkPhysicalDevice AvailableDevice : AvailableDevices)
        {
            VkPhysicalDeviceProperties Properties;
            vkGetPhysicalDeviceProperties(AvailableDevice, &Properties);

            bool HasAnyCompute = false;
            std::vector<VkQueueFamilyProperties> QueueFamilyProperties;
            {
                uint32_t QueueFamilyPropertyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(AvailableDevice, &QueueFamilyPropertyCount, nullptr);
                QueueFamilyProperties.resize(QueueFamilyPropertyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(AvailableDevice, &QueueFamilyPropertyCount, QueueFamilyProperties.data());

                for (VkQueueFamilyProperties FamilyProperties : QueueFamilyProperties)
                {
                    if ((FamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
                    {
                        HasAnyCompute = true;
                        break;
                    }
                }
            }

            std::print(" - {}\n", Properties.deviceName);

            bool HasDeviceLocalHostVisible = false;
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
                    HasDeviceLocalHostVisible = true;
                }

                std::print("   {}:", MemoryType.heapIndex);
#define PRINT_FLAG(FLAG, SIGIL) std::print(" {}", HAS_FLAG(MemoryType.propertyFlags, FLAG) ? SIGIL : "--")
                PRINT_FLAG(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DL");
                PRINT_FLAG(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HV");
                PRINT_FLAG(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HC");
                PRINT_FLAG(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HA");
                PRINT_FLAG(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LA");
                PRINT_FLAG(VK_MEMORY_PROPERTY_PROTECTED_BIT, ":P");
                PRINT_FLAG(VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, "DC");
                PRINT_FLAG(VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD, "DU");
                PRINT_FLAG(VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV, "RC");
#undef PRINT_FLAG
                std::print("\n");
            }
            std::print("\n");
        }
    }




    PrintShader();


    vkDestroyInstance(Instance, nullptr);
    std::print("Done!\n");
    return 0;
}
