#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require


#define ABS_MODE 0
#define DYNAMIC_GAIN 0


#if ABS_MODE
    #define IR_MUTATOR(Sample) abs(Sample)
#else
    #define IR_MUTATOR(Sample) (Sample)
#endif


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
    const int Stop = Start + Range;
    const int LocalIndex = int(gl_GlobalInvocationID.x);
    const int Sample = Start + LocalIndex;
    if (Sample < Stop || LocalIndex >= SizeC)
    {
        float Acc = 0.0f;
        const int Iterations = min(min(SizeA, SizeB), Sample + 1);
        const int StartA = max(0, Sample + 1 - Iterations);
        const int StartB = max(0, SizeB - Iterations);
        for (int i = 0; i < Iterations; ++i)
        {
            // The modulos here are to prevent overflow.  Wrap around is not expected.
            const float SampleA = BufferA.Data[(StartA + i) % SizeA];
            const float SampleB = BufferB.Data[(StartB + i) % SizeB];
            Acc += SampleA * IR_MUTATOR(SampleB);
        }

        BufferC.Data[LocalIndex] = Acc * Gain;
    }
}
