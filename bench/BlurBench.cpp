#include "SkBenchmark.h"
#include "SkCanvas.h"
#include "SkPaint.h"
#include "SkRandom.h"
#include "SkShader.h"
#include "SkString.h"
#include "SkBlurMaskFilter.h"

#define SMALL   SkIntToScalar(2)
#define BIG     SkIntToScalar(10)

static const char* gStyleName[] = {
    "normal",
    "solid",
    "outer",
    "inner"
};

class BlurBench : public SkBenchmark {
    SkScalar    fRadius;
    SkBlurMaskFilter::BlurStyle fStyle;
    SkString    fName;

public:
    BlurBench(void* param, SkScalar rad, SkBlurMaskFilter::BlurStyle bs) : INHERITED(param) {
        fRadius = rad;
        fStyle = bs;
        const char* name = rad > 0 ? gStyleName[bs] : "none";
        fName.printf("blur_%d_%s", SkScalarRound(rad), name);
    }
    
protected:
    virtual const char* onGetName() {
        return fName.c_str();
    }
    
    virtual void onDraw(SkCanvas* canvas) {
        SkPaint paint;
        this->setupPaint(&paint);

        paint.setAntiAlias(true);

        SkRandom rand;
        for (int i = 0; i < 10; i++) {
            SkRect r = SkRect::MakeWH(rand.nextUScalar1() * 400,
                                      rand.nextUScalar1() * 400);
            r.offset(fRadius, fRadius);

            if (fRadius > 0) {
                SkMaskFilter* mf = SkBlurMaskFilter::Create(fRadius, fStyle, 0);
                paint.setMaskFilter(mf)->unref();
            }
            canvas->drawOval(r, paint);
        }
    }
    
private:
    typedef SkBenchmark INHERITED;
};

static SkBenchmark* Fact00(void* p) { return new BlurBench(p, SMALL, SkBlurMaskFilter::kNormal_BlurStyle); }
static SkBenchmark* Fact01(void* p) { return new BlurBench(p, SMALL, SkBlurMaskFilter::kSolid_BlurStyle); }
static SkBenchmark* Fact02(void* p) { return new BlurBench(p, SMALL, SkBlurMaskFilter::kOuter_BlurStyle); }
static SkBenchmark* Fact03(void* p) { return new BlurBench(p, SMALL, SkBlurMaskFilter::kInner_BlurStyle); }

static SkBenchmark* Fact10(void* p) { return new BlurBench(p, BIG, SkBlurMaskFilter::kNormal_BlurStyle); }
static SkBenchmark* Fact11(void* p) { return new BlurBench(p, BIG, SkBlurMaskFilter::kSolid_BlurStyle); }
static SkBenchmark* Fact12(void* p) { return new BlurBench(p, BIG, SkBlurMaskFilter::kOuter_BlurStyle); }
static SkBenchmark* Fact13(void* p) { return new BlurBench(p, BIG, SkBlurMaskFilter::kInner_BlurStyle); }

static SkBenchmark* FactNone(void* p) { return new BlurBench(p, 0, SkBlurMaskFilter::kNormal_BlurStyle); }

static BenchRegistry gReg00(Fact00);
static BenchRegistry gReg01(Fact01);
static BenchRegistry gReg02(Fact02);
static BenchRegistry gReg03(Fact03);

static BenchRegistry gReg10(Fact10);
static BenchRegistry gReg11(Fact11);
static BenchRegistry gReg12(Fact12);
static BenchRegistry gReg13(Fact13);

static BenchRegistry gRegNone(FactNone);
