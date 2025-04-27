#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_KHR_shader_subgroup_basic: require
#extension GL_KHR_shader_subgroup_arithmetic: require

#define DIV_UP(X, Y) ((X + Y - 1) / Y)


layout(buffer_reference, std430, buffer_reference_align = 4) buffer SomeBufferRef
{
    float Data[];
};


layout(std430, push_constant) uniform PushConstantsBlock
{
    SomeBufferRef BufferA;  // + 8 = 8
    SomeBufferRef BufferB;  // + 8 = 16
    SomeBufferRef BufferC;  // + 8 = 24
    int SizeA;              // + 4 = 28
    int SizeB;              // + 4 = 32
    int SizeC;              // + 4 = 36
    int Start;              // + 4 = 40 bytes
};


layout (local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
void main()
{
    // Application must guarantee the following:
    //  - SizeA is always greater than SizeB, as BufferA must be padded with SizeB zeros.
    //  - SizeA is always greater than or equal to SizeC
    //  - LocalIndex <= SizeC
    const int LaneIndex = int(gl_LocalInvocationID.x);
    const int GroupIndex = int(gl_WorkGroupID.x);
    const int Sample = Start + GroupIndex;

    const int Slice = DIV_UP(SizeB, 32);
    const int SliceStart = LaneIndex * Slice;
    const int SliceStop = min(SliceStart + Slice, SizeB);

    float Acc = 0.0f;

    if (SliceStart < SizeB)
    {
        const int StartA = Sample - (SizeB - 1);
        for (int i = SliceStart; i < SliceStop; ++i)
        {
            const float SampleA = BufferA.Data[(StartA + i) % SizeA];
            const float SampleB = BufferB.Data[i];
            Acc += SampleA * SampleB;
        }
    }

    if (subgroupElect())
    {
        BufferC.Data[GroupIndex] = subgroupAdd(Acc);
    }
}
