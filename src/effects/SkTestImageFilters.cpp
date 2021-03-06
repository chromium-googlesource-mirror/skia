#include "SkTestImageFilters.h"
#include "SkCanvas.h"

bool SkOffsetImageFilter::onFilterImage(const SkBitmap& src,
                                        const SkMatrix& matrix,
                                        SkBitmap* result,
                                        SkIPoint* loc) {
    SkVector vec;
    matrix.mapVectors(&vec, &fOffset, 1);
    
    loc->fX += SkScalarRoundToInt(vec.fX);
    loc->fY += SkScalarRoundToInt(vec.fY);
    *result = src;
    return true;
}

bool SkOffsetImageFilter::onFilterBounds(const SkIRect& src, const SkMatrix& ctm,
                                         SkIRect* dst) {
    SkVector vec;
    ctm.mapVectors(&vec, &fOffset, 1);

    *dst = src;
    dst->offset(SkScalarRoundToInt(vec.fX), SkScalarRoundToInt(vec.fY));
    return true;
}

void SkOffsetImageFilter::flatten(SkFlattenableWriteBuffer& buffer) {
    this->INHERITED::flatten(buffer);
    buffer.writeScalar(fOffset.x());
    buffer.writeScalar(fOffset.y());
}

SkOffsetImageFilter::SkOffsetImageFilter(SkFlattenableReadBuffer& buffer) : INHERITED(buffer) {
    fOffset.fX = buffer.readScalar();
    fOffset.fY = buffer.readScalar();
}

SkFlattenable::Factory SkOffsetImageFilter::getFactory() {
    return CreateProc;
}

///////////////////////////////////////////////////////////////////////////////

SkComposeImageFilter::~SkComposeImageFilter() {
    SkSafeUnref(fInner);
    SkSafeUnref(fOuter);
}

bool SkComposeImageFilter::onFilterImage(const SkBitmap& src,
                                         const SkMatrix& ctm,
                                         SkBitmap* result,
                                         SkIPoint* loc) {
    if (!fOuter && !fInner) {
        return false;
    }
    
    if (!fOuter || !fInner) {
        return (fOuter ? fOuter : fInner)->filterImage(src, ctm, result, loc);
    }
    
    SkBitmap tmp;
    return fInner->filterImage(src, ctm, &tmp, loc) &&
    fOuter->filterImage(tmp, ctm, result, loc);
}

bool SkComposeImageFilter::onFilterBounds(const SkIRect& src,
                                          const SkMatrix& ctm,
                                          SkIRect* dst) {
    if (!fOuter && !fInner) {
        return false;
    }
    
    if (!fOuter || !fInner) {
        return (fOuter ? fOuter : fInner)->filterBounds(src, ctm, dst);
    }
    
    SkIRect tmp;
    return fInner->filterBounds(src, ctm, &tmp) &&
           fOuter->filterBounds(tmp, ctm, dst);
}

void SkComposeImageFilter::flatten(SkFlattenableWriteBuffer& buffer) {
    this->INHERITED::flatten(buffer);

    buffer.writeFlattenable(fOuter);
    buffer.writeFlattenable(fInner);
}

SkComposeImageFilter::SkComposeImageFilter(SkFlattenableReadBuffer& buffer) : INHERITED(buffer) {
    fOuter = (SkImageFilter*)buffer.readFlattenable();
    fInner = (SkImageFilter*)buffer.readFlattenable();
}

SkFlattenable::Factory SkComposeImageFilter::getFactory() {
    return CreateProc;
}

///////////////////////////////////////////////////////////////////////////////

template <typename T> T* SkSafeRefReturn(T* obj) {
    SkSafeRef(obj);
    return obj;
}

void SkMergeImageFilter::initAlloc(int count, bool hasModes) {
    if (count < 1) {
        fFilters = NULL;
        fModes = NULL;
        fCount = 0;
    } else {
        int modeCount = hasModes ? count : 0;
        size_t size = sizeof(SkImageFilter*) * count + sizeof(uint8_t) * modeCount;
        if (size <= sizeof(fStorage)) {
            fFilters = SkTCast<SkImageFilter**>(fStorage);
        } else {
            fFilters = SkTCast<SkImageFilter**>(sk_malloc_throw(size));
        }
        fModes = hasModes ? SkTCast<uint8_t*>(fFilters + count) : NULL;
        fCount = count;
    }
}

