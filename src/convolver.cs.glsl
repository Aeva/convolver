#version 450
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_KHR_shader_subgroup_arithmetic: require

#define DIV_UP(X, Y) ((X + Y - 1) / Y)


layout(buffer_reference, std430, buffer_reference_align = 4) buffer SomeBufferRef
{
    int16_t Data[];
};


layout(std430, push_constant) uniform PushConstantsBlock
{
    SomeBufferRef BufferA; // + 8 = 8
    SomeBufferRef BufferB; // + 8 = 16
    SomeBufferRef BufferC; // + 8 = 24
    int SizeA;              // + 4 = 28
    int SizeB;              // + 4 = 32
    int SizeC;              // + 4 = 36
    int Start;              // + 4 = 40
    float GainB;            // + 4 = 44 bytes
};


layout (local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;
void main()
{
    // Application must guarantee the following:
    //  - SizeA is always greater than SizeB, as BufferA must be padded with SizeB zeros.
    //  - SizeA is always greater than or equal to SizeC
    //  - LocalIndex <= SizeC

    const int LaneIndex = int(gl_SubgroupInvocationID);
    const int GroupIndex = int(gl_WorkGroupID.x);
    const int Sample = Start + GroupIndex;

    float LaneAcc = 0.0f;

    const float FloatToShort = float(0x7fff);
    const float ShortToFloat = 1.0 / FloatToShort;

    const float ScaleA = ShortToFloat;
    const float ScaleB = ShortToFloat * GainB;
    const float ScaleC = FloatToShort;

    const int StartA = Sample - (SizeB - 1);
    for (int i = LaneIndex; i < SizeB; i += GROUP_SIZE)
    {
        const float SampleA = float(int(BufferA.Data[(StartA + i) % SizeA]));
        const float SampleB = float(int(BufferB.Data[i]));
        LaneAcc += (SampleA * ScaleA) * (SampleB * ScaleB);
    }

    const float Total = subgroupAdd(LaneAcc);

    if (subgroupElect())
    {
        BufferC.Data[Sample % SizeC] = int16_t(int(Total * ScaleC));
    }
}
