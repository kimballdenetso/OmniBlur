#ifndef OMNIBLUR_KERNEL
#define OMNIBLUR_KERNEL

#include "PrGPU/KernelSupport/KernelCore.h" //includes KernelWrapper.h
#include "PrGPU/KernelSupport/KernelMemory.h"

#if GF_DEVICE_TARGET_DEVICE
#if GF_DEVICE_TARGET_HLSL
    #define fmax max
    #define fmin min
#endif

GF_KERNEL_FUNCTION(BoxBlurKernel,
    ((GF_PTR_READ_ONLY(float4))(inSrc))
    ((GF_PTR(float4))(outDst)),
    ((int)(inSrcPitch))
    ((int)(inDstPitch))
    ((int)(in16f))
    ((unsigned int)(inWidth))
    ((unsigned int)(inHeight))
    ((int)(inRadius))
    ((int)(inHorizontal)),
    ((uint2)(inXY)(KERNEL_XY)))
{
    if (inXY.x < inWidth && inXY.y < inHeight)
    {
        float4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
        int count = 0;

        for (int d = -inRadius; d <= inRadius; d++)
        {
            int sx = inHorizontal ? (int)inXY.x + d : (int)inXY.x;
            int sy = inHorizontal ? (int)inXY.y     : (int)inXY.y + d;

            if (sx < 0 || sx >= (int)inWidth || sy < 0 || sy >= (int)inHeight) continue;

            float4 pixel = ReadFloat4(inSrc, sy * inSrcPitch + sx, !!in16f);
            sum.x += pixel.x;
            sum.y += pixel.y;
            sum.z += pixel.z;
            sum.w += pixel.w;
            count++;
        }

        if (count > 0)
        {
            sum.x /= count;
            sum.y /= count;
            sum.z /= count;
            sum.w /= count;
        }

        WriteFloat4(sum, outDst, inXY.y * inDstPitch + inXY.x, !!in16f);
    }
}
#endif
#endif
