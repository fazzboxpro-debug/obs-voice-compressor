// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/dsp.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
static int checks=0;
void check(bool ok,const char *label) { ++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);} }
int main() {
    voice::Params p;p.highpass=0;p.knee=0;p.threshold=-20;p.ratio=4;
    check(std::fabs(voice::reduction(-4,p)-12)<1e-5,"4:1 static curve");
    check(voice::reduction(-30,p)==0,"below threshold");
    p.knee=8;check(std::fabs(voice::reduction(-24,p))<1e-5,"knee lower edge");
    check(std::fabs(voice::reduction(-16,p)-3)<1e-5,"knee upper edge");
    for(float rate:{44100.f,48000.f,96000.f}) {
        voice::Compressor c;p.knee=0;
        std::vector<float> l((int)rate,voice::gain(-4)),r((int)rate,voice::gain(-10));
        float *planes[]={l.data(),r.data()};
        auto m=c.process(planes,2,l.size(),rate,p);
        check(std::fabs(voice::db(l.back())+16)<.03,"steady-state compression");
        check(std::fabs(l.back()/r.back()-voice::gain(6))<.001,"stereo link preserves balance");
        check(m.reduction>11.9 && m.reduction<12.1,"meter reduction");
        check(l.front()>l.back(),"attack retains transient");
        std::fill(l.begin(),l.end(),0);std::fill(r.begin(),r.end(),0);
        c.process(planes,2,l.size(),rate,p);
        std::fill(l.begin(),l.end(),.01f);
        c.process(planes,2,l.size(),rate,p);
        check(std::fabs(l.back()-.01f)<1e-5,"release recovers");
        c.reset();p.ratio=1;
        std::fill(l.begin(),l.end(),.2f);c.process(planes,2,l.size(),rate,p);
        check(std::fabs(l.back()-.2f)<1e-6,"1:1 unity");
        p.ratio=4;p.mix=0;
        std::fill(l.begin(),l.end(),.5f);c.process(planes,2,l.size(),rate,p);
        check(std::fabs(l.back()-.5f)<.001,"dry mix unity after smoothing");p.mix=100;
    }
    voice::Compressor c;float bad[]={std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),0};
    float *planes[]={bad,nullptr};c.process(planes,2,3,48000,p);
    check(std::isfinite(bad[0])&&std::isfinite(bad[1]),"invalid audio sanitized");
    check(bad[2]==0,"silence remains zero");
    // Identical output regardless of block boundaries.
    std::vector<float> whole(48000),split(48000);
    for(size_t i=0;i<whole.size();i++) whole[i]=split[i]=.6f*std::sin(float(i)*.13f);
    voice::Compressor a,b;float *one[]={whole.data()};a.process(one,1,whole.size(),48000,p);
    for(size_t i=0;i<split.size();i+=137) {float *part[]={split.data()+i};b.process(part,1,std::min(size_t(137),split.size()-i),48000,p);}
    check(whole==split,"block-size invariance");
    std::printf("PASS: %d DSP checks\n",checks);
}
