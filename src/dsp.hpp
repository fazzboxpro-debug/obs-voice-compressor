// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace voice {
struct Params {
    float threshold=-18, ratio=3, attack=10, release=120, knee=6;
    float makeup=0, mix=100, highpass=80;
};
struct Meter { float input=0, output=0, reduction=0; };
inline float db(float x) { return 20.0f*std::log10(std::max(x,1.0e-8f)); }
inline float gain(float d) { return std::pow(10.0f,d/20.0f); }
inline float reduction(float level, const Params &p) {
    const float x=level-p.threshold, slope=1.0f-1.0f/p.ratio;
    if(p.knee>0 && x>-p.knee/2 && x<p.knee/2)
        return slope*(x+p.knee/2)*(x+p.knee/2)/(2*p.knee);
    return x>0 ? slope*x : 0;
}
class Compressor {
    float gr=0, outputGain=1, wet=1, previousInput[8]{}, previousOutput[8]{};
public:
    void reset() { gr=0; outputGain=1; wet=1;
        for(int c=0;c<8;c++) previousInput[c]=previousOutput[c]=0; }
    Meter process(float **data, size_t channels, size_t frames, float rate, Params p) {
        Meter meter;
        if(rate<8000 || !std::isfinite(rate)) return meter;
        channels=std::min(channels,size_t(8));
        p.ratio=std::clamp(p.ratio,1.0f,20.0f);
        p.knee=std::clamp(p.knee,0.0f,24.0f);
        const float a=std::exp(-1/(rate*std::max(p.attack,0.1f)*0.001f));
        const float r=std::exp(-1/(rate*std::max(p.release,10.0f)*0.001f));
        const float smooth=std::exp(-1/(rate*0.010f));
        const float hp=std::exp(-6.28318530718f*std::clamp(p.highpass,0.0f,300.0f)/rate);
        const float targetGain=gain(std::clamp(p.makeup,-24.0f,24.0f));
        const float targetWet=std::clamp(p.mix*0.01f,0.0f,1.0f);
        for(size_t i=0;i<frames;i++) {
            float peak=0;
            for(size_t c=0;c<channels;c++) if(data[c]) {
                float x=data[c][i];
                if(!std::isfinite(x)) x=data[c][i]=0;
                meter.input=std::max(meter.input,std::fabs(x));
                float filtered=hp*(previousOutput[c]+x-previousInput[c]);
                previousInput[c]=x;
                previousOutput[c]=std::fabs(filtered)<1e-20f?0:filtered;
                peak=std::max(peak,std::fabs(p.highpass>0?filtered:x));
            }
            float target=reduction(db(peak),p);
            float coefficient=target>gr?a:r;
            gr=coefficient*gr+(1-coefficient)*target;
            if(gr<1e-8f) gr=0;
            outputGain=smooth*outputGain+(1-smooth)*targetGain;
            wet=smooth*wet+(1-smooth)*targetWet;
            float multiplier=(1-wet)+wet*gain(-gr)*outputGain;
            for(size_t c=0;c<channels;c++) if(data[c]) {
                data[c][i]*=multiplier;
                meter.output=std::max(meter.output,std::fabs(data[c][i]));
            }
            meter.reduction=std::max(meter.reduction,gr);
        }
        return meter;
    }
};
}
