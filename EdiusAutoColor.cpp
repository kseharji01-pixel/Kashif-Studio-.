#include <algorithm>
#include <cmath>
#include <cstring>

#include "ofxCore.h"
#include "ofxImageEffect.h"

#define PLUGIN_NAME "Auto Color Correction"
#define PLUGIN_GROUP "EDIUS Custom Plugins"
#define PLUGIN_ID "com.custom.edius.autocolor"
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 0

static OfxHost* gHost = nullptr;
static OfxImageEffectSuiteV1* gEffectSuite = nullptr;
static OfxPropertySuiteV1* gPropertySuite = nullptr;

static inline float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
static inline float tone(float v) {
    // Mild contrast curve around middle gray.
    const float contrast = 1.08f;
    return clamp01((v - 0.5f) * contrast + 0.5f);
}

template <typename T>
static void processRGBA(T* dst, const T* src, int width, int height, int srcRowBytes, int dstRowBytes, float scale) {
    const float maxV = static_cast<float>(std::numeric_limits<T>::max());
    // First pass: estimate RGB gray-world gains.
    double rSum = 0, gSum = 0, bSum = 0;
    const int channels = 4;
    for (int y = 0; y < height; ++y) {
        const T* row = reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(src) + y * srcRowBytes);
        for (int x = 0; x < width; ++x) {
            const T* p = row + x * channels;
            rSum += static_cast<double>(p[0]);
            gSum += static_cast<double>(p[1]);
            bSum += static_cast<double>(p[2]);
        }
    }
    const double n = static_cast<double>(std::max(1, width * height));
    const double rAvg = rSum / n, gAvg = gSum / n, bAvg = bSum / n;
    const double gray = (rAvg + gAvg + bAvg) / 3.0;
    const float rg = static_cast<float>(gray / std::max(rAvg, 1.0));
    const float gg = static_cast<float>(gray / std::max(gAvg, 1.0));
    const float bg = static_cast<float>(gray / std::max(bAvg, 1.0));

    for (int y = 0; y < height; ++y) {
        const T* srow = reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(src) + y * srcRowBytes);
        T* drow = reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(dst) + y * dstRowBytes);
        for (int x = 0; x < width; ++x) {
            const T* s = srow + x * channels;
            T* d = drow + x * channels;
            float r = clamp01(static_cast<float>(s[0]) / maxV * rg);
            float g = clamp01(static_cast<float>(s[1]) / maxV * gg);
            float b = clamp01(static_cast<float>(s[2]) / maxV * bg);
            r = clamp01((r - 0.5f) * (1.08f * scale) + 0.5f);
            g = clamp01((g - 0.5f) * (1.08f * scale) + 0.5f);
            b = clamp01((b - 0.5f) * (1.08f * scale) + 0.5f);
            d[0] = static_cast<T>(std::lround(r * maxV));
            d[1] = static_cast<T>(std::lround(g * maxV));
            d[2] = static_cast<T>(std::lround(b * maxV));
            d[3] = s[3];
        }
    }
}

static void processRGBAFloat(float* dst, const float* src, int width, int height, int srcRowBytes, int dstRowBytes) {
    const int channels = 4;
    double rSum=0,gSum=0,bSum=0;
    for(int y=0;y<height;++y){
        const float* row=reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(src)+y*srcRowBytes);
        for(int x=0;x<width;++x){ const float* p=row+x*channels; rSum+=p[0]; gSum+=p[1]; bSum+=p[2]; }
    }
    const double n=static_cast<double>(std::max(1,width*height));
    const double gray=(rSum+gSum+bSum)/(3.0*n);
    const float rg=static_cast<float>(gray/std::max(rSum/n,0.000001));
    const float gg=static_cast<float>(gray/std::max(gSum/n,0.000001));
    const float bg=static_cast<float>(gray/std::max(bSum/n,0.000001));
    for(int y=0;y<height;++y){
        const float* srow=reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(src)+y*srcRowBytes);
        float* drow=reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(dst)+y*dstRowBytes);
        for(int x=0;x<width;++x){
            const float* s=srow+x*channels; float* d=drow+x*channels;
            d[0]=clamp01((s[0]*rg-0.5f)*1.08f+0.5f);
            d[1]=clamp01((s[1]*gg-0.5f)*1.08f+0.5f);
            d[2]=clamp01((s[2]*bg-0.5f)*1.08f+0.5f);
            d[3]=s[3];
        }
    }
}

