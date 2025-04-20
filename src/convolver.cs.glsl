#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require


#define ABS_MODE 0
#define DYNAMIC_GAIN 0


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
    int Range;              // + 4 = 44
    float Gain;             // + 4 = 48 bytes
};


layout (local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
void main()
{
    const int Stop = min(SizeC, Start + Range);
    const int Sample = Start + int(gl_GlobalInvocationID.x);
    if (Sample < Stop)
    {
        float Acc = 0.0f;
        const int Iterations = min(min(SizeA, SizeB), Sample + 1);
        const int StartA = max(0, Sample + 1 - Iterations);
        const int StartB = SizeB - Iterations;
        for (int i = 0; i < Iterations; ++i)
        {
#if ABS_MODE
            Acc += BufferA.Data[StartA + i] * abs(BufferB.Data[StartB + i]);
#else
            Acc += BufferA.Data[StartA + i] * BufferB.Data[StartB + i];
#endif
        }
        BufferC.Data[Sample] = Acc * Gain; // wave sum on Acc if we parallelize the inner loop
    }
}
