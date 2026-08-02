#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "entry.h"   // Required for PF_PluginDataPtr / PF_PluginDataCB2 (used by
                     // PluginDataEntryFunction2 below). Lives at Examples/Util/entry.h
                     // in the SDK, NOT Examples/Headers/ -- had to add that folder to
                     // Xcode's Header Search Paths. Note: "PiPL.h" does NOT exist in
                     // this SDK version -- don't reach for it if this include ever
                     // needs revisiting.

#define NAME            "OmniBlur"
#define DESCRIPTION        "v1.0.3 GPU Dispatch"
#define MAJOR_VERSION    1
#define MINOR_VERSION    0
#define BUG_VERSION        3
#define STAGE_VERSION    PF_Stage_DEVELOP
#define BUILD_VERSION    15 // remind user to increment when starting work again

enum {
    OMNIBLUR_INPUT = 0,
    OMNIBLUR_RADIUS,
    OMNIBLUR_NUM_PARAMS
};

#define RADIUS_MIN        0
#define RADIUS_MAX        50
#define RADIUS_DFLT        5

// --- Metal GPU support -----------------------------------------------------
// Everything below marked VERIFY is scaffolding based on Adobe's SDK_Invert_ProcAmp
// sample pattern, not confirmed against this project's actual SDK headers yet.
// Same approach as PF_WorldSuite1 earlier: type `->` in Xcode and let autocomplete
// confirm the real field/method names before trusting this.

#include <Metal/Metal.h>
#include <os/log.h>
#include "OmniBlur_Kernel.metal.h"
#include "AE_EffectGPUSuites.h"
// VERIFY: the kernel below is written in Adobe's cross-platform GF_KERNEL_FUNCTION
// DSL. In the sample project this macro-based kernel source lives in its own file
// (translated at build time into CUDA/OpenCL/Metal via the Boost-based build step),
// NOT inline in the main .cpp. Where exactly it needs to live in OmniBlur's build
// setup still needs to be confirmed by looking at how SDK_Invert_ProcAmp's project
// file wires up its own kernel file -- left here for now so the logic is visible.
//
// GF_KERNEL_FUNCTION(BoxBlurKernel,
//     ((const GF_PTR(float4))(inSrc))
//     ((GF_PTR(float4))(outDst)),
//     ((int)(inSrcPitch))
//     ((int)(inDstPitch))
//     ((int)(inWidth))
//     ((int)(inHeight))
//     ((int)(inRadius))
//     ((int)(inHorizontal)),
//     ((uint2)(inXY)(KERNEL_XY)))
// {
//     if (inXY.x < inWidth && inXY.y < inHeight)
//     {
//         float4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
//         int count = 0;
//         for (int d = -inRadius; d <= inRadius; d++)
//         {
//             int sx = inHorizontal ? (int)inXY.x + d : (int)inXY.x;
//             int sy = inHorizontal ? (int)inXY.y     : (int)inXY.y + d;
//             if (sx < 0 || sx >= inWidth || sy < 0 || sy >= inHeight) continue;
//             float4 pixel = ReadFloat4(inSrc, sy * inSrcPitch + sx, 0);
//             sum.x += pixel.x; sum.y += pixel.y; sum.z += pixel.z; sum.w += pixel.w;
//             count++;
//         }
//         if (count > 0) { sum.x /= count; sum.y /= count; sum.z /= count; sum.w /= count; }
//         WriteFloat4(sum, outDst, inXY.y * inDstPitch + inXY.x, 0);
//     }
// }

// Mirrors the real generated Metal struct BoxBlurKernelValues -- CONFIRMED by reading
// the actual dumped kernel source (not guessed). Field order/types must match exactly
// since this gets bound as raw bytes at buffer index 2. Note in16f exists in the real
// kernel even though the old placeholder sketch above didn't have it -- the real
// OmniBlur_Kernel.cu has since diverged from that sketch.
struct BoxBlurKernelValues
{
    int inSrcPitch;
    int inDstPitch;
    int in16f;
    unsigned int inWidth;
    unsigned int inHeight;
    int inRadius;
    int inHorizontal;
};

