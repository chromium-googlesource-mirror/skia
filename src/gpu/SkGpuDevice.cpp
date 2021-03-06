
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */



#include "GrContext.h"
#include "GrTextContext.h"

#include "SkGpuDevice.h"
#include "SkGrTexturePixelRef.h"

#include "SkColorFilter.h"
#include "SkDrawProcs.h"
#include "SkGlyphCache.h"
#include "SkImageFilter.h"
#include "SkTLazy.h"
#include "SkUtils.h"

#define CACHE_LAYER_TEXTURES 1

#if 0
    extern bool (*gShouldDrawProc)();
    #define CHECK_SHOULD_DRAW(draw)                             \
        do {                                                    \
            if (gShouldDrawProc && !gShouldDrawProc()) return;  \
            this->prepareRenderTarget(draw);                    \
        } while (0)
#else
    #define CHECK_SHOULD_DRAW(draw) this->prepareRenderTarget(draw)
#endif

// we use the same texture slot on GrPaint for bitmaps and shaders
// (since drawBitmap, drawSprite, and drawDevice ignore skia's shader)
enum {
    kBitmapTextureIdx = 0,
    kShaderTextureIdx = 0
};


#define MAX_BLUR_SIGMA 4.0f
// FIXME:  This value comes from from SkBlurMaskFilter.cpp.
// Should probably be put in a common header someplace.
#define MAX_BLUR_RADIUS SkIntToScalar(128)
// This constant approximates the scaling done in the software path's
// "high quality" mode, in SkBlurMask::Blur() (1 / sqrt(3)).
// IMHO, it actually should be 1:  we blur "less" than we should do
// according to the CSS and canvas specs, simply because Safari does the same.
// Firefox used to do the same too, until 4.0 where they fixed it.  So at some
// point we should probably get rid of these scaling constants and rebaseline
// all the blur tests.
#define BLUR_SIGMA_SCALE 0.6f
///////////////////////////////////////////////////////////////////////////////

SkGpuDevice::SkAutoCachedTexture::
             SkAutoCachedTexture(SkGpuDevice* device,
                                 const SkBitmap& bitmap,
                                 const GrSamplerState* sampler,
                                 GrTexture** texture) {
    GrAssert(texture);
    *texture = this->set(device, bitmap, sampler);
}

SkGpuDevice::SkAutoCachedTexture::SkAutoCachedTexture() {
}

GrTexture* SkGpuDevice::SkAutoCachedTexture::set(SkGpuDevice* device,
                                                 const SkBitmap& bitmap,
                                                 const GrSamplerState* sampler) {
    if (fTex.texture()) {
        fDevice->unlockCachedTexture(fTex);
    }
    fDevice = device;
    GrTexture* texture = (GrTexture*)bitmap.getTexture();
    if (texture) {
        // return the native texture
        fTex.reset();
    } else {
        // look it up in our cache
        fTex = device->lockCachedTexture(bitmap, sampler);
        texture = fTex.texture();
    }
    return texture;
}

SkGpuDevice::SkAutoCachedTexture::~SkAutoCachedTexture() {
    if (fTex.texture()) {
        fDevice->unlockCachedTexture(fTex);
    }
}

///////////////////////////////////////////////////////////////////////////////

bool gDoTraceDraw;

struct GrSkDrawProcs : public SkDrawProcs {
public:
    GrContext* fContext;
    GrTextContext* fTextContext;
    GrFontScaler* fFontScaler;  // cached in the skia glyphcache
};

///////////////////////////////////////////////////////////////////////////////

static SkBitmap::Config grConfig2skConfig(GrPixelConfig config, bool* isOpaque) {
    switch (config) {
        case kAlpha_8_GrPixelConfig:
            *isOpaque = false;
            return SkBitmap::kA8_Config;
        case kRGB_565_GrPixelConfig:
            *isOpaque = true;
            return SkBitmap::kRGB_565_Config;
        case kRGBA_4444_GrPixelConfig:
            *isOpaque = false;
            return SkBitmap::kARGB_4444_Config;
        case kSkia8888_PM_GrPixelConfig:
            // we don't currently have a way of knowing whether
            // a 8888 is opaque based on the config.
            *isOpaque = false;
            return SkBitmap::kARGB_8888_Config;
        default:
            *isOpaque = false;
            return SkBitmap::kNo_Config;
    }
}

static SkBitmap make_bitmap(GrContext* context, GrRenderTarget* renderTarget) {
    GrPixelConfig config = renderTarget->config();

    bool isOpaque;
    SkBitmap bitmap;
    bitmap.setConfig(grConfig2skConfig(config, &isOpaque),
                     renderTarget->width(), renderTarget->height());
    bitmap.setIsOpaque(isOpaque);
    return bitmap;
}

SkGpuDevice::SkGpuDevice(GrContext* context, GrTexture* texture)
: SkDevice(make_bitmap(context, texture->asRenderTarget())) {
    this->initFromRenderTarget(context, texture->asRenderTarget());
}

SkGpuDevice::SkGpuDevice(GrContext* context, GrRenderTarget* renderTarget)
: SkDevice(make_bitmap(context, renderTarget)) {
    this->initFromRenderTarget(context, renderTarget);
}

void SkGpuDevice::initFromRenderTarget(GrContext* context, 
                                       GrRenderTarget* renderTarget) {
    fNeedPrepareRenderTarget = false;
    fDrawProcs = NULL;
    
    fContext = context;
    fContext->ref();
    
    fTexture = NULL;
    fRenderTarget = NULL;
    fNeedClear = false;
    
    GrAssert(NULL != renderTarget);
    fRenderTarget = renderTarget;
    fRenderTarget->ref();
    // if this RT is also a texture, hold a ref on it
    fTexture = fRenderTarget->asTexture();
    SkSafeRef(fTexture);

    SkGrRenderTargetPixelRef* pr = new SkGrRenderTargetPixelRef(fRenderTarget);
    this->setPixelRef(pr, 0)->unref();
}

SkGpuDevice::SkGpuDevice(GrContext* context, SkBitmap::Config config, int width,
                         int height, Usage usage)
: SkDevice(config, width, height, false /*isOpaque*/) {
    fNeedPrepareRenderTarget = false;
    fDrawProcs = NULL;

    fContext = context;
    fContext->ref();

    fTexture = NULL;
    fRenderTarget = NULL;
    fNeedClear = false;

    if (config != SkBitmap::kRGB_565_Config) {
        config = SkBitmap::kARGB_8888_Config;
    }
    SkBitmap bm;
    bm.setConfig(config, width, height);

#if CACHE_LAYER_TEXTURES
    TexType type = (kSaveLayer_Usage == usage) ? 
                            kSaveLayerDeviceRenderTarget_TexType :
                            kDeviceRenderTarget_TexType;
    fCache = this->lockCachedTexture(bm, NULL, type);
    fTexture = fCache.texture();
    if (fTexture) {
        SkASSERT(NULL != fTexture->asRenderTarget());
        // hold a ref directly on fTexture (even though fCache has one) to match
        // other constructor paths. Simplifies cleanup.
        fTexture->ref();
    }
#else
    const GrTextureDesc desc = {
        kRenderTarget_GrTextureFlagBit,
        kNone_GrAALevel,
        width,
        height,
        SkGr::Bitmap2PixelConfig(bm)
    };

    fTexture = fContext->createUncachedTexture(desc, NULL, 0);
#endif
    if (NULL != fTexture) {
        fRenderTarget = fTexture->asRenderTarget();
        fRenderTarget->ref();

        GrAssert(NULL != fRenderTarget);

        // we defer the actual clear until our gainFocus()
        fNeedClear = true;

        // wrap the bitmap with a pixelref to expose our texture
        SkGrTexturePixelRef* pr = new SkGrTexturePixelRef(fTexture);
        this->setPixelRef(pr, 0)->unref();
    } else {
        GrPrintf("--- failed to create gpu-offscreen [%d %d]\n",
                 width, height);
        GrAssert(false);
    }
}

SkGpuDevice::~SkGpuDevice() {
    if (fDrawProcs) {
        delete fDrawProcs;
    }

    SkSafeUnref(fTexture);
    SkSafeUnref(fRenderTarget);
    if (fCache.texture()) {
        GrAssert(NULL != fTexture);
        GrAssert(fRenderTarget == fTexture->asRenderTarget());
        fContext->unlockTexture(fCache);
    } 
    fContext->unref();
}

///////////////////////////////////////////////////////////////////////////////

