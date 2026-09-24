// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdio>
#include <string>
#include <array>
#include "dsp.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("voice-compressor", "en-US")
MODULE_EXPORT const char *obs_module_description(void) { return "Voice Compressor: linked soft-knee audio compressor"; }
MODULE_EXPORT const char *obs_module_name(void) { return "Voice Compressor"; }
MODULE_EXPORT const char *obs_module_author(void) { return "Voice Compressor contributors"; }

struct Filter {
    obs_source_t *source=nullptr;
    std::atomic<float> threshold{-18},ratio{3},attack{10},release{120},knee{6},makeup{0},mix{100},highpass{80};
    std::atomic<float> inPeak{0},outPeak{0},grPeak{0};
    voice::Compressor dsp;
    std::mutex windowMutex;
    std::thread windowThread;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> stopping{false};
    std::atomic<bool> meterRunning{false};
};
static void peak_store(std::atomic<float> &value,float x) {
    float old=value.load(std::memory_order_relaxed);
    while(old<x && !value.compare_exchange_weak(old,x,std::memory_order_relaxed)) {}
}
using MeterText = std::array<std::wstring,7>;
static std::wstring localized(const char *key) {
    const char *utf8=obs_module_text(key);
    int count=MultiByteToWideChar(CP_UTF8,0,utf8,-1,nullptr,0);
    if(count<=0) return L"";
    std::wstring value(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,utf8,-1,value.data(),count);
    value.resize(count-1);return value;
}
struct MeterWindow { MeterText text; Filter *filter; float input=-60,output=-60,gr=0; HFONT title,body; };
static void draw_text(HDC dc,HFONT font,int x,int y,const wchar_t *text,COLORREF color) {
    SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    TextOutW(dc,x,y,text,(int)wcslen(text));
}
static LRESULT CALLBACK meter_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto *m=(MeterWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(msg==WM_NCCREATE) {
        m=(MeterWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)m);
    }
    if(!m) return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==WM_TIMER) {
        if(m->filter->stopping.load()) { DestroyWindow(hwnd);return 0; }
        m->input=std::max(m->input-1.8f,voice::db(m->filter->inPeak.exchange(0)));
        m->output=std::max(m->output-1.8f,voice::db(m->filter->outPeak.exchange(0)));
        m->gr=std::max(m->gr-0.6f,m->filter->grPeak.exchange(0));
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT area;GetClientRect(hwnd,&area);
        HDC back=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,area.right,area.bottom);
        auto original=SelectObject(back,bitmap);
        HBRUSH bg=CreateSolidBrush(RGB(17,23,33));FillRect(back,&area,bg);DeleteObject(bg);
        draw_text(back,m->title,28,24,L"VOICE COMPRESSOR",RGB(239,245,251));
        draw_text(back,m->body,28,62,m->text[1].c_str(),RGB(139,158,179));
        const wchar_t *labels[]={m->text[2].c_str(),m->text[3].c_str(),m->text[4].c_str()};
        float values[]={m->input,m->output,m->gr};
        for(int k=0;k<3;k++) {
            int y=111+k*82;
            draw_text(back,m->body,28,y,labels[k],RGB(178,194,212));
            wchar_t number[48];swprintf(number,48,k==2?L"%.1f dB":L"%.1f dBFS",std::max(values[k],k==2?0.0f:-60.0f));
            draw_text(back,m->body,330,y,number,RGB(236,242,249));
            RECT track={28,y+28,448,y+44};HBRUSH b=CreateSolidBrush(RGB(38,48,62));FillRect(back,&track,b);DeleteObject(b);
            float fraction=k==2?values[k]/24:(values[k]+60)/60;
            track.right=28+(int)(420*std::clamp(fraction,0.0f,1.0f));
            COLORREF color=k==2?RGB(245,185,75):RGB(59,211,173);
            if(k<2 && values[k]>=0) color=RGB(255,93,107);
            b=CreateSolidBrush(color);FillRect(back,&track,b);DeleteObject(b);
        }
        draw_text(back,m->body,28,368,m->text[5].c_str(),RGB(139,158,179));
        draw_text(back,m->body,28,394,m->text[6].c_str(),RGB(139,158,179));
        BitBlt(dc,0,0,area.right,area.bottom,back,0,0,SRCCOPY);
        SelectObject(back,original);DeleteObject(bitmap);DeleteDC(back);EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_DESTROY) { KillTimer(hwnd,1);PostQuitMessage(0);return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool open_meter(obs_properties_t*,obs_property_t*,void *data) {
    auto *f=(Filter*)data;if(!f) return false;
    std::lock_guard<std::mutex> guard(f->windowMutex);
    if(f->meterRunning.exchange(true)) {
        if(auto h=f->window.load()) { ShowWindow(h,SW_RESTORE);SetForegroundWindow(h); }
        return false;
    }
    if(f->windowThread.joinable()) f->windowThread.join();
    f->stopping=false;
    f->inPeak=0;f->outPeak=0;f->grPeak=0;
    MeterText text={localized("MeterTitle"),localized("MeterSubtitle"),localized("MeterInput"),
        localized("MeterOutput"),localized("MeterReduction"),localized("MeterHint"),localized("MeterLimit")};
    f->windowThread=std::thread([f,text] {
        HINSTANCE instance=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&meter_proc,&instance);
        WNDCLASSW wc{};wc.lpfnWndProc=meter_proc;wc.hInstance=instance;
        wc.lpszClassName=L"VoiceCompressorMeter_010";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        MeterWindow m{};m.filter=f;m.text=text;
        m.title=CreateFontW(-25,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        m.body=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        RECT r={0,0,478,438};DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        AdjustWindowRect(&r,style,FALSE);
        // Keep the meter visible while OBS's filter controls have focus.
        // Do not steal focus on timer updates; topmost affects only Z order.
        HWND h=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,m.text[0].c_str(),style,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,&m);
        f->window=h;
        if(h) {
            SetTimer(h,1,33,nullptr);ShowWindow(h,SW_SHOW);
            if(f->stopping.load()) PostMessageW(h,WM_CLOSE,0,0);
            MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg);DispatchMessageW(&msg); }
        }
        f->window=nullptr;DeleteObject(m.title);DeleteObject(m.body);f->meterRunning=false;
    });
    return false;
}
static float setting(obs_data_t *s,const char *key,float fallback,float low,float high) {
    double value=obs_data_get_double(s,key);
    return std::isfinite(value)?std::clamp((float)value,low,high):fallback;
}
static void update(void *data,obs_data_t *s) {
    auto *f=(Filter*)data;
    f->threshold=setting(s,"threshold",-18,-60,0);f->ratio=setting(s,"ratio",3,1,20);
    f->attack=setting(s,"attack",10,0.1f,100);f->release=setting(s,"release",120,10,1500);
    f->knee=setting(s,"knee",6,0,24);f->makeup=setting(s,"makeup",0,-24,24);
    f->mix=setting(s,"mix",100,0,100);f->highpass=setting(s,"highpass",80,0,300);
}
static void defaults(obs_data_t *s) {
    obs_data_set_default_double(s,"threshold",-18);obs_data_set_default_double(s,"ratio",3);
    obs_data_set_default_double(s,"attack",10);obs_data_set_default_double(s,"release",120);
    obs_data_set_default_double(s,"knee",6);obs_data_set_default_double(s,"makeup",0);
    obs_data_set_default_double(s,"mix",100);obs_data_set_default_double(s,"highpass",80);
}
static void *create(obs_data_t *s,obs_source_t *source) {
    auto *f=new Filter;f->source=source;update(f,s);return f;
}
static void destroy(void *data) {
    auto *f=(Filter*)data;
    { std::lock_guard<std::mutex> guard(f->windowMutex);
      f->stopping=true;if(auto h=f->window.load()) PostMessageW(h,WM_CLOSE,0,0);
      if(f->windowThread.joinable()) f->windowThread.join(); }
    delete f;
}
static bool preset(obs_properties_t*,obs_property_t *prop,void *data) {
    auto *f=(Filter*)data;if(!f) return false;
    const bool strong=strcmp(obs_property_name(prop),"preset_strong")==0;
    obs_data_t *s=obs_source_get_settings(f->source);
    obs_data_set_double(s,"threshold",strong?-24:-18);obs_data_set_double(s,"ratio",strong?4:2.5);
    obs_data_set_double(s,"attack",strong?5:15);obs_data_set_double(s,"release",strong?150:120);
    obs_data_set_double(s,"knee",strong?8:6);obs_data_set_double(s,"makeup",0);
    obs_data_set_double(s,"mix",100);obs_data_set_double(s,"highpass",80);
    obs_source_update(f->source,s);obs_data_release(s);
    // Return true to let OBS queue RefreshProperties after ButtonClicked returns.
    // Emitting update_properties here synchronously reloads the filter dialog
    // and destroys the WidgetInfo whose ButtonClicked method is still running.
    return true;
}
static bool show_help(obs_properties_t*,obs_property_t*,void*) {
    const char *keys[]={"GuideIntro","GuideStep1","GuideStep2","GuideStep3","GuideStep4","GuideMeters","GuideMore"};
    std::wstring text;
    for(auto key:keys) { if(!text.empty()) text+=L"\r\n\r\n";text+=localized(key); }
    // A modal help dialog runs its own message loop without rebuilding OBS properties.
    MessageBoxW(GetActiveWindow(),text.c_str(),localized("HelpTitle").c_str(),MB_OK|MB_ICONINFORMATION|MB_TOPMOST);
    return false;
}
static obs_properties_t *properties(void *data) {
    auto *p=obs_properties_create();
    obs_properties_add_text(p,"help",obs_module_text("Help"),OBS_TEXT_INFO);
    obs_properties_add_button2(p,"usage_help",obs_module_text("HelpButton"),show_help,data);
    obs_properties_add_button2(p,"preset_natural",obs_module_text("Natural"),preset,data);
    obs_properties_add_button2(p,"preset_strong",obs_module_text("Strong"),preset,data);
    struct Control { const char *id,*label,*suffix; double low,high,step; };
    Control controls[]={
        {"threshold","Threshold"," dB",-60,0,0.5},{"ratio","Ratio",":1",1,20,0.1},
        {"attack","Attack"," ms",0.1,100,0.1},{"release","Release"," ms",10,1500,5},
        {"knee","Knee"," dB",0,24,0.5},{"makeup","Makeup"," dB",-24,24,0.5},
        {"mix","Mix"," %",0,100,1},{"highpass","Highpass"," Hz",0,300,5}};
    for(auto &c:controls) {
        auto *v=obs_properties_add_float_slider(p,c.id,obs_module_text(c.label),c.low,c.high,c.step);
        obs_property_float_set_suffix(v,c.suffix);
        std::string hint=std::string(c.label)+"Hint";
        obs_property_set_long_description(v,obs_module_text(hint.c_str()));
    }
    obs_properties_add_button2(p,"meter",obs_module_text("Meter"),open_meter,data);
    return p;
}
static obs_audio_data *audio(void *data,obs_audio_data *a) {
    auto *f=(Filter*)data;if(!a || !a->frames) return a;
    auto *output=obs_get_audio();if(!output) return a;
    voice::Params p{f->threshold.load(),f->ratio.load(),f->attack.load(),f->release.load(),
        f->knee.load(),f->makeup.load(),f->mix.load(),f->highpass.load()};
    float *planes[8]{};size_t channels=std::min(size_t(8),audio_output_get_channels(output));
    for(size_t c=0;c<channels;c++) planes[c]=(float*)a->data[c];
    auto result=f->dsp.process(planes,channels,a->frames,(float)audio_output_get_sample_rate(output),p);
    peak_store(f->inPeak,result.input);peak_store(f->outPeak,result.output);peak_store(f->grPeak,result.reduction);
    return a;
}
static const char *name(void*) { return obs_module_text("FilterName"); }
bool obs_module_load(void) {
    obs_source_info info{};info.id="voice_compressor_filter";info.type=OBS_SOURCE_TYPE_FILTER;
    info.output_flags=OBS_SOURCE_AUDIO;info.get_name=name;info.create=create;info.destroy=destroy;
    info.get_defaults=defaults;info.get_properties=properties;info.update=update;info.filter_audio=audio;
    obs_register_source(&info);
    blog(LOG_INFO,"[Voice Compressor] Loaded version 0.1.4 (beginner help)");
    return true;
}
void obs_module_unload(void) {
    HINSTANCE instance=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&meter_proc,&instance);
    UnregisterClassW(L"VoiceCompressorMeter_010",instance);
}