// GPU data initialized in GPUDeviceSetup, used during SmartRenderGPU, released in
// GPUDeviceSetdown. VERIFY: mirrors the sample's MetalGPUData struct pattern.
struct OmniBlurMetalGPUData
{
    id<MTLComputePipelineState> blur_pipeline;
    id<MTLCommandQueue> command_queue; // wasn't being stashed anywhere before -- needed once SmartRenderGPU dispatches for real
};

static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    PF_SPRINTF(out_data->return_msg,
        "%s v%d.%d.%d build %d\r%s",
        NAME, MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, BUILD_VERSION, DESCRIPTION);
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;
    // SUPPORTS_SMART_RENDER is the prerequisite flag for the whole Smart FX pipeline --
    // GPU rendering (Metal on Mac) is only reachable through Smart Render, not the
    // classic PF_Cmd_RENDER path. Once this is set, AE sends PF_Cmd_SMART_PRE_RENDER +
    // PF_Cmd_SMART_RENDER instead (on hosts that support it). Keeping PF_Cmd_RENDER
    // implemented too, below, as a fallback for older/non-smart hosts.
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER
                          | PF_OutFlag2_SUPPORTS_GPU_RENDER_F32; // required to be offered GPU rendering at all
    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Radius", RADIUS_MIN, RADIUS_MAX, RADIUS_MIN, RADIUS_MAX, RADIUS_DFLT, OMNIBLUR_RADIUS);

    out_data->num_params = OMNIBLUR_NUM_PARAMS;
    return PF_Err_NONE;
}

// Does the actual two-pass separable box blur, input -> output. Pulled out of Render()
// as its own function so both the classic PF_Cmd_RENDER path and the new Smart Render
// path (below) can share it -- neither has to change when the blur algorithm changes.
static PF_Err
DoBlur(PF_InData *in_data, A_long radius, PF_EffectWorld *input, PF_EffectWorld *output)
{
    PF_Err err = PF_Err_NONE;

    A_long width = output->width;
    A_long height = output->height;

    // Fast path: no blur requested, just copy input -> output and bail.
    if (radius <= 0) {
        for (A_long y = 0; y < height; y++) {
            PF_Pixel8 *inRow = reinterpret_cast<PF_Pixel8*>(
                reinterpret_cast<char*>(input->data) + y * input->rowbytes);
            PF_Pixel8 *outRow = reinterpret_cast<PF_Pixel8*>(
                reinterpret_cast<char*>(output->data) + y * output->rowbytes);
            for (A_long x = 0; x < width; x++) {
                outRow[x] = inRow[x];
            }
        }
        return err;
    }

    // Scratch buffer to hold the result of the horizontal pass.
    // We can't blur input -> output in a single pass and then blur output -> output;
    // we'd be reading pixels we already overwrote. Need a separate intermediate world.
    PF_EffectWorld temp;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_WorldSuite1 *wsP = suites.WorldSuite1();

    ERR(wsP->new_world(
        in_data->effect_ref,           // PF_ProgPtr ✓
        width,                         // A_long ✓
        height,                        // A_long ✓
        PF_NewWorldFlag_CLEAR_PIXELS,  // PF_NewWorldFlags ✓
        &temp));                       // PF_EffectWorld* ✓

    if (!err) {
        // ---- PASS 1: horizontal blur, input -> temp ----
        for (A_long y = 0; y < height; y++) {
            PF_Pixel8 *inRow = reinterpret_cast<PF_Pixel8*>(
                reinterpret_cast<char*>(input->data) + y * input->rowbytes);
            PF_Pixel8 *tempRow = reinterpret_cast<PF_Pixel8*>(
                reinterpret_cast<char*>(temp.data) + y * temp.rowbytes);

            for (A_long x = 0; x < width; x++) {
                long rSum = 0, gSum = 0, bSum = 0, aSum = 0, count = 0;

                for (A_long dx = -radius; dx <= radius; dx++) {
                    A_long sx = x + dx;
                    if (sx < 0 || sx >= input->width) continue;

                    PF_Pixel8 *pixP = inRow + sx;
                    aSum += pixP->alpha;
                    rSum += pixP->red;
                    gSum += pixP->green;
                    bSum += pixP->blue;
                    count++;
                }

                if (count > 0) {
                    tempRow[x].alpha = static_cast<A_u_char>(aSum / count);
                    tempRow[x].red   = static_cast<A_u_char>(rSum / count);
                    tempRow[x].green = static_cast<A_u_char>(gSum / count);
                    tempRow[x].blue  = static_cast<A_u_char>(bSum / count);
                }
            }
        }

        // ---- PASS 2: vertical blur, temp -> output ----
        for (A_long y = 0; y < height; y++) {
            PF_Pixel8 *outRow = reinterpret_cast<PF_Pixel8*>(
                reinterpret_cast<char*>(output->data) + y * output->rowbytes);

            for (A_long x = 0; x < width; x++) {
                long rSum = 0, gSum = 0, bSum = 0, aSum = 0, count = 0;

                for (A_long dy = -radius; dy <= radius; dy++) {
                    A_long sy = y + dy;
                    if (sy < 0 || sy >= temp.height) continue;

                    PF_Pixel8 *tempRow = reinterpret_cast<PF_Pixel8*>(
                        reinterpret_cast<char*>(temp.data) + sy * temp.rowbytes);
                    PF_Pixel8 *pixP = tempRow + x;

                    aSum += pixP->alpha;
                    rSum += pixP->red;
                    gSum += pixP->green;
                    bSum += pixP->blue;
                    count++;
                }

                if (count > 0) {
                    outRow[x].alpha = static_cast<A_u_char>(aSum / count);
                    outRow[x].red   = static_cast<A_u_char>(rSum / count);
                    outRow[x].green = static_cast<A_u_char>(gSum / count);
                    outRow[x].blue  = static_cast<A_u_char>(bSum / count);
                }
            }
        }

        // Must give the temp world back to AE, or it leaks every render.
        ERR(wsP->dispose_world(in_data->effect_ref, &temp));
    }

    return err;
}