static OfxStatus Render(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs) {
    if (!gEffectSuite || !gPropertySuite) return kOfxStatErrFatal;
    OfxPropertySetHandle srcClip=nullptr, dstClip=nullptr;
    if (gEffectSuite->clipGetHandle(instance, "Source", &srcClip, nullptr) != kOfxStatOK) return kOfxStatFailed;
    if (gEffectSuite->clipGetHandle(instance, "Output", &dstClip, nullptr) != kOfxStatOK) return kOfxStatFailed;
    double time=0;
    gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxPropertySetHandle srcImg=nullptr, dstImg=nullptr;
    if (gEffectSuite->clipGetImage(srcClip, time, nullptr, &srcImg) != kOfxStatOK || !srcImg) return kOfxStatFailed;
    if (gEffectSuite->clipGetImage(dstClip, time, nullptr, &dstImg) != kOfxStatOK || !dstImg) { gEffectSuite->clipReleaseImage(srcClip, srcImg); return kOfxStatFailed; }

    void *srcData=nullptr,*dstData=nullptr; int srcRow=0,dstRow=0; int srcX1=0,srcY1=0,srcX2=0,srcY2=0;
    char depth[64]={0}, comps[64]={0};
    gPropertySuite->propGetPointer(srcImg,kOfxImagePropData,0,&srcData);
    gPropertySuite->propGetPointer(dstImg,kOfxImagePropData,0,&dstData);
    gPropertySuite->propGetInt(srcImg,kOfxImagePropRowBytes,0,&srcRow);
    gPropertySuite->propGetInt(dstImg,kOfxImagePropRowBytes,0,&dstRow);
    gPropertySuite->propGetString(srcImg,kOfxImageEffectPropPixelDepth,0,depth);
    gPropertySuite->propGetString(srcImg,kOfxImageEffectPropComponents,0,comps);
    OfxRectI bounds{}; gPropertySuite->propGetInt(srcImg,kOfxImagePropBounds,0,&srcX1); // fallback below
    gPropertySuite->propGetInt(srcImg,kOfxImagePropBounds,1,&srcY1);
    gPropertySuite->propGetInt(srcImg,kOfxImagePropBounds,2,&srcX2);
    gPropertySuite->propGetInt(srcImg,kOfxImagePropBounds,3,&srcY2);
    const int width=std::max(0,srcX2-srcX1), height=std::max(0,srcY2-srcY1);
    if(!srcData||!dstData||width<=0||height<=0||std::strcmp(comps,kOfxImageComponentRGBA)!=0){
        gEffectSuite->clipReleaseImage(dstClip,dstImg); gEffectSuite->clipReleaseImage(srcClip,srcImg); return kOfxStatOK;
    }
    if(std::strcmp(depth,kOfxBitDepthFloat)==0) processRGBAFloat(static_cast<float*>(dstData),static_cast<const float*>(srcData),width,height,srcRow,dstRow);
    else if(std::strcmp(depth,kOfxBitDepthUByte)==0) processRGBA<unsigned char>(static_cast<unsigned char*>(dstData),static_cast<const unsigned char*>(srcData),width,height,srcRow,dstRow,1.0f);
    else if(std::strcmp(depth,kOfxBitDepthUShort)==0) processRGBA<unsigned short>(static_cast<unsigned short*>(dstData),static_cast<const unsigned short*>(srcData),width,height,srcRow,dstRow,1.0f);
    gEffectSuite->clipReleaseImage(dstClip,dstImg); gEffectSuite->clipReleaseImage(srcClip,srcImg);
    return kOfxStatOK;
}

