#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require


layout(buffer_reference, std430, buffer_reference_align = 4) buffer SomeBufferRef {
    uint Data[];
};


struct AssortedConstants {
    SomeBufferRef SomeBuffer;
};


layout(std430, push_constant) uniform PushConstantsBlock {
    AssortedConstants PushConstants;
};


layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main()
{
    uint A = PushConstants.SomeBuffer.Data[0];
    uint B = PushConstants.SomeBuffer.Data[1];
    uint C = A + B;
    PushConstants.SomeBuffer.Data[0] = B;
    PushConstants.SomeBuffer.Data[1] = C;
}
