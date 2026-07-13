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
#define DESCRIPTION        "v1.0.1 Separate blur"
#define MAJOR_VERSION    1
#define MINOR_VERSION    0
#define BUG_VERSION        0
#define STAGE_VERSION    PF_Stage_DEVELOP
#define BUILD_VERSION    2 // remind user to increment when starting work again

enum {
    OMNIBLUR_INPUT = 0,
    OMNIBLUR_RADIUS,
    OMNIBLUR_NUM_PARAMS
};

#define RADIUS_MIN        0
#define RADIUS_MAX        50
#define RADIUS_DFLT        5

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

static PF_Err
Render(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[], PF_LayerDef *output)
{
    PF_Err err = PF_Err_NONE;

    A_long radius = params[OMNIBLUR_RADIUS]->u.sd.value;
    PF_EffectWorld *input = &params[OMNIBLUR_INPUT]->u.ld;

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
        }
    } catch (PF_Err &thrown_err) {
        err = thrown_err;
    }

    return err;
}