static OfxStatus Describe(OfxImageEffectHandle handle) {
    OfxPropertySetHandle props=nullptr;
    if(gEffectSuite->getPropertySet(handle,&props)!=kOfxStatOK) return kOfxStatFailed;
    gPropertySuite->propSetString(props,kOfxPropLabel,0,PLUGIN_NAME);
    gPropertySuite->propSetString(props,kOfxImageEffectPluginPropGrouping,0,PLUGIN_GROUP);
    gPropertySuite->propSetString(props,kOfxImageEffectPluginPropSingleInstance,0,"0");
    gPropertySuite->propSetString(props,kOfxImageEffectPropSupportedContexts,0,kOfxImageEffectContextFilter);
    return kOfxStatOK;
}

static OfxStatus DescribeInContext(OfxImageEffectHandle handle) {
    OfxPropertySetHandle props=nullptr; gEffectSuite->getPropertySet(handle,&props);
    OfxImageClipHandle source=nullptr, output=nullptr;
    if(gEffectSuite->clipDefine(handle,"Source",&source)!=kOfxStatOK) return kOfxStatFailed;
    OfxPropertySetHandle sp=nullptr; gEffectSuite->clipGetPropertySet(source,&sp);
    gPropertySuite->propSetString(sp,kOfxPropLabel,0,"Source");
    gPropertySuite->propSetString(sp,kOfxImageEffectPropComponents,0,kOfxImageComponentRGBA);
    if(gEffectSuite->clipDefine(handle,"Output",&output)!=kOfxStatOK) return kOfxStatFailed;
    OfxPropertySetHandle op=nullptr; gEffectSuite->clipGetPropertySet(output,&op);
    gPropertySuite->propSetString(op,kOfxPropLabel,0,"Output");
    gPropertySuite->propSetString(op,kOfxImageEffectPropComponents,0,kOfxImageComponentRGBA);
    return kOfxStatOK;
}

static OfxStatus PluginMain(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    if(!action) return kOfxStatErrBadHandle;
    if(std::strcmp(action,kOfxActionLoad)==0) return kOfxStatOK;
    if(std::strcmp(action,kOfxImageEffectActionDescribe)==0) return Describe((OfxImageEffectHandle)handle);
    if(std::strcmp(action,kOfxImageEffectActionDescribeInContext)==0) return DescribeInContext((OfxImageEffectHandle)handle);
    if(std::strcmp(action,kOfxImageEffectActionRender)==0) return Render((OfxImageEffectHandle)handle,inArgs);
    return kOfxStatReplyDefault;
}

static void SetHost(OfxHost* host) {
    gHost=host;
    if(!host || !host->fetchSuite) return;
    gPropertySuite=(OfxPropertySuiteV1*)host->fetchSuite(host->host,&kOfxPropertySuite,kOfxPropertySuiteVersion);
    gEffectSuite=(OfxImageEffectSuiteV1*)host->fetchSuite(host->host,&kOfxImageEffectSuite,kOfxImageEffectSuiteVersion);
}

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    PLUGIN_ID,
    PLUGIN_VERSION_MAJOR,
    PLUGIN_VERSION_MINOR,
    SetHost,
    PluginMain
};

#if defined(_WIN32)
extern "C" __declspec(dllexport) OfxStatus OfxPluginMain(int nth, const OfxHost* host, OfxPlugin** plugin) {
#else
extern "C" OfxStatus OfxPluginMain(int nth, const OfxHost* host, OfxPlugin** plugin) {
#endif
    if(!plugin) return kOfxStatErrBadHandle;
    if(nth==0){ *plugin=&gPlugin; return kOfxStatOK; }
    *plugin=nullptr; return kOfxStatReplyDefault;
}
