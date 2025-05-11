
#include <vector>
#include <print>

#include "assorted.h"
#include "shader.h"


static const char ConvolverShaderSource[] = {
    #embed "scratch/convolver.cs.spirv"
};


static void PrintShader()
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


VkResult CreateConvolverShader(VkDevice Device, VkShaderModule& ShaderModule)
{
    PrintShader();

    static_assert(sizeof(ConvolverShaderSource) % sizeof(uint32_t) == 0);
    VkShaderModuleCreateInfo CreateInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = sizeof(ConvolverShaderSource),
        .pCode = (const uint32_t*)ConvolverShaderSource
    };

    return vkCreateShaderModule(Device, &CreateInfo, nullptr, &ShaderModule);
}