void SkGpuDevice::makeRenderTargetCurrent() {
    fContext->setRenderTarget(fRenderTarget);
    fContext->flush(true);
    fNeedPrepareRenderTarget = true;
}

///////////////////////////////////////////////////////////////////////////////

namespace {
GrPixelConfig config8888_to_gr_config(SkCanvas::Config8888 config8888) {
    switch (config8888) {
        case SkCanvas::kNative_Premul_Config8888:
            return kSkia8888_PM_GrPixelConfig;
        case SkCanvas::kNative_Unpremul_Config8888:
            return kSkia8888_UPM_GrPixelConfig;
        case SkCanvas::kBGRA_Premul_Config8888:
            return kBGRA_8888_PM_GrPixelConfig;
        case SkCanvas::kBGRA_Unpremul_Config8888:
            return kBGRA_8888_UPM_GrPixelConfig;
        case SkCanvas::kRGBA_Premul_Config8888:
            return kRGBA_8888_PM_GrPixelConfig;
        case SkCanvas::kRGBA_Unpremul_Config8888:
            return kRGBA_8888_UPM_GrPixelConfig;
        default:
            GrCrash("Unexpected Config8888.");
            return kSkia8888_PM_GrPixelConfig;
    }
}
}

bool SkGpuDevice::onReadPixels(const SkBitmap& bitmap,
                               int x, int y,
                               SkCanvas::Config8888 config8888) {
    SkASSERT(SkBitmap::kARGB_8888_Config == bitmap.config());
    SkASSERT(!bitmap.isNull());
    SkASSERT(SkIRect::MakeWH(this->width(), this->height()).contains(SkIRect::MakeXYWH(x, y, bitmap.width(), bitmap.height())));

    SkAutoLockPixels alp(bitmap);
    GrPixelConfig config;
    config = config8888_to_gr_config(config8888);
    return fContext->readRenderTargetPixels(fRenderTarget,
                                            x, y,
                                            bitmap.width(),
                                            bitmap.height(),
                                            config,
                                            bitmap.getPixels(),
                                            bitmap.rowBytes());
}

void SkGpuDevice::writePixels(const SkBitmap& bitmap, int x, int y,
                              SkCanvas::Config8888 config8888) {
    SkAutoLockPixels alp(bitmap);
    if (!bitmap.readyToDraw()) {
        return;
    }

    GrPixelConfig config;
    if (SkBitmap::kARGB_8888_Config == bitmap.config()) {
        config = config8888_to_gr_config(config8888);
    } else {
        config= SkGr::BitmapConfig2PixelConfig(bitmap.config(),
                                               bitmap.isOpaque());
    }

    fRenderTarget->writePixels(x, y, bitmap.width(), bitmap.height(),
                               config, bitmap.getPixels(), bitmap.rowBytes());
}

///////////////////////////////////////////////////////////////////////////////

static void convert_matrixclip(GrContext* context, const SkMatrix& matrix,
                               const SkClipStack& clipStack,
                               const SkRegion& clipRegion,
                               const SkIPoint& origin) {
    context->setMatrix(matrix);

    SkGrClipIterator iter;
    iter.reset(clipStack);
    const SkIRect& skBounds = clipRegion.getBounds();
    GrRect bounds;
    bounds.setLTRB(GrIntToScalar(skBounds.fLeft),
                   GrIntToScalar(skBounds.fTop),
                   GrIntToScalar(skBounds.fRight),
                   GrIntToScalar(skBounds.fBottom));
    GrClip grc(&iter, GrIntToScalar(-origin.x()), GrIntToScalar(-origin.y()),
               &bounds);
    context->setClip(grc);
}

// call this ever each draw call, to ensure that the context reflects our state,
// and not the state from some other canvas/device
void SkGpuDevice::prepareRenderTarget(const SkDraw& draw) {
    if (fNeedPrepareRenderTarget ||
        fContext->getRenderTarget() != fRenderTarget) {

        fContext->setRenderTarget(fRenderTarget);
        SkASSERT(draw.fClipStack);
        convert_matrixclip(fContext, *draw.fMatrix,
                           *draw.fClipStack, *draw.fClip, this->getOrigin());
        fNeedPrepareRenderTarget = false;
    }
}

void SkGpuDevice::setMatrixClip(const SkMatrix& matrix, const SkRegion& clip,
                                const SkClipStack& clipStack) {
    this->INHERITED::setMatrixClip(matrix, clip, clipStack);
    // We don't need to set them now because the context may not reflect this device.
    fNeedPrepareRenderTarget = true;
}

void SkGpuDevice::gainFocus(SkCanvas* canvas, const SkMatrix& matrix,
                            const SkRegion& clip, const SkClipStack& clipStack) {

    fContext->setRenderTarget(fRenderTarget);

    this->INHERITED::gainFocus(canvas, matrix, clip, clipStack);

    convert_matrixclip(fContext, matrix, clipStack, clip, this->getOrigin());

    if (fNeedClear) {
        fContext->clear(NULL, 0x0);
        fNeedClear = false;
    }
}

SkGpuRenderTarget* SkGpuDevice::accessRenderTarget() { 
    return (SkGpuRenderTarget*)fRenderTarget; 
}