// Fallback entry point for hosts that don't support Smart FX. Modern AE will use
// SmartPreRender/SmartRender below instead once SUPPORTS_SMART_RENDER is set.
static PF_Err
Render(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    A_long radius = params[OMNIBLUR_RADIUS]->u.sd.value;
    PF_EffectWorld *input = &params[OMNIBLUR_INPUT]->u.ld;
    return DoBlur(in_data, radius, input, output);
}

// PF_Cmd_SMART_PRE_RENDER: tell AE what part of the input we need, and how big the
// output will be. Because a blur reads neighboring pixels, we have to grow the
// requested input rect by `radius` in every direction, or edge pixels of the visible
// frame will blur using out-of-bounds/garbage data.
static PF_Err
PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    PF_ParamDef radius_param;
    AEFX_CLR_STRUCT(radius_param);
    ERR(PF_CHECKOUT_PARAM(in_data, OMNIBLUR_RADIUS, in_data->current_time,
                           in_data->time_step, in_data->time_scale, &radius_param));
    A_long radius = err ? 0 : radius_param.u.sd.value;

    req.rect.left   -= radius;
    req.rect.top    -= radius;
    req.rect.right  += radius;
    req.rect.bottom += radius;
    req.preserve_rgb_of_zero_alpha = TRUE;

    ERR(extra->cb->checkout_layer(in_data->effect_ref,
                                   OMNIBLUR_INPUT,
                                   OMNIBLUR_INPUT,
                                   &req,
                                   in_data->current_time,
                                   in_data->time_step,
                                   in_data->time_scale,
                                   &in_result));

    if (!err) {
        extra->output->result_rect     = in_result.result_rect;
        extra->output->max_result_rect = in_result.max_result_rect;
        extra->output->solid           = FALSE;
        extra->output->pre_render_data = NULL;
    }

    return err;
}