void SkMergeImageFilter::init(SkImageFilter* const filters[], int count,
                              const SkXfermode::Mode modes[]) {
    this->initAlloc(count, !!modes);
    for (int i = 0; i < count; ++i) {
        fFilters[i] = SkSafeRefReturn(filters[i]);
        if (modes) {
            fModes[i] = SkToU8(modes[i]);
        }
    }
}

SkMergeImageFilter::SkMergeImageFilter(SkImageFilter* first, SkImageFilter* second,
                                       SkXfermode::Mode mode) {
    SkImageFilter* filters[] = { first, second };
    SkXfermode::Mode modes[] = { mode, mode };
    this->init(filters, 2, SkXfermode::kSrcOver_Mode == mode ? NULL : modes);
}

SkMergeImageFilter::SkMergeImageFilter(SkImageFilter* const filters[], int count,
                                       const SkXfermode::Mode modes[]) {
    this->init(filters, count, modes);
}

SkMergeImageFilter::~SkMergeImageFilter() {
    for (int i = 0; i < fCount; ++i) {
        SkSafeUnref(fFilters[i]);
    }

    if (fFilters != SkTCast<SkImageFilter**>(fStorage)) {
        sk_free(fFilters);
        // fModes is allocated in the same block as fFilters, so no need to
        // separately free it.
    }
}

bool SkMergeImageFilter::onFilterBounds(const SkIRect& src, const SkMatrix& ctm,
                                        SkIRect* dst) {
    if (fCount < 1) {
        return false;
    }

    SkIRect totalBounds;
    
    for (int i = 0; i < fCount; ++i) {
        SkImageFilter* filter = fFilters[i];
        SkIRect r;
        if (filter) {
            if (!filter->filterBounds(src, ctm, &r)) {
                return false;
            }
        } else {
            r = src;
        }
        if (0 == i) {
            totalBounds = r;
        } else {
            totalBounds.join(r);
        }
    }

    // don't modify dst until now, so we don't accidentally change it in the
    // loop, but then return false on the next filter.
    *dst = totalBounds;
    return true;
}

bool SkMergeImageFilter::onFilterImage(const SkBitmap& src, const SkMatrix& ctm,
                                       SkBitmap* result, SkIPoint* loc) {
    if (fCount < 1) {
        return false;
    }

    const SkIRect srcBounds = SkIRect::MakeXYWH(loc->x(), loc->y(),
                                                src.width(), src.height());
    SkIRect bounds;
    if (!this->filterBounds(srcBounds, ctm, &bounds)) {
        return false;
    }

    const int x0 = bounds.left();
    const int y0 = bounds.top();

    SkBitmap dst;
    dst.setConfig(SkBitmap::kARGB_8888_Config, bounds.width(), bounds.height());
    dst.allocPixels();
    dst.eraseColor(0);

    SkCanvas canvas(dst);
    SkPaint paint;

    for (int i = 0; i < fCount; ++i) {
        SkBitmap tmp;
        const SkBitmap* srcPtr;
        SkIPoint pos = *loc;
        SkImageFilter* filter = fFilters[i];
        if (filter) {
            if (!filter->filterImage(src, ctm, &tmp, &pos)) {
                return false;
            }
            srcPtr = &tmp;
        } else {
            srcPtr = &src;
        }
        
        if (fModes) {
            paint.setXfermodeMode((SkXfermode::Mode)fModes[i]);
        } else {
            paint.setXfermode(NULL);
        }
        canvas.drawSprite(*srcPtr, pos.x() - x0, pos.y() - y0, &paint);
    }

    loc->set(bounds.left(), bounds.top());
    result->swap(dst);
    return true;
}

void SkMergeImageFilter::flatten(SkFlattenableWriteBuffer& buffer) {
    this->INHERITED::flatten(buffer);

    int storedCount = fCount;
    if (fModes) {
        // negative count signals we have modes
        storedCount = -storedCount;
    }
    buffer.write32(storedCount);

    if (fCount) {
        for (int i = 0; i < fCount; ++i) {
            buffer.writeFlattenable(fFilters[i]);
        }
        if (fModes) {
            buffer.write(fModes, fCount * sizeof(fModes[0]));
        }
    }
}

