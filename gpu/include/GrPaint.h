/*
    Copyright 2011 Google Inc.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

         http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
 */

#ifndef GrPaint_DEFINED
#define GrPaint_DEFINED

#include "GrTexture.h"
#include "GrSamplerState.h"
#include "GrDrawTarget.h"

/**
 * The paint describes how pixels are colored when the context draws to
 * them.
 */
class GrPaint {
public:

    // All the paint fields are public except texture (it's ref-counted)
    GrDrawTarget::BlendCoeff    fSrcBlendCoeff;
    GrDrawTarget::BlendCoeff    fDstBlendCoeff;
    bool                        fAntiAlias;
    bool                        fDither;

    GrColor                     fColor;

    GrMatrix                    fTextureMatrix;
    GrSamplerState              fSampler;

    void setTexture(GrTexture* texture) {
        GrSafeRef(texture);
        GrSafeUnref(fTexture);
        fTexture = texture;
    }

    GrTexture* getTexture() const { return fTexture; }

    // uninitialized
    GrPaint() {
        fTexture = NULL;
    }

    GrPaint(const GrPaint& paint) {
        fSrcBlendCoeff = paint.fSrcBlendCoeff;
        fDstBlendCoeff = paint.fDstBlendCoeff;
        fAntiAlias = paint.fAntiAlias;
        fDither = paint.fDither;

        fColor = paint.fColor;

        fTextureMatrix = paint.fTextureMatrix;
        fSampler = paint.fSampler;
        fTexture = paint.fTexture;
        GrSafeRef(fTexture);
    }

    ~GrPaint() {
        GrSafeUnref(fTexture);
    }

    // sets paint to src-over, solid white, no texture
    void reset() {
        resetBlend();
        resetOptions();
        resetColor();
        resetTexture();
    }

private:
    GrTexture*      fTexture;

    void resetBlend() {
        fSrcBlendCoeff = GrDrawTarget::kOne_BlendCoeff;
        fDstBlendCoeff = GrDrawTarget::kZero_BlendCoeff;
    }

    void resetOptions() {
        fAntiAlias = false;
        fDither = false;
    }

    void resetColor() {
        fColor = GrColorPackRGBA(0xff, 0xff, 0xff, 0xff);
    }

    void resetTexture() {
        setTexture(NULL);
        fTextureMatrix = GrMatrix::I();
        fSampler.setClampNoFilter();
    }

};

#endif