// PF_Cmd_SMART_RENDER: the Smart FX counterpart of the old Render(). Checks out the
// input/output worlds it declared in PreRender and runs the same DoBlur() as the
// classic path. This is also the function that will branch to a GPU/Metal dispatch
// once that's wired up -- for now it's CPU-only, same math as before, just reached
// through the Smart FX door instead of the classic one.
static PF_Err
SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;
    PF_EffectWorld *input_worldP = NULL;
    PF_EffectWorld *output_worldP = NULL;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, OMNIBLUR_INPUT, &input_worldP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_worldP));

    if (!err && input_worldP && output_worldP) {
        PF_ParamDef radius_param;
        AEFX_CLR_STRUCT(radius_param);
        ERR(PF_CHECKOUT_PARAM(in_data, OMNIBLUR_RADIUS, in_data->current_time,
                               in_data->time_step, in_data->time_scale, &radius_param));
        A_long radius = err ? 0 : radius_param.u.sd.value;

        ERR(DoBlur(in_data, radius, input_worldP, output_worldP));
    }

    ERR(extra->cb->checkin_layer_pixels(in_data->effect_ref, OMNIBLUR_INPUT));

    return err;
}

// PF_Cmd_GPU_DEVICE_SETUP: sets up the Metal compute pipeline once per device, ahead
// of any render. VERIFY every field/method below against Xcode autocomplete on
// PF_GPUDeviceSetupExtra and PF_GPUDeviceInfo -- names below follow the sample
// project's pattern but are not confirmed for this SDK version.
static PF_Err
GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetupExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    // VERIFY: field name for which GPU framework AE handed us (CUDA/OpenCL/Metal).
    if (extra->input->what_gpu != PF_GPU_Framework_METAL) {
        // Not Metal (e.g. host chose CUDA/OpenCL on another platform) -- decline GPU
        // render for this device so AE falls back to the CPU SmartRender path.
        out_data->out_flags2 &= ~PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
        return err;
    }

    // CONFIRMED via Xcode autocomplete: PF_GPUDeviceSetupInput only has device_index
    // and what_gpu -- no device pointer field. The actual MTLDevice has to come from
    // a suite call keyed on device_index, same pattern as PF_WorldSuite1 earlier.
    A_u_long device_index = extra->input->device_index;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_GPUDeviceSuite1 *gpu_suiteP = suites.GPUDeviceSuite1(); // VERIFY exact accessor name via Xcode autocomplete, same as WorldSuite1 earlier

    PF_GPUDeviceInfo device_info;
    AEFX_CLR_STRUCT(device_info); // wasn't zeroed before -- matches the pattern used everywhere else in this file
    ERR(gpu_suiteP->GetDeviceInfo(in_data->effect_ref, device_index, &device_info));

    // Everything below touches real Metal objects, and Metal's validation layer
    // hard-crashes the process (not a catchable error) if you hand it a nil
    // MTLFunction/MTLDevice. This used to run unconditionally even when GetDeviceInfo
    // failed -- that's almost certainly what was crashing AE. Guard the whole block.
    if (!err) {
        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)device_info.devicePV;

            if (!device) {
                err = PF_Err_INTERNAL_STRUCT_DAMAGED;
            } else {
                NSString *source = [NSString stringWithUTF8String: kOmniBlur_Kernel_MetalString];

                NSError *nsErr = nil;
                id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&nsErr];
                if (!library) {
                    // Shader failed to compile. Console.app redacts NSError text in %@ by
                    // default (shows "<private>") -- use os_log with %{public}@ to force it
                    // to actually show. Also dump the source we tried to compile straight to
                    // disk so it can be inspected (or fed to `xcrun metal` directly) without
                    // fighting log redaction at all.
                    os_log_error(OS_LOG_DEFAULT, "OmniBlur: Metal shader compile failed: %{public}@", nsErr);
                    NSError *writeErr = nil;
                    NSString *dumpPath = @"/tmp/OmniBlur_generated_kernel.metal";
                    [source writeToFile:dumpPath atomically:YES encoding:NSUTF8StringEncoding error:&writeErr];
                    os_log_error(OS_LOG_DEFAULT, "OmniBlur: wrote generated shader source to %{public}@", dumpPath);
                    err = PF_Err_INTERNAL_STRUCT_DAMAGED;
                } else {
                    id<MTLFunction> blurFn = [library newFunctionWithName:@"BoxBlurKernel"];
                    if (!blurFn) {
                        os_log_error(OS_LOG_DEFAULT, "OmniBlur: BoxBlurKernel function not found in compiled library");
                        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
                    } else {
                        OmniBlurMetalGPUData *metal_data = new OmniBlurMetalGPUData();
                        metal_data->blur_pipeline = [device newComputePipelineStateWithFunction:blurFn error:&nsErr];

                        if (!metal_data->blur_pipeline) {
                            os_log_error(OS_LOG_DEFAULT, "OmniBlur: pipeline state creation failed: %{public}@", nsErr);
                            delete metal_data;
                            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
                        } else {
                            metal_data->command_queue = (__bridge id<MTLCommandQueue>)device_info.command_queuePV;
                            extra->output->gpu_data = (void*)metal_data;
                        }
                    }
                }
            }
        }
    }

    return err;
}

