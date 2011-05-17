#include "gm.h"

namespace skiagm {

class NoColorBleedGM : public GM {
public:
	NoColorBleedGM() {}

protected:
    virtual SkString onShortName() {
        return SkString("nocolorbleed");
    }

	virtual SkISize onISize() {
        return make_isize(200, 200);
    }

    void drawBG(SkCanvas* canvas) {
        canvas->drawColor(0xFFDDDDDD);
    }

    virtual void onDraw(SkCanvas* canvas) {
        drawBG(canvas);

        SkBitmap sprite;
        sprite.setConfig(SkBitmap::kARGB_8888_Config, 4, 4, 4*sizeof(SkColor));
        SkColor spriteData[16] = {
            SK_ColorBLACK,  SK_ColorCYAN,    SK_ColorMAGENTA, SK_ColorYELLOW,
            SK_ColorBLACK,  SK_ColorWHITE,   SK_ColorBLACK,   SK_ColorRED,
            SK_ColorGREEN,  SK_ColorBLACK,   SK_ColorWHITE,   SK_ColorBLUE,
            SK_ColorYELLOW, SK_ColorMAGENTA, SK_ColorCYAN,    SK_ColorBLACK
        };
        sprite.setPixels(spriteData);

        // We draw a magnified subrect of the sprite
        // sample interpolation may cause color bleeding around edges
        // the subrect is a pure white area
        SkIRect srcRect;
        SkRect dstRect;
        SkPaint paint;
        paint.setFilterBitmap(true);
        //First row : full texture with and without filtering
        srcRect.setXYWH(0, 0, 4, 4);
        dstRect.setXYWH(0.0f, 0.0f, 100.0f, 100.0f);
        canvas->drawBitmapRect(sprite, &srcRect, dstRect, &paint);
        dstRect.setXYWH(100.0f, 0.0f, 100.0f, 100.0f);
        canvas->drawBitmapRect(sprite, &srcRect, dstRect);
        //Second row : sub rect of texture with and without filtering
        srcRect.setXYWH(1, 1, 2, 2);
        dstRect.setXYWH(25.0f, 125.0f, 50.0f, 50.0f);
        canvas->drawBitmapRect(sprite, &srcRect, dstRect, &paint);
        dstRect.setXYWH(125.0f, 125.0f, 50.0f, 50.0f);
        canvas->drawBitmapRect(sprite, &srcRect, dstRect);
    }

private:
    typedef GM INHERITED;
};

//////////////////////////////////////////////////////////////////////////////

static GM* MyFactory(void*) { return new NoColorBleedGM; }
static GMRegistry reg(MyFactory);

}