SkMergeImageFilter::SkMergeImageFilter(SkFlattenableReadBuffer& buffer) : INHERITED(buffer) {
    int storedCount = buffer.readS32();
    this->initAlloc(SkAbs32(storedCount), storedCount < 0);

    for (int i = 0; i < fCount; ++i) {
        fFilters[i] = (SkImageFilter*)buffer.readFlattenable();
    }

    if (fModes) {
        SkASSERT(storedCount < 0);
        buffer.read(fModes, fCount * sizeof(fModes[0]));
    } else {
        SkASSERT(storedCount >= 0);
    }
}

SkFlattenable::Factory SkMergeImageFilter::getFactory() {
    return CreateProc;
}

///////////////////////////////////////////////////////////////////////////////

#include "SkColorFilter.h"

SkColorFilterImageFilter::~SkColorFilterImageFilter() {
    SkSafeUnref(fColorFilter);
}

bool SkColorFilterImageFilter::onFilterImage(const SkBitmap& src,
                                             const SkMatrix& matrix,
                                             SkBitmap* result,
                                             SkIPoint* loc) {
    if (SkBitmap::kARGB_8888_Config != src.config()) {
        return false;
    }

    SkColorFilter* cf = fColorFilter;
    if (NULL == cf) {
        *result = src;
        return true;
    }

    SkAutoLockPixels alpsrc(src);
    if (!src.readyToDraw()) {
        return false;
    }

    SkBitmap dst(src);
    dst.allocPixels();
    if (!dst.readyToDraw()) {
        return false;
    }
    
    for (int y = 0; y < src.height(); ++y) {
        cf->filterSpan(src.getAddr32(0, y), src.width(), dst.getAddr32(0, y));
    }
    
    result->swap(dst);
    return true;
}

void SkColorFilterImageFilter::flatten(SkFlattenableWriteBuffer& buffer) {
    this->INHERITED::flatten(buffer);
    
    buffer.writeFlattenable(fColorFilter);
}

SkColorFilterImageFilter::SkColorFilterImageFilter(SkFlattenableReadBuffer& buffer) : INHERITED(buffer) {
    fColorFilter = (SkColorFilter*)buffer.readFlattenable();
}

SkFlattenable::Factory SkColorFilterImageFilter::getFactory() {
    return CreateProc;
}

///////////////////////////////////////////////////////////////////////////////

bool SkDownSampleImageFilter::onFilterImage(const SkBitmap& src,
                                            const SkMatrix& matrix,
                                            SkBitmap* result, SkIPoint*) {
    SkScalar scale = fScale;
    if (scale > SK_Scalar1 || scale <= 0) {
        return false;
    }
    
    int dstW = SkScalarRoundToInt(src.width() * scale);
    int dstH = SkScalarRoundToInt(src.height() * scale);
    if (dstW < 1) {
        dstW = 1;
    }
    if (dstH < 1) {
        dstH = 1;
    }
    
    SkBitmap dst;
    dst.setConfig(SkBitmap::kARGB_8888_Config, dstW, dstH);
    dst.allocPixels();
    dst.eraseColor(0);
    
    // downsample
    {
        SkPaint paint;
        paint.setFilterBitmap(true);
        
        SkCanvas canvas(dst);
        canvas.scale(scale, scale);
        canvas.drawBitmap(src, 0, 0, &paint);
    }
    
    result->setConfig(SkBitmap::kARGB_8888_Config, src.width(), src.height());
    result->allocPixels();
    result->eraseColor(0);
    
    // upscale
    {
        SkRect r = SkRect::MakeWH(SkIntToScalar(result->width()),
                                  SkIntToScalar(result->height()));
        SkCanvas canvas(*result);
        canvas.drawBitmapRect(dst, NULL, r, NULL);
    }
    return true;
}

void SkDownSampleImageFilter::flatten(SkFlattenableWriteBuffer& buffer) {
    this->INHERITED::flatten(buffer);
    
    buffer.writeScalar(fScale);
}

SkDownSampleImageFilter::SkDownSampleImageFilter(SkFlattenableReadBuffer& buffer) : INHERITED(buffer) {
    fScale = buffer.readScalar();
}

SkFlattenable::Factory SkDownSampleImageFilter::getFactory() {
    return CreateProc;
}