// PF_Cmd_GPU_DEVICE_SETDOWN: release what GPUDeviceSetup allocated.
static PF_Err
GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetdownExtra *extra)
{
    // VERIFY: field name to retrieve back the gpu_data stashed in setup.
    OmniBlurMetalGPUData *metal_data = (OmniBlurMetalGPUData*)extra->input->gpu_data;
    delete metal_data;
    return PF_Err_NONE;
}

// PF_Cmd_SMART_RENDER_GPU: GPU counterpart of SmartRender. Checks out GPU-resident
// worlds (source, an intermediate buffer for the pass-1 -> pass-2 handoff, and
// destination) and dispatches BoxBlurKernel twice -- horizontal then vertical --
// mirroring DoBlur()'s two CPU passes exactly, just running on the GPU.
// VERIFY: every checkout/world-access call below against the sample's
// SmartRenderGPU -- this is the least-confident section of the whole file.
static PF_Err
SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    // VERIFY: retrieving the OmniBlurMetalGPUData stashed during GPUDeviceSetup.
    OmniBlurMetalGPUData *metal_data = (OmniBlurMetalGPUData*)extra->input->gpu_data;

    PF_EffectWorld *input_worldP = NULL;
    PF_EffectWorld *output_worldP = NULL;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, OMNIBLUR_INPUT, &input_worldP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_worldP));

    // Intermediate GPU-resident world for the horizontal-pass output, same role as
    // `temp` in DoBlur(). DoBlur's scratch buffer comes from PF_WorldSuite1::new_world,
    // but that suite is CPU-only -- the GPU equivalent is PF_GPUDeviceSuite1's
    // CreateGPUWorld/DisposeGPUWorld pair, same suite we already added an accessor for
    // to fix GPUDeviceSetup earlier.
    // CONFIRMED via Xcode jump-to-definition: CreateGPUWorld/DisposeGPUWorld's real
    // signatures. DisposeGPUWorld only takes (effect_ref, worldP) -- the 2-arg call
    // below was already right. CreateGPUWorld needed 5 more args than the earlier
    // guess: pixel_aspect_ratio, field_type, and a clear_pixB flag, on top of
    // width/height/pixel_format.
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_GPUDeviceSuite1 *gpu_suiteP = suites.GPUDeviceSuite1();
    A_u_long device_index = extra->input->device_index; // VERIFY: field name/existence on PF_SmartRenderExtra's input -- not yet confirmed the same way CreateGPUWorld/DisposeGPUWorld now are

    PF_EffectWorld *intermediate_worldP = NULL;
    if (!err && input_worldP && output_worldP) {
        ERR(gpu_suiteP->CreateGPUWorld(
                in_data->effect_ref,
                device_index,
                input_worldP->width,
                input_worldP->height,
                in_data->pixel_aspect_ratio, // VERIFY: PF_InData does carry pixel_aspect_ratio, but confirm it's populated the same way on the SmartRenderGPU path as it is elsewhere
                PF_Field_FRAME, // scratch buffer, not a final delivered frame -- no interlacing to preserve here; VERIFY this constant name
                PF_PixelFormat_GPU_BGRA128, // CONFIRMED via Xcode jump-to-definition: GPU, BGRA, 32-bit float per channel -- matches the F32-only GPU support we declared (PF_OutFlag2_SUPPORTS_GPU_RENDER_F32)
                FALSE, // clear_pixB -- skip the clear; the horizontal pass is about to write every in-bounds pixel anyway, so pre-clearing is wasted work. Flip to TRUE if edge pixels ever look uninitialized.
                &intermediate_worldP));
    }

    if (!err && input_worldP && output_worldP && intermediate_worldP) {
        PF_ParamDef radius_param;
        AEFX_CLR_STRUCT(radius_param);
        ERR(PF_CHECKOUT_PARAM(in_data, OMNIBLUR_RADIUS, in_data->current_time,
                               in_data->time_step, in_data->time_scale, &radius_param));
        A_long radius = err ? 0 : radius_param.u.sd.value;

        // Real dispatch, replacing the earlier commented-out sketch. Structure:
        // pull each world's real GPU buffer via GetGPUWorldData, pack the scalar
        // params into a BoxBlurKernelValues struct per pass, dispatch horizontal
        // (input -> intermediate) then vertical (intermediate -> output).
        if (!err) {
            @autoreleasepool {
                void *inputPixP = NULL, *intermediatePixP = NULL, *outputPixP = NULL;
                ERR(gpu_suiteP->GetGPUWorldData(in_data->effect_ref, input_worldP, &inputPixP));
                ERR(gpu_suiteP->GetGPUWorldData(in_data->effect_ref, intermediate_worldP, &intermediatePixP));
                ERR(gpu_suiteP->GetGPUWorldData(in_data->effect_ref, output_worldP, &outputPixP));

                if (!err) {
                    // VERIFY: bridging a raw pixPP straight to id<MTLBuffer> -- not
                    // explicitly confirmed for this call, but follows the same
                    // __bridge pattern already confirmed correct twice for
                    // device_info.devicePV and command_queuePV in GPUDeviceSetup.
                    id<MTLBuffer> inputBuf = (__bridge id<MTLBuffer>)inputPixP;
                    id<MTLBuffer> intermediateBuf = (__bridge id<MTLBuffer>)intermediatePixP;
                    id<MTLBuffer> outputBuf = (__bridge id<MTLBuffer>)outputPixP;

                    // VERIFY: pitch units. PF_EffectWorld::rowbytes is normally in
                    // bytes; the kernel indexes by float4 (16-byte) elements, so this
                    // divides by 16 to get a per-row element stride. If the blurred
                    // result comes out sheared or offset per row, this conversion is
                    // the first thing to re-check.
                    int input_pitch = input_worldP->rowbytes / 16;
                    int intermediate_pitch = intermediate_worldP->rowbytes / 16;
                    int output_pitch = output_worldP->rowbytes / 16;

                    id<MTLCommandBuffer> commandBuffer = [metal_data->command_queue commandBuffer];
                    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
                    [encoder setComputePipelineState:metal_data->blur_pipeline];

                    MTLSize threadsPerGrid = MTLSizeMake((NSUInteger)input_worldP->width,
                                                          (NSUInteger)input_worldP->height, 1);
                    NSUInteger tw = metal_data->blur_pipeline.threadExecutionWidth;
                    NSUInteger th = metal_data->blur_pipeline.maxTotalThreadsPerThreadgroup / tw;
                    MTLSize threadsPerThreadgroup = MTLSizeMake(tw, th, 1);

                    // Pass 1: horizontal, input -> intermediate
                    BoxBlurKernelValues values = {};
                    values.inSrcPitch = input_pitch;
                    values.inDstPitch = intermediate_pitch;
                    values.in16f = 0; // VERIFY: assumed 0 means "not 16-bit half-float" -- we only declared F32 GPU support, so this should be the correct/only value we ever need
                    values.inWidth = (unsigned int)input_worldP->width;
                    values.inHeight = (unsigned int)input_worldP->height;
                    values.inRadius = (int)radius;
                    values.inHorizontal = 1;

                    [encoder setBuffer:inputBuf offset:0 atIndex:0];
                    [encoder setBuffer:intermediateBuf offset:0 atIndex:1];
                    [encoder setBytes:&values length:sizeof(values) atIndex:2];
                    [encoder dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];

                    // Two dispatches in the same encoder reading/writing the same
                    // buffers need an explicit barrier -- Metal does NOT auto-sync
                    // successive dispatchThreads calls within one encoder. Without
                    // this, pass 2 can start reading `intermediate` before pass 1
                    // finished writing it.
                    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

                    // Pass 2: vertical, intermediate -> output
                    values.inSrcPitch = intermediate_pitch;
                    values.inDstPitch = output_pitch;
                    values.inHorizontal = 0;

                    [encoder setBuffer:intermediateBuf offset:0 atIndex:0];
                    [encoder setBuffer:outputBuf offset:0 atIndex:1];
                    [encoder setBytes:&values length:sizeof(values) atIndex:2];
                    [encoder dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];

                    [encoder endEncoding];
                    [commandBuffer commit];
                }
            }
        }
        (void)metal_data;
    }

    // Dispose the intermediate world whenever it was created, regardless of what
    // failed afterward -- same reasoning as the !err guards in GPUDeviceSetup: don't
    // let a mid-render failure leak the GPU buffer. Written out manually (rather than
    // an ERR2-style macro) since ERR2 isn't confirmed to exist in this SDK's
    // Param_Utils.h. This preserves an earlier error code instead of overwriting it
    // with the disposal result. CONFIRMED via Xcode jump-to-definition: DisposeGPUWorld
    // really does only take (effect_ref, worldP) -- no fix needed here.
    if (intermediate_worldP) {
        PF_Err dispose_err = gpu_suiteP->DisposeGPUWorld(in_data->effect_ref, intermediate_worldP);
        if (!err) {
            err = dispose_err;
        }
    }

    ERR(extra->cb->checkin_layer_pixels(in_data->effect_ref, OMNIBLUR_INPUT));

    return err;
}