bool SkGpuDevice::bindDeviceAsTexture(GrPaint* paint) {
    if (NULL != fTexture) {
        paint->setTexture(kBitmapTextureIdx, fTexture);
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////

SK_COMPILE_ASSERT(SkShader::kNone_BitmapType == 0, shader_type_mismatch);
SK_COMPILE_ASSERT(SkShader::kDefault_BitmapType == 1, shader_type_mismatch);
SK_COMPILE_ASSERT(SkShader::kRadial_BitmapType == 2, shader_type_mismatch);
SK_COMPILE_ASSERT(SkShader::kSweep_BitmapType == 3, shader_type_mismatch);
SK_COMPILE_ASSERT(SkShader::kTwoPointRadial_BitmapType == 4,
                  shader_type_mismatch);
SK_COMPILE_ASSERT(SkShader::kLast_BitmapType == 4, shader_type_mismatch);

static const GrSamplerState::SampleMode sk_bmp_type_to_sample_mode[] = {
    (GrSamplerState::SampleMode) -1,                    // kNone_BitmapType
    GrSamplerState::kNormal_SampleMode,                 // kDefault_BitmapType
    GrSamplerState::kRadial_SampleMode,                 // kRadial_BitmapType
    GrSamplerState::kSweep_SampleMode,                  // kSweep_BitmapType
    GrSamplerState::kRadial2_SampleMode,                // kTwoPointRadial_BitmapType
};

bool SkGpuDevice::skPaint2GrPaintNoShader(const SkPaint& skPaint,
                                          bool justAlpha,
                                          GrPaint* grPaint,
                                          bool constantColor) {

    grPaint->fDither    = skPaint.isDither();
    grPaint->fAntiAlias = skPaint.isAntiAlias();

    SkXfermode::Coeff sm = SkXfermode::kOne_Coeff;
    SkXfermode::Coeff dm = SkXfermode::kISA_Coeff;

    SkXfermode* mode = skPaint.getXfermode();
    if (mode) {
        if (!mode->asCoeff(&sm, &dm)) {
            //SkDEBUGCODE(SkDebugf("Unsupported xfer mode.\n");)
#if 0
            return false;
#endif
        }
    }
    grPaint->fSrcBlendCoeff = sk_blend_to_grblend(sm);
    grPaint->fDstBlendCoeff = sk_blend_to_grblend(dm);

    if (justAlpha) {
        uint8_t alpha = skPaint.getAlpha();
        grPaint->fColor = GrColorPackRGBA(alpha, alpha, alpha, alpha);
        // justAlpha is currently set to true only if there is a texture,
        // so constantColor should not also be true.
        GrAssert(!constantColor);
    } else {
        grPaint->fColor = SkGr::SkColor2GrColor(skPaint.getColor());
        grPaint->setTexture(kShaderTextureIdx, NULL);
    }
    SkColorFilter* colorFilter = skPaint.getColorFilter();
    SkColor color;
    SkXfermode::Mode filterMode;
    if (colorFilter != NULL && colorFilter->asColorMode(&color, &filterMode)) {
        if (!constantColor) {
            grPaint->fColorFilterColor = SkGr::SkColor2GrColor(color);
            grPaint->fColorFilterXfermode = filterMode;
            return true;
        }
        SkColor filtered = colorFilter->filterColor(skPaint.getColor());
        grPaint->fColor = SkGr::SkColor2GrColor(filtered);
    }
    grPaint->resetColorFilter();
    return true;
}

bool SkGpuDevice::skPaint2GrPaintShader(const SkPaint& skPaint,
                                        SkAutoCachedTexture* act,
                                        const SkMatrix& ctm,
                                        GrPaint* grPaint,
                                        bool constantColor) {

    SkASSERT(NULL != act);

    SkShader* shader = skPaint.getShader();
    if (NULL == shader) {
        return this->skPaint2GrPaintNoShader(skPaint,
                                             false,
                                             grPaint,
                                             constantColor);
    } else if (!this->skPaint2GrPaintNoShader(skPaint, true, grPaint, false)) {
        return false;
    }

    SkBitmap bitmap;
    SkMatrix* matrix = grPaint->textureSampler(kShaderTextureIdx)->matrix();
    SkShader::TileMode tileModes[2];
    SkScalar twoPointParams[3];
    SkShader::BitmapType bmptype = shader->asABitmap(&bitmap, matrix,
                                                     tileModes, twoPointParams);

    GrSamplerState::SampleMode sampleMode = sk_bmp_type_to_sample_mode[bmptype];
    if (-1 == sampleMode) {
        SkShader::GradientInfo info;
        SkColor                color;

        info.fColors = &color;
        info.fColorOffsets = NULL;
        info.fColorCount = 1;
        if (SkShader::kColor_GradientType == shader->asAGradient(&info)) {
            SkPaint copy(skPaint);
            copy.setShader(NULL);
            // modulate the paint alpha by the shader's solid color alpha
            U8CPU newA = SkMulDiv255Round(SkColorGetA(color), copy.getAlpha());
            copy.setColor(SkColorSetA(color, newA));
            return this->skPaint2GrPaintNoShader(copy,
                                                 false,
                                                 grPaint,
                                                 constantColor);
        }
        return false;
    }
    GrSamplerState* sampler = grPaint->textureSampler(kShaderTextureIdx);
    sampler->setSampleMode(sampleMode);
    if (skPaint.isFilterBitmap()) {
        sampler->setFilter(GrSamplerState::kBilinear_Filter);
    } else {
        sampler->setFilter(GrSamplerState::kNearest_Filter);
    }
    sampler->setWrapX(sk_tile_mode_to_grwrap(tileModes[0]));
    sampler->setWrapY(sk_tile_mode_to_grwrap(tileModes[1]));
    if (GrSamplerState::kRadial2_SampleMode == sampleMode) {
        sampler->setRadial2Params(twoPointParams[0],
                                  twoPointParams[1],
                                  twoPointParams[2] < 0);
    }

    GrTexture* texture = act->set(this, bitmap, sampler);
    if (NULL == texture) {
        SkDebugf("Couldn't convert bitmap to texture.\n");
        return false;
    }
    grPaint->setTexture(kShaderTextureIdx, texture);

    // since our texture coords will be in local space, we wack the texture
    // matrix to map them back into 0...1 before we load it
    SkMatrix localM;
    if (shader->getLocalMatrix(&localM)) {
        SkMatrix inverse;
        if (localM.invert(&inverse)) {
            matrix->preConcat(inverse);
        }
    }
    if (SkShader::kDefault_BitmapType == bmptype) {
        GrScalar sx = GrFixedToScalar(GR_Fixed1 / bitmap.width());
        GrScalar sy = GrFixedToScalar(GR_Fixed1 / bitmap.height());
        matrix->postScale(sx, sy);
    } else if (SkShader::kRadial_BitmapType == bmptype) {
        GrScalar s = GrFixedToScalar(GR_Fixed1 / bitmap.width());
        matrix->postScale(s, s);
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////

void SkGpuDevice::clear(SkColor color) {
    fContext->clear(NULL, color);
}

void SkGpuDevice::drawPaint(const SkDraw& draw, const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    GrPaint grPaint;
    SkAutoCachedTexture act;
    if (!this->skPaint2GrPaintShader(paint,
                                     &act,
                                     *draw.fMatrix,
                                     &grPaint,
                                     true)) {
        return;
    }

    fContext->drawPaint(grPaint);
}

// must be in SkCanvas::PointMode order
static const GrPrimitiveType gPointMode2PrimtiveType[] = {
    kPoints_PrimitiveType,
    kLines_PrimitiveType,
    kLineStrip_PrimitiveType
};

void SkGpuDevice::drawPoints(const SkDraw& draw, SkCanvas::PointMode mode,
                             size_t count, const SkPoint pts[], const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    SkScalar width = paint.getStrokeWidth();
    if (width < 0) {
        return;
    }

    // we only handle hairlines here, else we let the SkDraw call our drawPath()
    if (width > 0) {
        draw.drawPoints(mode, count, pts, paint, true);
        return;
    }

    GrPaint grPaint;
    SkAutoCachedTexture act;
    if (!this->skPaint2GrPaintShader(paint,
                                     &act,
                                     *draw.fMatrix,
                                     &grPaint,
                                     true)) {
        return;
    }

    fContext->drawVertices(grPaint,
                           gPointMode2PrimtiveType[mode],
                           count,
                           (GrPoint*)pts,
                           NULL,
                           NULL,
                           NULL,
                           0);
}

///////////////////////////////////////////////////////////////////////////////

void SkGpuDevice::drawRect(const SkDraw& draw, const SkRect& rect,
                          const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    bool doStroke = paint.getStyle() != SkPaint::kFill_Style;
    SkScalar width = paint.getStrokeWidth();

    /*
        We have special code for hairline strokes, miter-strokes, and fills.
        Anything else we just call our path code.
     */
    bool usePath = doStroke && width > 0 &&
                    paint.getStrokeJoin() != SkPaint::kMiter_Join;
    // another reason we might need to call drawPath...
    if (paint.getMaskFilter()) {
        usePath = true;
    }
    // until we aa rotated rects...
    if (!usePath && paint.isAntiAlias() && !draw.fMatrix->rectStaysRect()) {
        usePath = true;
    }
    // small miter limit means right angles show bevel...
    if (SkPaint::kMiter_Join == paint.getStrokeJoin() &&
        paint.getStrokeMiter() < SK_ScalarSqrt2)
    {
        usePath = true;
    }
    // until we can both stroke and fill rectangles
    if (paint.getStyle() == SkPaint::kStrokeAndFill_Style) {
        usePath = true;
    }

    if (usePath) {
        SkPath path;
        path.addRect(rect);
        this->drawPath(draw, path, paint, NULL, true);
        return;
    }

    GrPaint grPaint;
    SkAutoCachedTexture act;
    if (!this->skPaint2GrPaintShader(paint,
                                     &act,
                                     *draw.fMatrix,
                                     &grPaint,
                                     true)) {
        return;
    }
    fContext->drawRect(grPaint, rect, doStroke ? width : -1);
}

#include "SkMaskFilter.h"
#include "SkBounder.h"

static GrPathFill skToGrFillType(SkPath::FillType fillType) {
    switch (fillType) {
        case SkPath::kWinding_FillType:
            return kWinding_PathFill;
        case SkPath::kEvenOdd_FillType:
            return kEvenOdd_PathFill;
        case SkPath::kInverseWinding_FillType:
            return kInverseWinding_PathFill;
        case SkPath::kInverseEvenOdd_FillType:
            return kInverseEvenOdd_PathFill;
        default:
            SkDebugf("Unsupported path fill type\n");
            return kHairLine_PathFill;
    }
}

static void buildKernel(float sigma, float* kernel, int kernelWidth) {
    int halfWidth = (kernelWidth - 1) / 2;
    float sum = 0.0f;
    float denom = 1.0f / (2.0f * sigma * sigma);
    for (int i = 0; i < kernelWidth; ++i) {
        float x = static_cast<float>(i - halfWidth);
        // Note that the constant term (1/(sqrt(2*pi*sigma^2)) of the Gaussian
        // is dropped here, since we renormalize the kernel below.
        kernel[i] = sk_float_exp(- x * x * denom);
        sum += kernel[i];
    }
    // Normalize the kernel
    float scale = 1.0f / sum;
    for (int i = 0; i < kernelWidth; ++i)
        kernel[i] *= scale;
}

static void scaleRect(SkRect* rect, float xScale, float yScale) {
    rect->fLeft *= xScale;
    rect->fTop *= yScale;
    rect->fRight *= xScale;
    rect->fBottom *= yScale;
}

static float adjustSigma(float sigma, int *scaleFactor, int *halfWidth,
                         int *kernelWidth) {
    *scaleFactor = 1;
    while (sigma > MAX_BLUR_SIGMA) {
        *scaleFactor *= 2;
        sigma *= 0.5f;
    }
    *halfWidth = static_cast<int>(ceilf(sigma * 3.0f));
    *kernelWidth = *halfWidth * 2 + 1;
    return sigma;
}

// Apply a Gaussian blur to srcTexture by sigmaX and sigmaY, within the given
// rect.
// temp1 and temp2 are used for allocation of intermediate textures.
// If temp2 is non-NULL, srcTexture will be untouched, and the return 
// value will be either temp1 or temp2.
// If temp2 is NULL, srcTexture will be overwritten with intermediate
// results, and the return value will either be temp1 or srcTexture.
static GrTexture* gaussianBlur(GrContext* context, GrTexture* srcTexture,
                               GrAutoScratchTexture* temp1,
                               GrAutoScratchTexture* temp2,
                               const SkRect& rect,
                               float sigmaX, float sigmaY) {

    GrRenderTarget* oldRenderTarget = context->getRenderTarget();
    GrClip oldClip = context->getClip();
    GrTexture* origTexture = srcTexture;
    GrAutoMatrix avm(context, GrMatrix::I());
    SkIRect clearRect;
    int scaleFactorX, halfWidthX, kernelWidthX;
    int scaleFactorY, halfWidthY, kernelWidthY;
    sigmaX = adjustSigma(sigmaX, &scaleFactorX, &halfWidthX, &kernelWidthX);
    sigmaY = adjustSigma(sigmaY, &scaleFactorY, &halfWidthY, &kernelWidthY);

    SkRect srcRect(rect);
    scaleRect(&srcRect, 1.0f / scaleFactorX, 1.0f / scaleFactorY);
    srcRect.roundOut();
    scaleRect(&srcRect, scaleFactorX, scaleFactorY);
    context->setClip(srcRect);

    const GrTextureDesc desc = {
        kRenderTarget_GrTextureFlagBit | kNoStencil_GrTextureFlagBit,
        kNone_GrAALevel,
        srcRect.width(),
        srcRect.height(),
        kRGBA_8888_GrPixelConfig
    };

    temp1->set(context, desc);
    if (temp2) temp2->set(context, desc);

    GrTexture* dstTexture = temp1->texture();
    GrPaint paint;
    paint.reset();
    paint.textureSampler(0)->setFilter(GrSamplerState::kBilinear_Filter);

    for (int i = 1; i < scaleFactorX || i < scaleFactorY; i *= 2) {
        paint.textureSampler(0)->matrix()->setIDiv(srcTexture->width(),
                                                   srcTexture->height());
        context->setRenderTarget(dstTexture->asRenderTarget());
        SkRect dstRect(srcRect);
        scaleRect(&dstRect, i < scaleFactorX ? 0.5f : 1.0f, 
                            i < scaleFactorY ? 0.5f : 1.0f);
        paint.setTexture(0, srcTexture);
        context->drawRectToRect(paint, dstRect, srcRect);
        srcRect = dstRect;
        SkTSwap(srcTexture, dstTexture);
        // If temp2 is non-NULL, don't render back to origTexture
        if (temp2 && dstTexture == origTexture) dstTexture = temp2->texture();
    }

    if (sigmaX > 0.0f) {
        SkAutoTMalloc<float> kernelStorageX(kernelWidthX);
        float* kernelX = kernelStorageX.get();
        buildKernel(sigmaX, kernelX, kernelWidthX);

        if (scaleFactorX > 1) {
            // Clear out a halfWidth to the right of the srcRect to prevent the
            // X convolution from reading garbage.
            clearRect = SkIRect::MakeXYWH(
                srcRect.fRight, srcRect.fTop, halfWidthX, srcRect.height());
            context->clear(&clearRect, 0x0);
        }

        context->setRenderTarget(dstTexture->asRenderTarget());
        context->convolveInX(srcTexture, srcRect, kernelX, kernelWidthX);
        SkTSwap(srcTexture, dstTexture);
        if (temp2 && dstTexture == origTexture) dstTexture = temp2->texture();
    }

    if (sigmaY > 0.0f) {
        SkAutoTMalloc<float> kernelStorageY(kernelWidthY);
        float* kernelY = kernelStorageY.get();
        buildKernel(sigmaY, kernelY, kernelWidthY);

        if (scaleFactorY > 1 || sigmaX > 0.0f) {
            // Clear out a halfWidth below the srcRect to prevent the Y
            // convolution from reading garbage.
            clearRect = SkIRect::MakeXYWH(
                srcRect.fLeft, srcRect.fBottom, srcRect.width(), halfWidthY);
            context->clear(&clearRect, 0x0);
        }

        context->setRenderTarget(dstTexture->asRenderTarget());
        context->convolveInY(srcTexture, srcRect, kernelY, kernelWidthY);
        SkTSwap(srcTexture, dstTexture);
        if (temp2 && dstTexture == origTexture) dstTexture = temp2->texture();
    }

    if (scaleFactorX > 1 || scaleFactorY > 1) {
        // Clear one pixel to the right and below, to accommodate bilinear
        // upsampling.
        clearRect = SkIRect::MakeXYWH(
            srcRect.fLeft, srcRect.fBottom, srcRect.width() + 1, 1);
        context->clear(&clearRect, 0x0);
        clearRect = SkIRect::MakeXYWH(
            srcRect.fRight, srcRect.fTop, 1, srcRect.height());
        context->clear(&clearRect, 0x0);
        // FIXME:  This should be mitchell, not bilinear.
        paint.textureSampler(0)->setFilter(GrSamplerState::kBilinear_Filter);
        paint.textureSampler(0)->matrix()->setIDiv(srcTexture->width(),
                                                   srcTexture->height());
        context->setRenderTarget(dstTexture->asRenderTarget());
        paint.setTexture(0, srcTexture);
        SkRect dstRect(srcRect);
        scaleRect(&dstRect, scaleFactorX, scaleFactorY);
        context->drawRectToRect(paint, dstRect, srcRect);
        srcRect = dstRect;
        SkTSwap(srcTexture, dstTexture);
    }
    context->setRenderTarget(oldRenderTarget);
    context->setClip(oldClip);
    return srcTexture;
}

static bool drawWithGPUMaskFilter(GrContext* context, const SkPath& path,
                                  SkMaskFilter* filter, const SkMatrix& matrix,
                                  const SkRegion& clip, SkBounder* bounder,
                                  GrPaint* grp) {
#ifdef SK_DISABLE_GPU_BLUR
    return false;
#endif
    SkMaskFilter::BlurInfo info;
    SkMaskFilter::BlurType blurType = filter->asABlur(&info);
    if (SkMaskFilter::kNone_BlurType == blurType ||
        !context->supportsShaders()) {
        return false;
    }
    SkScalar radius = info.fIgnoreTransform ? info.fRadius
                                            : matrix.mapRadius(info.fRadius);
    radius = SkMinScalar(radius, MAX_BLUR_RADIUS);
    if (radius <= 0) {
        return false;
    }
    float sigma = SkScalarToFloat(radius) * BLUR_SIGMA_SCALE;
    float sigma3 = sigma * 3.0f;

    SkRect srcRect = path.getBounds();
    SkRect clipRect;
    clipRect.set(clip.getBounds());

    // Outset srcRect and clipRect by 3 * sigma, to compute affected blur area.
    srcRect.inset(-sigma3, -sigma3);
    clipRect.inset(-sigma3, -sigma3);
    srcRect.intersect(clipRect);
    SkRect finalRect = srcRect;
    SkIRect finalIRect;
    finalRect.roundOut(&finalIRect);
    if (clip.quickReject(finalIRect)) {
        return true;
    }
    if (bounder && !bounder->doIRect(finalIRect)) {
        return true;
    }
    GrPoint offset = GrPoint::Make(-srcRect.fLeft, -srcRect.fTop);
    srcRect.offset(offset);
    const GrTextureDesc desc = {
        kRenderTarget_GrTextureFlagBit,
        kNone_GrAALevel,
        srcRect.width(),
        srcRect.height(),
        // We actually only need A8, but it often isn't supported as a
        // render target
        kRGBA_8888_PM_GrPixelConfig
    };

    GrAutoScratchTexture pathEntry(context, desc);
    GrTexture* pathTexture = pathEntry.texture();
    if (NULL == pathTexture) {
        return false;
    }
    GrRenderTarget* oldRenderTarget = context->getRenderTarget();
    // Once this code moves into GrContext, this should be changed to use
    // an AutoClipRestore.
    GrClip oldClip = context->getClip();
    context->setRenderTarget(pathTexture->asRenderTarget());
    context->setClip(srcRect);
    context->clear(NULL, 0);
    GrPaint tempPaint;
    tempPaint.reset();

    GrAutoMatrix avm(context, GrMatrix::I());
    tempPaint.fAntiAlias = grp->fAntiAlias;
    if (tempPaint.fAntiAlias) {
        // AA uses the "coverage" stages on GrDrawTarget. Coverage with a dst
        // blend coeff of zero requires dual source blending support in order
        // to properly blend partially covered pixels. This means the AA
        // code path may not be taken. So we use a dst blend coeff of ISA. We
        // could special case AA draws to a dst surface with known alpha=0 to
        // use a zero dst coeff when dual source blending isn't available.
        tempPaint.fSrcBlendCoeff = kOne_BlendCoeff;
        tempPaint.fDstBlendCoeff = kISC_BlendCoeff;
    }
    // Draw hard shadow to pathTexture with path topleft at origin 0,0.
    context->drawPath(tempPaint, path, skToGrFillType(path.getFillType()), &offset);

    GrAutoScratchTexture temp1, temp2;
    // If we're doing a normal blur, we can clobber the pathTexture in the
    // gaussianBlur.  Otherwise, we need to save it for later compositing.
    bool isNormalBlur = blurType == SkMaskFilter::kNormal_BlurType;
    GrTexture* blurTexture = gaussianBlur(context, pathTexture,
                                          &temp1, isNormalBlur ? NULL : &temp2,
                                          srcRect, sigma, sigma);

    if (!isNormalBlur) {
        GrPaint paint;
        paint.reset();
        paint.textureSampler(0)->setFilter(GrSamplerState::kNearest_Filter);
        paint.textureSampler(0)->matrix()->setIDiv(pathTexture->width(),
                                                   pathTexture->height());
        // Blend pathTexture over blurTexture.
        context->setRenderTarget(blurTexture->asRenderTarget());
        paint.setTexture(0, pathTexture);
        if (SkMaskFilter::kInner_BlurType == blurType) {
            // inner:  dst = dst * src
            paint.fSrcBlendCoeff = kDC_BlendCoeff;
            paint.fDstBlendCoeff = kZero_BlendCoeff;
        } else if (SkMaskFilter::kSolid_BlurType == blurType) {
            // solid:  dst = src + dst - src * dst
            //             = (1 - dst) * src + 1 * dst
            paint.fSrcBlendCoeff = kIDC_BlendCoeff;
            paint.fDstBlendCoeff = kOne_BlendCoeff;
        } else if (SkMaskFilter::kOuter_BlurType == blurType) {
            // outer:  dst = dst * (1 - src)
            //             = 0 * src + (1 - src) * dst
            paint.fSrcBlendCoeff = kZero_BlendCoeff;
            paint.fDstBlendCoeff = kISC_BlendCoeff;
        }
        context->drawRect(paint, srcRect);
    }
    context->setRenderTarget(oldRenderTarget);
    context->setClip(oldClip);
    
    if (grp->hasTextureOrMask()) {
        GrMatrix inverse;
        if (!matrix.invert(&inverse)) {
            return false;
        }
        grp->preConcatActiveSamplerMatrices(inverse);
    }

    static const int MASK_IDX = GrPaint::kMaxMasks - 1;
    // we assume the last mask index is available for use
    GrAssert(NULL == grp->getMask(MASK_IDX));
    grp->setMask(MASK_IDX, blurTexture);
    grp->maskSampler(MASK_IDX)->reset();

    grp->maskSampler(MASK_IDX)->matrix()->setTranslate(-finalRect.fLeft,
                                                       -finalRect.fTop);
    grp->maskSampler(MASK_IDX)->matrix()->postIDiv(blurTexture->width(),
                                                   blurTexture->height());
    context->drawRect(*grp, finalRect);
    return true;
}

static bool drawWithMaskFilter(GrContext* context, const SkPath& path,
                               SkMaskFilter* filter, const SkMatrix& matrix,
                               const SkRegion& clip, SkBounder* bounder,
                               GrPaint* grp) {
    SkMask  srcM, dstM;

    if (!SkDraw::DrawToMask(path, &clip.getBounds(), filter, &matrix, &srcM,
                            SkMask::kComputeBoundsAndRenderImage_CreateMode)) {
        return false;
    }
    SkAutoMaskFreeImage autoSrc(srcM.fImage);

    if (!filter->filterMask(&dstM, srcM, matrix, NULL)) {
        return false;
    }
    // this will free-up dstM when we're done (allocated in filterMask())
    SkAutoMaskFreeImage autoDst(dstM.fImage);

    if (clip.quickReject(dstM.fBounds)) {
        return false;
    }
    if (bounder && !bounder->doIRect(dstM.fBounds)) {
        return false;
    }

    // we now have a device-aligned 8bit mask in dstM, ready to be drawn using
    // the current clip (and identity matrix) and grpaint settings

    // used to compute inverse view, if necessary
    GrMatrix ivm = context->getMatrix();

    GrAutoMatrix avm(context, GrMatrix::I());

    const GrTextureDesc desc = {
        kNone_GrTextureFlags,
        kNone_GrAALevel,
        dstM.fBounds.width(),
        dstM.fBounds.height(),
        kAlpha_8_GrPixelConfig
    };

    GrAutoScratchTexture ast(context, desc);
    GrTexture* texture = ast.texture();

    if (NULL == texture) {
        return false;
    }
    texture->writePixels(0, 0, desc.fWidth, desc.fHeight, desc.fConfig,
                               dstM.fImage, dstM.fRowBytes);

    if (grp->hasTextureOrMask() && ivm.invert(&ivm)) {
        grp->preConcatActiveSamplerMatrices(ivm);
    }

    static const int MASK_IDX = GrPaint::kMaxMasks - 1;
    // we assume the last mask index is available for use
    GrAssert(NULL == grp->getMask(MASK_IDX));
    grp->setMask(MASK_IDX, texture);
    grp->maskSampler(MASK_IDX)->reset();

    GrRect d;
    d.setLTRB(GrIntToScalar(dstM.fBounds.fLeft),
              GrIntToScalar(dstM.fBounds.fTop),
              GrIntToScalar(dstM.fBounds.fRight),
              GrIntToScalar(dstM.fBounds.fBottom));

    GrMatrix* m = grp->maskSampler(MASK_IDX)->matrix();
    m->setTranslate(-dstM.fBounds.fLeft*SK_Scalar1,
                         -dstM.fBounds.fTop*SK_Scalar1);
    m->postIDiv(texture->width(), texture->height());
    context->drawRect(*grp, d);
    return true;
}

void SkGpuDevice::drawPath(const SkDraw& draw, const SkPath& origSrcPath,
                           const SkPaint& origPaint, const SkMatrix* prePathMatrix,
                           bool pathIsMutable) {
    CHECK_SHOULD_DRAW(draw);

    bool             doFill = true;
    SkTLazy<SkPaint> lazyPaint;
    const SkPaint* paint = &origPaint;
    
    // can we cheat, and threat a thin stroke as a hairline (w/ modulated alpha)
    // if we can, we draw lots faster (raster device does this same test)
    {
        SkAlpha newAlpha;
        if (SkDrawTreatAsHairline(*paint, *draw.fMatrix, &newAlpha)) {
            lazyPaint.set(*paint);
            lazyPaint.get()->setAlpha(newAlpha);
            lazyPaint.get()->setStrokeWidth(0);
            paint = lazyPaint.get();
            doFill = false;
        }
    }
    // must reference paint from here down, and not origPaint
    // since we may have change the paint (using lazyPaint for storage)
    
    GrPaint grPaint;
    SkAutoCachedTexture act;
    if (!this->skPaint2GrPaintShader(*paint,
                                     &act,
                                     *draw.fMatrix,
                                     &grPaint,
                                     true)) {
        return;
    }

    // If we have a prematrix, apply it to the path, optimizing for the case
    // where the original path can in fact be modified in place (even though
    // its parameter type is const).
    SkPath* pathPtr = const_cast<SkPath*>(&origSrcPath);
    SkPath  tmpPath;

    if (prePathMatrix) {
        SkPath* result = pathPtr;

        if (!pathIsMutable) {
            result = &tmpPath;
            pathIsMutable = true;
        }
        // should I push prePathMatrix on our MV stack temporarily, instead
        // of applying it here? See SkDraw.cpp
        pathPtr->transform(*prePathMatrix, result);
        pathPtr = result;
    }
    // at this point we're done with prePathMatrix
    SkDEBUGCODE(prePathMatrix = (const SkMatrix*)0x50FF8001;)

    if (doFill && (paint->getPathEffect() || 
                   paint->getStyle() != SkPaint::kFill_Style)) {
        // it is safe to use tmpPath here, even if we already used it for the
        // prepathmatrix, since getFillPath can take the same object for its
        // input and output safely.
        doFill = paint->getFillPath(*pathPtr, &tmpPath);
        pathPtr = &tmpPath;
    }

    if (paint->getMaskFilter()) {
        // avoid possibly allocating a new path in transform if we can
        SkPath* devPathPtr = pathIsMutable ? pathPtr : &tmpPath;

        // transform the path into device space
        pathPtr->transform(*draw.fMatrix, devPathPtr);
        if (!drawWithGPUMaskFilter(fContext, *devPathPtr, paint->getMaskFilter(),
                                   *draw.fMatrix, *draw.fClip, draw.fBounder,
                                   &grPaint)) {
            drawWithMaskFilter(fContext, *devPathPtr, paint->getMaskFilter(),
                               *draw.fMatrix, *draw.fClip, draw.fBounder,
                               &grPaint);
        }
        return;
    }

    GrPathFill fill = kHairLine_PathFill;

    if (doFill) {
        switch (pathPtr->getFillType()) {
            case SkPath::kWinding_FillType:
                fill = kWinding_PathFill;
                break;
            case SkPath::kEvenOdd_FillType:
                fill = kEvenOdd_PathFill;
                break;
            case SkPath::kInverseWinding_FillType:
                fill = kInverseWinding_PathFill;
                break;
            case SkPath::kInverseEvenOdd_FillType:
                fill = kInverseEvenOdd_PathFill;
                break;
            default:
                SkDebugf("Unsupported path fill type\n");
                return;
        }
    }

    fContext->drawPath(grPaint, *pathPtr, fill);
}

namespace {

inline int get_tile_count(int l, int t, int r, int b, int tileSize)  {
    int tilesX = (r / tileSize) - (l / tileSize) + 1;
    int tilesY = (b / tileSize) - (t / tileSize) + 1;
    return tilesX * tilesY;
}

inline int determine_tile_size(const SkBitmap& bitmap, 
                               const SkIRect* srcRectPtr,
                               int maxTextureSize) {
    static const int kSmallTileSize = 1 << 10;
    if (maxTextureSize <= kSmallTileSize) {
        return maxTextureSize;
    }

    size_t maxTexTotalTileSize;
    size_t smallTotalTileSize;

    if (NULL == srcRectPtr) {
        int w = bitmap.width();
        int h = bitmap.height();
        maxTexTotalTileSize = get_tile_count(0, 0, w, h, maxTextureSize);
        smallTotalTileSize = get_tile_count(0, 0, w, h, kSmallTileSize);
    } else {
        maxTexTotalTileSize = get_tile_count(srcRectPtr->fLeft,
                                             srcRectPtr->fTop,
                                             srcRectPtr->fRight,
                                             srcRectPtr->fBottom,
                                             maxTextureSize);
        smallTotalTileSize = get_tile_count(srcRectPtr->fLeft,
                                            srcRectPtr->fTop,
                                            srcRectPtr->fRight,
                                            srcRectPtr->fBottom,
                                            kSmallTileSize);
    }
    maxTexTotalTileSize *= maxTextureSize * maxTextureSize;
    smallTotalTileSize *= kSmallTileSize * kSmallTileSize;

    if (maxTexTotalTileSize > 2 * smallTotalTileSize) {
        return kSmallTileSize;
    } else {
        return maxTextureSize;
    }
}
}

bool SkGpuDevice::shouldTileBitmap(const SkBitmap& bitmap,
                                   const GrSamplerState& sampler,
                                   const SkIRect* srcRectPtr,
                                   int* tileSize) const {
    SkASSERT(NULL != tileSize);

    // if bitmap is explictly texture backed then just use the texture
    if (NULL != bitmap.getTexture()) {
        return false;
    }
    // if it's larger than the max texture size, then we have no choice but
    // tiling
    const int maxTextureSize = fContext->getMaxTextureSize();
    if (bitmap.width() > maxTextureSize ||
        bitmap.height() > maxTextureSize) {
        *tileSize = determine_tile_size(bitmap, srcRectPtr, maxTextureSize);
        return true;
    }
    // if we are going to have to draw the whole thing, then don't tile
    if (NULL == srcRectPtr) {
        return false;
    }
    // if the entire texture is already in our cache then no reason to tile it
    if (this->isBitmapInTextureCache(bitmap, sampler)) {
        return false;
    }

    // At this point we know we could do the draw by uploading the entire bitmap
    // as a texture. However, if the texture would be large compared to the
    // cache size and we don't require most of it for this draw then tile to
    // reduce the amount of upload and cache spill.

    // assumption here is that sw bitmap size is a good proxy for its size as
    // a texture
    size_t bmpSize = bitmap.getSize();
    size_t cacheSize;
    fContext->getTextureCacheLimits(NULL, &cacheSize);
    if (bmpSize < cacheSize / 2) {
        return false;
    }

    SkFixed fracUsed =
        SkFixedMul((srcRectPtr->width() << 16) / bitmap.width(),
                   (srcRectPtr->height() << 16) / bitmap.height());
    if (fracUsed <= SK_FixedHalf) {
        *tileSize = determine_tile_size(bitmap, srcRectPtr, maxTextureSize);
        return true;
    } else {
        return false;
    }
}

void SkGpuDevice::drawBitmap(const SkDraw& draw,
                             const SkBitmap& bitmap,
                             const SkIRect* srcRectPtr,
                             const SkMatrix& m,
                             const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    SkIRect srcRect;
    if (NULL == srcRectPtr) {
        srcRect.set(0, 0, bitmap.width(), bitmap.height());
    } else {
        srcRect = *srcRectPtr;
    }

    if (paint.getMaskFilter()){
        // Convert the bitmap to a shader so that the rect can be drawn
        // through drawRect, which supports mask filters.
        SkBitmap        tmp;    // subset of bitmap, if necessary
        const SkBitmap* bitmapPtr = &bitmap;
        if (srcRectPtr) {
            if (!bitmap.extractSubset(&tmp, srcRect)) {
                return;     // extraction failed
            }
            bitmapPtr = &tmp;
            srcRect.set(0,0, srcRect.width(), srcRect.height());
        }
        SkPaint paintWithTexture(paint);
        paintWithTexture.setShader(SkShader::CreateBitmapShader( *bitmapPtr,
            SkShader::kClamp_TileMode, SkShader::kClamp_TileMode))->unref();
        SkRect ScalarRect;
        ScalarRect.set(srcRect);

        // Transform 'm' needs to be concatenated to the draw matrix,
        // rather than transforming the primitive directly, so that 'm' will 
        // also affect the behavior of the mask filter.
        SkMatrix drawMatrix;
        drawMatrix.setConcat(*draw.fMatrix, m);
        SkDraw transformedDraw(draw);
        transformedDraw.fMatrix = &drawMatrix;

        this->drawRect(transformedDraw, ScalarRect, paintWithTexture);

        return;
    }

    GrPaint grPaint;
    if (!this->skPaint2GrPaintNoShader(paint, true, &grPaint, false)) {
        return;
    }
    GrSamplerState* sampler = grPaint.textureSampler(kBitmapTextureIdx);
    if (paint.isFilterBitmap()) {
        sampler->setFilter(GrSamplerState::kBilinear_Filter);
    } else {
        sampler->setFilter(GrSamplerState::kNearest_Filter);
    }

    int tileSize;
    if (!this->shouldTileBitmap(bitmap, *sampler, srcRectPtr, &tileSize)) {
        // take the simple case
        this->internalDrawBitmap(draw, bitmap, srcRect, m, &grPaint);
        return;
    }

    // undo the translate done by SkCanvas
    int DX = SkMax32(0, srcRect.fLeft);
    int DY = SkMax32(0, srcRect.fTop);
    // compute clip bounds in local coordinates
    SkIRect clipRect;
    {
        SkRect r;
        r.set(draw.fClip->getBounds());
        SkMatrix matrix, inverse;
        matrix.setConcat(*draw.fMatrix, m);
        if (!matrix.invert(&inverse)) {
            return;
        }
        inverse.mapRect(&r);
        r.roundOut(&clipRect);
        // apply the canvas' translate to our local clip
        clipRect.offset(DX, DY);
    }

    int nx = bitmap.width() / tileSize;
    int ny = bitmap.height() / tileSize;
    for (int x = 0; x <= nx; x++) {
        for (int y = 0; y <= ny; y++) {
            SkIRect tileR;
            tileR.set(x * tileSize, y * tileSize,
                      (x + 1) * tileSize, (y + 1) * tileSize);
            if (!SkIRect::Intersects(tileR, clipRect)) {
                continue;
            }

            SkIRect srcR = tileR;
            if (!srcR.intersect(srcRect)) {
                continue;
            }

            SkBitmap tmpB;
            if (bitmap.extractSubset(&tmpB, tileR)) {
                // now offset it to make it "local" to our tmp bitmap
                srcR.offset(-tileR.fLeft, -tileR.fTop);

                SkMatrix tmpM(m);
                {
                    int dx = tileR.fLeft - DX + SkMax32(0, srcR.fLeft);
                    int dy = tileR.fTop -  DY + SkMax32(0, srcR.fTop);
                    tmpM.preTranslate(SkIntToScalar(dx), SkIntToScalar(dy));
                }
                this->internalDrawBitmap(draw, tmpB, srcR, tmpM, &grPaint);
            }
        }
    }
}

/*
 *  This is called by drawBitmap(), which has to handle images that may be too
 *  large to be represented by a single texture.
 *
 *  internalDrawBitmap assumes that the specified bitmap will fit in a texture
 *  and that non-texture portion of the GrPaint has already been setup.
 */
void SkGpuDevice::internalDrawBitmap(const SkDraw& draw,
                                     const SkBitmap& bitmap,
                                     const SkIRect& srcRect,
                                     const SkMatrix& m,
                                     GrPaint* grPaint) {
    SkASSERT(bitmap.width() <= fContext->getMaxTextureSize() &&
             bitmap.height() <= fContext->getMaxTextureSize());

    SkAutoLockPixels alp(bitmap, !bitmap.getTexture());
    if (!bitmap.getTexture() && !bitmap.readyToDraw()) {
        SkDebugf("nothing to draw\n");
        return;
    }

    GrSamplerState* sampler = grPaint->textureSampler(kBitmapTextureIdx);

    sampler->setWrapX(GrSamplerState::kClamp_WrapMode);
    sampler->setWrapY(GrSamplerState::kClamp_WrapMode);
    sampler->setSampleMode(GrSamplerState::kNormal_SampleMode);
    sampler->matrix()->reset();

    GrTexture* texture;
    SkAutoCachedTexture act(this, bitmap, sampler, &texture);
    if (NULL == texture) {
        return;
    }

    grPaint->setTexture(kBitmapTextureIdx, texture);

    GrRect dstRect = SkRect::MakeWH(GrIntToScalar(srcRect.width()),
                                    GrIntToScalar(srcRect.height()));
    GrRect paintRect;
    paintRect.setLTRB(GrFixedToScalar((srcRect.fLeft << 16) / bitmap.width()),
                      GrFixedToScalar((srcRect.fTop << 16) / bitmap.height()),
                      GrFixedToScalar((srcRect.fRight << 16) / bitmap.width()),
                      GrFixedToScalar((srcRect.fBottom << 16) / bitmap.height()));

    if (GrSamplerState::kNearest_Filter != sampler->getFilter() &&
        (srcRect.width() < bitmap.width() || 
         srcRect.height() < bitmap.height())) {
        // If drawing a subrect of the bitmap and filtering is enabled,
        // use a constrained texture domain to avoid color bleeding
        GrScalar left, top, right, bottom;
        if (srcRect.width() > 1) {
            GrScalar border = GR_ScalarHalf / bitmap.width();
            left = paintRect.left() + border;
            right = paintRect.right() - border;
        } else {
            left = right = GrScalarHalf(paintRect.left() + paintRect.right());
        }
        if (srcRect.height() > 1) {
            GrScalar border = GR_ScalarHalf / bitmap.height();
            top = paintRect.top() + border;
            bottom = paintRect.bottom() - border;
        } else {
            top = bottom = GrScalarHalf(paintRect.top() + paintRect.bottom());
        }
        GrRect textureDomain;
        textureDomain.setLTRB(left, top, right, bottom);
        sampler->setTextureDomain(textureDomain);
    }

    fContext->drawRectToRect(*grPaint, dstRect, paintRect, &m);
}

void SkGpuDevice::drawSprite(const SkDraw& draw, const SkBitmap& bitmap,
                            int left, int top, const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    SkAutoLockPixels alp(bitmap);
    if (!bitmap.getTexture() && !bitmap.readyToDraw()) {
        return;
    }

    GrPaint grPaint;
    if(!this->skPaint2GrPaintNoShader(paint, true, &grPaint, false)) {
        return;
    }

    GrAutoMatrix avm(fContext, GrMatrix::I());

    GrSamplerState* sampler = grPaint.textureSampler(kBitmapTextureIdx);

    GrTexture* texture;
    sampler->reset();
    SkAutoCachedTexture act(this, bitmap, sampler, &texture);

    grPaint.setTexture(kBitmapTextureIdx, texture);

    fContext->drawRectToRect(grPaint,
                             GrRect::MakeXYWH(GrIntToScalar(left),
                                              GrIntToScalar(top),
                                              GrIntToScalar(bitmap.width()),
                                              GrIntToScalar(bitmap.height())),
                             GrRect::MakeWH(GR_Scalar1, GR_Scalar1));
}

void SkGpuDevice::drawDevice(const SkDraw& draw, SkDevice* dev,
                            int x, int y, const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    GrPaint grPaint;
    if (!((SkGpuDevice*)dev)->bindDeviceAsTexture(&grPaint) ||
        !this->skPaint2GrPaintNoShader(paint, true, &grPaint, false)) {
        return;
    }

    GrTexture* devTex = grPaint.getTexture(0);
    SkASSERT(NULL != devTex);

    const SkBitmap& bm = dev->accessBitmap(false);
    int w = bm.width();
    int h = bm.height();

    GrAutoMatrix avm(fContext, GrMatrix::I());

    grPaint.textureSampler(kBitmapTextureIdx)->reset();

    GrRect dstRect = GrRect::MakeXYWH(GrIntToScalar(x),
                                      GrIntToScalar(y),
                                      GrIntToScalar(w),
                                      GrIntToScalar(h));
    SkImageFilter* imageFilter = paint.getImageFilter();
    SkSize size;
    if (NULL != imageFilter && imageFilter->asABlur(&size)) {
        GrAutoScratchTexture temp1, temp2;
        GrTexture* blurTexture = gaussianBlur(fContext,
                                              devTex, &temp1, &temp2,
                                              GrRect::MakeWH(w, h),
                                              size.width(),
                                              size.height());
        grPaint.setTexture(kBitmapTextureIdx, blurTexture);
        devTex = blurTexture;
    }
    // The device being drawn may not fill up its texture (saveLayer uses
    // the approximate ).
    GrRect srcRect = GrRect::MakeWH(GR_Scalar1 * w / devTex->width(),
                                    GR_Scalar1 * h / devTex->height());

    fContext->drawRectToRect(grPaint, dstRect, srcRect);
}

///////////////////////////////////////////////////////////////////////////////

// must be in SkCanvas::VertexMode order
static const GrPrimitiveType gVertexMode2PrimitiveType[] = {
    kTriangles_PrimitiveType,
    kTriangleStrip_PrimitiveType,
    kTriangleFan_PrimitiveType,
};

void SkGpuDevice::drawVertices(const SkDraw& draw, SkCanvas::VertexMode vmode,
                              int vertexCount, const SkPoint vertices[],
                              const SkPoint texs[], const SkColor colors[],
                              SkXfermode* xmode,
                              const uint16_t indices[], int indexCount,
                              const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    GrPaint grPaint;
    SkAutoCachedTexture act;
    // we ignore the shader if texs is null.
    if (NULL == texs) {
        if (!this->skPaint2GrPaintNoShader(paint,
                                           false,
                                           &grPaint, 
                                           NULL == colors)) {
            return;
        }
    } else {
        if (!this->skPaint2GrPaintShader(paint, &act,
                                         *draw.fMatrix,
                                         &grPaint,
                                         NULL == colors)) {
            return;
        }
    }

    if (NULL != xmode && NULL != texs && NULL != colors) {
        if (!SkXfermode::IsMode(xmode, SkXfermode::kMultiply_Mode)) {
            SkDebugf("Unsupported vertex-color/texture xfer mode.\n");
#if 0
            return
#endif
        }
    }

    SkAutoSTMalloc<128, GrColor> convertedColors(0);
    if (NULL != colors) {
        // need to convert byte order and from non-PM to PM
        convertedColors.reset(vertexCount);
        for (int i = 0; i < vertexCount; ++i) {
            convertedColors[i] = SkGr::SkColor2GrColor(colors[i]);
        }
        colors = convertedColors.get();
    }
    fContext->drawVertices(grPaint,
                           gVertexMode2PrimitiveType[vmode],
                           vertexCount,
                           (GrPoint*) vertices,
                           (GrPoint*) texs,
                           colors,
                           indices,
                           indexCount);
}

///////////////////////////////////////////////////////////////////////////////

static void GlyphCacheAuxProc(void* data) {
    delete (GrFontScaler*)data;
}

static GrFontScaler* get_gr_font_scaler(SkGlyphCache* cache) {
    void* auxData;
    GrFontScaler* scaler = NULL;
    if (cache->getAuxProcData(GlyphCacheAuxProc, &auxData)) {
        scaler = (GrFontScaler*)auxData;
    }
    if (NULL == scaler) {
        scaler = new SkGrFontScaler(cache);
        cache->setAuxProc(GlyphCacheAuxProc, scaler);
    }
    return scaler;
}

static void SkGPU_Draw1Glyph(const SkDraw1Glyph& state,
                             SkFixed fx, SkFixed fy,
                             const SkGlyph& glyph) {
    SkASSERT(glyph.fWidth > 0 && glyph.fHeight > 0);

    GrSkDrawProcs* procs = (GrSkDrawProcs*)state.fDraw->fProcs;

    if (NULL == procs->fFontScaler) {
        procs->fFontScaler = get_gr_font_scaler(state.fCache);
    }

    /*
     *  What should we do with fy? (assuming horizontal/latin text)
     *
     *  The raster code calls SkFixedFloorToFixed on it, as it does with fx.
     *  It calls that rather than round, because our caller has already added
     *  SK_FixedHalf, so that calling floor gives us the rounded integer.
     *
     *  Test code between raster and gpu (they should draw the same)
     *
     *      canvas->drawText("Hamburgefons", 12, 0, 16.5f, paint);
     *
     *  Perhaps we should only perform this integralization if there is no
     *  fExtMatrix...
     */
    fy = SkFixedFloorToFixed(fy);

    procs->fTextContext->drawPackedGlyph(GrGlyph::Pack(glyph.getGlyphID(), fx, 0),
                                         SkFixedFloorToFixed(fx), fy,
                                         procs->fFontScaler);
}

SkDrawProcs* SkGpuDevice::initDrawForText(GrTextContext* context) {

    // deferred allocation
    if (NULL == fDrawProcs) {
        fDrawProcs = new GrSkDrawProcs;
        fDrawProcs->fD1GProc = SkGPU_Draw1Glyph;
        fDrawProcs->fContext = fContext;
    }

    // init our (and GL's) state
    fDrawProcs->fTextContext = context;
    fDrawProcs->fFontScaler = NULL;
    return fDrawProcs;
}

void SkGpuDevice::drawText(const SkDraw& draw, const void* text,
                          size_t byteLength, SkScalar x, SkScalar y,
                          const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    if (draw.fMatrix->hasPerspective()) {
        // this guy will just call our drawPath()
        draw.drawText((const char*)text, byteLength, x, y, paint);
    } else {
        SkDraw myDraw(draw);

        GrPaint grPaint;
        SkAutoCachedTexture act;

        if (!this->skPaint2GrPaintShader(paint,
                                         &act,
                                         *draw.fMatrix,
                                         &grPaint,
                                         true)) {
            return;
        }
        GrTextContext context(fContext, grPaint, draw.fExtMatrix);
        myDraw.fProcs = this->initDrawForText(&context);
        this->INHERITED::drawText(myDraw, text, byteLength, x, y, paint);
    }
}

void SkGpuDevice::drawPosText(const SkDraw& draw, const void* text,
                             size_t byteLength, const SkScalar pos[],
                             SkScalar constY, int scalarsPerPos,
                             const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    if (draw.fMatrix->hasPerspective()) {
        // this guy will just call our drawPath()
        draw.drawPosText((const char*)text, byteLength, pos, constY,
                         scalarsPerPos, paint);
    } else {
        SkDraw myDraw(draw);

        GrPaint grPaint;
        SkAutoCachedTexture act;
        if (!this->skPaint2GrPaintShader(paint,
                                         &act,
                                         *draw.fMatrix,
                                         &grPaint,
                                         true)) {
            return;
        }

        GrTextContext context(fContext, grPaint, draw.fExtMatrix);
        myDraw.fProcs = this->initDrawForText(&context);
        this->INHERITED::drawPosText(myDraw, text, byteLength, pos, constY,
                                     scalarsPerPos, paint);
    }
}

void SkGpuDevice::drawTextOnPath(const SkDraw& draw, const void* text,
                                size_t len, const SkPath& path,
                                const SkMatrix* m, const SkPaint& paint) {
    CHECK_SHOULD_DRAW(draw);

    SkASSERT(draw.fDevice == this);
    draw.drawTextOnPath((const char*)text, len, path, m, paint);
}

///////////////////////////////////////////////////////////////////////////////

bool SkGpuDevice::filterTextFlags(const SkPaint& paint, TextFlags* flags) {
    if (!paint.isLCDRenderText()) {
        // we're cool with the paint as is
        return false;
    }

    if (paint.getShader() ||
        paint.getXfermode() || // unless its srcover
        paint.getMaskFilter() ||
        paint.getRasterizer() ||
        paint.getColorFilter() ||
        paint.getPathEffect() ||
        paint.isFakeBoldText() ||
        paint.getStyle() != SkPaint::kFill_Style) {
        // turn off lcd
        flags->fFlags = paint.getFlags() & ~SkPaint::kLCDRenderText_Flag;
        flags->fHinting = paint.getHinting();
        return true;
    }
    // we're cool with the paint as is
    return false;
}

void SkGpuDevice::flush() {
    fContext->flush(false);
}

///////////////////////////////////////////////////////////////////////////////

SkGpuDevice::TexCache SkGpuDevice::lockCachedTexture(const SkBitmap& bitmap,
                                            const GrSamplerState* sampler,
                                            TexType type) {
    GrContext::TextureCacheEntry entry;
    GrContext* ctx = this->context();

    if (kBitmap_TexType != type) {
        const GrTextureDesc desc = {
            kRenderTarget_GrTextureFlagBit,
            kNone_GrAALevel,
            bitmap.width(),
            bitmap.height(),
            SkGr::Bitmap2PixelConfig(bitmap)
        };
        GrContext::ScratchTexMatch match;
        if (kSaveLayerDeviceRenderTarget_TexType == type) {
            // we know layers will only be drawn through drawDevice.
            // drawDevice has been made to work with content embedded in a
            // larger texture so its okay to use the approximate version.
            match = GrContext::kApprox_ScratchTexMatch;
        } else {
            SkASSERT(kDeviceRenderTarget_TexType == type);
            match = GrContext::kExact_ScratchTexMatch;
        }
        entry = ctx->lockScratchTexture(desc, match);
    } else {
        if (!bitmap.isVolatile()) {
            GrContext::TextureKey key = bitmap.getGenerationID();
            key |= ((uint64_t) bitmap.pixelRefOffset()) << 32;
        
            entry = ctx->findAndLockTexture(key, bitmap.width(),
                                            bitmap.height(), sampler);
            if (NULL == entry.texture()) {
                entry = sk_gr_create_bitmap_texture(ctx, key, sampler,
                                                    bitmap);
            }
        } else {
            entry = sk_gr_create_bitmap_texture(ctx, gUNCACHED_KEY,
                                                sampler, bitmap);
        }
        if (NULL == entry.texture()) {
            GrPrintf("---- failed to create texture for cache [%d %d]\n",
                     bitmap.width(), bitmap.height());
        }
    }
    return entry;
}

void SkGpuDevice::unlockCachedTexture(TexCache cache) {
    this->context()->unlockTexture(cache);
}

bool SkGpuDevice::isBitmapInTextureCache(const SkBitmap& bitmap,
                                         const GrSamplerState& sampler) const {
    GrContext::TextureKey key = bitmap.getGenerationID();
    key |= ((uint64_t) bitmap.pixelRefOffset()) << 32;
    return this->context()->isTextureInCache(key, bitmap.width(),
                                             bitmap.height(), &sampler);

}


SkDevice* SkGpuDevice::onCreateCompatibleDevice(SkBitmap::Config config, 
                                                int width, int height, 
                                                bool isOpaque,
                                                Usage usage) {
    return SkNEW_ARGS(SkGpuDevice,(this->context(), config, 
                                   width, height, usage));
}

