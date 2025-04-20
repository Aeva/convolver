#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require


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
    int Start;              // + 4 = 40
    int Range;              // + 4 = 44 bytes
};


layout (local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
void main()
{
    // Application must guarantee the following:
    //  - SizeA is always greater than SizeB, as BufferA must be padded with SizeB zeros.
    //  - SizeA is always greater than or equal to SizeC
    //  - Stop <= SizeA
    //  - LocalIndex <= SizeC
    const int Stop = Start + Range;
    const int LocalIndex = int(gl_GlobalInvocationID.x);
    const int Sample = Start + LocalIndex;
    if (Sample < Stop)
    {
        float Acc = 0.0f;
        const int Iterations = min(SizeB, Sample + 1);
        const int StartA = Sample + 1 - Iterations; // Possible range is 0 to SizeA - Size B, inclusive.
        const int StartB = SizeB - Iterations; // Possible range is 0 to SizeB - 1, inclusive.
        for (int i = 0; i < Iterations; ++i)
        {
            const float SampleA = BufferA.Data[StartA + i];
            const float SampleB = BufferB.Data[StartB + i];
            Acc += SampleA * SampleB;
        }

        BufferC.Data[LocalIndex] = Acc;
    }
}