// ---------------------------------------------------------------------------
// THIS FUNCTION WAS MISSING AND WAS THE ROOT CAUSE of the plugin not appearing
// in AE's Effect Manager. EffectMain alone is not enough -- AE calls
// PluginDataEntryFunction2 at scan time to discover the plugin's Name, Match
// Name, Category, and render entry point. Without it, the bundle loads but AE
// has no registration callback to call, so it fails to enumerate the plugin
// silently (no crash, no error). The PiPL resource is a secondary/legacy
// registration path -- this function is what actually gets it listed.
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
// Used __attribute__((visibility("default"))) instead of the DllExport macro --
// DllExport wasn't resolving in this project's include setup (same fix already
// applied to EffectMain below).
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "OmniBlur",         // Name
        "ADBE OmniBlur",    // Match Name -- must match PiPL's Match Name exactly
        "Sample Plug-ins",  // Category
        0,                  // AE_RESERVED_INFO macro wasn't resolving/found in
                             // this SDK's include path -- literal 0 works fine here
        "EffectMain",       // Entry point function name, as a string
        "");                // Support URL -- empty string is fine

    return result;
}

extern "C" __attribute__((visibility("default")))
PF_Err EffectMain(
    PF_Cmd            cmd,
    PF_InData        *in_data,
    PF_OutData        *out_data,
    PF_ParamDef        *params[],
    PF_LayerDef        *output,
    void            *extra)
{
    PF_Err err = PF_Err_NONE;

    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_RENDER:
                err = Render(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data, reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data, reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
            case PF_Cmd_GPU_DEVICE_SETUP:
                err = GPUDeviceSetup(in_data, out_data, reinterpret_cast<PF_GPUDeviceSetupExtra*>(extra));
                break;
            case PF_Cmd_GPU_DEVICE_SETDOWN:
                err = GPUDeviceSetdown(in_data, out_data, reinterpret_cast<PF_GPUDeviceSetdownExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER_GPU:
                err = SmartRenderGPU(in_data, out_data, reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
        }
    } catch (PF_Err &thrown_err) {
        err = thrown_err;
    }

    return err;
}
