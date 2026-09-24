// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <thread>
#include <util/platform.h>
static float captured=0;
static int blocks=0;
struct RefreshWatch { bool inClick=false; int unsafeReloads=0; };
static void properties_changed(void *context,calldata_t*) {
    auto *watch=(RefreshWatch*)context;
    // OBS's filter dialog reloads properties synchronously on this signal.
    // During a button callback that destroys the active WidgetInfo object.
    if(watch->inClick) ++watch->unsafeReloads;
}
static void capture(void*,obs_source_t*,const audio_data *a,bool) {
    if(a->frames && a->data[0]) {captured=((float*)a->data[0])[a->frames-1];++blocks;}
}
static void check(bool ok,const char *label) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}}
static void capture_help(HWND window) {
    RECT rc;GetWindowRect(window,&rc);int width=rc.right-rc.left,height=rc.bottom-rc.top;
    HDC screen=GetDC(window),mem=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);auto old=SelectObject(mem,bitmap);
    check(PrintWindow(window,mem,0)!=0,"help renders");SelectObject(mem,old);
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;
    bi.bmiHeader.biHeight=height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    std::vector<unsigned char> pixels(width*height*4);
    check(GetDIBits(mem,bitmap,0,height,pixels.data(),&bi,DIB_RGB_COLORS)!=0,"help pixels");
    BITMAPFILEHEADER bf{};bf.bfType=0x4d42;bf.bfOffBits=sizeof(bf)+sizeof(BITMAPINFOHEADER);bf.bfSize=bf.bfOffBits+(DWORD)pixels.size();
    FILE *file=std::fopen("help-test.bmp","wb");check(file,"help screenshot file");
    std::fwrite(&bf,sizeof(bf),1,file);std::fwrite(&bi.bmiHeader,sizeof(BITMAPINFOHEADER),1,file);
    std::fwrite(pixels.data(),pixels.size(),1,file);std::fclose(file);
    DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(window,screen);
}
int main(int argc,char **argv) {
    check(argc==3,"arguments");
    const char *locale=std::getenv("VOICE_TEST_LOCALE");if(!locale) locale="ja-JP";
    check(obs_startup(locale,nullptr,nullptr),"OBS startup");
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    check(obs_reset_audio(&ai),"audio startup");
    obs_module_t *module=nullptr;
    check(obs_open_module(&module,argv[1],argv[2])==MODULE_SUCCESS,"DLL load");
    check(obs_init_module(module),"module init");
    const char *translated=nullptr;
    check(obs_module_get_locale_string(module,"MeterInput",&translated),"meter translation loaded");
    const char *expected=strcmp(locale,"ja-JP")==0?"入力":strcmp(locale,"zh-CN")==0?"输入":strcmp(locale,"zh-TW")==0?"輸入":"INPUT";
    check(strcmp(translated,expected)==0,"correct locale selected");
    auto *uiSource=obs_source_create_private("voice_compressor_filter","meter test",nullptr);
    auto *uiProps=obs_source_properties(uiSource);
    for(auto *key:{"GuideIntro","GuideStep1","GuideStep2","GuideStep3","GuideStep4","GuideMeters","GuideMore"}) {
        const char *value=nullptr;
        check(obs_module_get_locale_string(module,key,&value)&&value&&*value,"help translation available");
    }
    check(obs_property_long_description(obs_properties_get(uiProps,"threshold"))!=nullptr,"parameter tooltip available");
    const char *helpTitle=nullptr;check(obs_module_get_locale_string(module,"HelpTitle",&helpTitle),"help title");
    wchar_t wideTitle[256];MultiByteToWideChar(CP_UTF8,0,helpTitle,-1,wideTitle,256);
    DWORD uiThread=GetCurrentThreadId();
    std::thread closer([&] {
        HWND dialog=nullptr;
        for(int i=0;i<300&&!dialog;i++) {
            Sleep(10);
            EnumThreadWindows(uiThread,[](HWND h,LPARAM p)->BOOL {
                wchar_t cls[64];GetClassNameW(h,cls,64);
                if(wcscmp(cls,L"#32770")==0) {*(HWND*)p=h;return FALSE;}return TRUE;
            },(LPARAM)&dialog);
        }
        check(dialog!=nullptr,"help opens");Sleep(150);
        std::puts("Help found; capturing");std::fflush(stdout);capture_help(dialog);
        std::puts("Help captured; closing");std::fflush(stdout);
        check(PostMessageW(dialog,WM_CLOSE,0,0)!=0,"post help close");
        std::printf("close posted to %p, owner thread %lu, expected %lu\n",(void*)dialog,GetWindowThreadProcessId(dialog,nullptr),uiThread);std::fflush(stdout);
    });
    check(!obs_property_button_clicked(obs_properties_get(uiProps,"usage_help"),uiSource),"help does not rebuild settings");
    std::puts("help callback returned");std::fflush(stdout);closer.join();std::puts("PASS: localized help opens, renders, and closes");
    RefreshWatch watch;
    signal_handler_connect(obs_source_get_signal_handler(uiSource),"update_properties",properties_changed,&watch);
    for(int i=0;i<200;i++) {
        const bool strong=(i%2)==0;
        watch.inClick=true;
        bool refresh=obs_property_button_clicked(obs_properties_get(uiProps,strong?"preset_strong":"preset_natural"),uiSource);
        watch.inClick=false;
        check(watch.unsafeReloads==0,"preset must not synchronously destroy its active UI callback");
        check(refresh,"preset requests deferred UI refresh");
        auto *s=obs_source_get_settings(uiSource);
        check(obs_data_get_double(s,"ratio")== (strong?4:2.5),"alternating preset ratio");
        obs_data_set_double(s,"threshold",-27);obs_source_update(uiSource,s);
        check(obs_data_get_double(s,"threshold")==-27,"editing remains possible after preset");
        obs_data_release(s);
        // The UI can safely rebuild after the callback returns.
        obs_properties_destroy(uiProps);uiProps=obs_source_properties(uiSource);
    }
    signal_handler_disconnect(obs_source_get_signal_handler(uiSource),"update_properties",properties_changed,&watch);
    std::puts("PASS: 200 preset switches without synchronous UI reload; subsequent editing works");
    obs_property_button_clicked(obs_properties_get(uiProps,"preset_strong"),uiSource);
    auto *uiSettings=obs_source_get_settings(uiSource);
    check(obs_data_get_double(uiSettings,"threshold")==-24,"preset applies");obs_data_release(uiSettings);
    obs_property_button_clicked(obs_properties_get(uiProps,"meter"),uiSource);
    // Immediate second click must not deadlock while the window is starting.
    obs_property_button_clicked(obs_properties_get(uiProps,"meter"),uiSource);
    HWND meter=nullptr;
    for(int attempt=0;attempt<100&&!meter;attempt++) {Sleep(10);meter=FindWindowW(L"VoiceCompressorMeter_010",nullptr);}
    check(meter!=nullptr,"meter opens");Sleep(100);
    check((GetWindowLongPtrW(meter,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,"meter stays above normal windows");
    RECT meterRect;GetWindowRect(meter,&meterRect);
    HWND controls=CreateWindowExW(0,L"STATIC",L"Filter controls focus test",WS_OVERLAPPEDWINDOW,
        meterRect.left,meterRect.top,meterRect.right-meterRect.left,meterRect.bottom-meterRect.top,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(controls!=nullptr,"test controls window");
    ShowWindow(controls,SW_SHOW);SetForegroundWindow(controls);
    SetWindowPos(controls,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    POINT meterPoint{40,120};ClientToScreen(meter,&meterPoint);
    check(WindowFromPoint(meterPoint)==meter,"meter remains visible when another normal window is raised");
    DestroyWindow(controls);
    std::puts("PASS: meter remains visible above overlapping control window");
    RECT rc;GetClientRect(meter,&rc);HDC screen=GetDC(meter),mem=CreateCompatibleDC(screen);
    HBITMAP image=CreateCompatibleBitmap(screen,rc.right,rc.bottom);auto old=SelectObject(mem,image);
    check(PrintWindow(meter,mem,PW_CLIENTONLY)!=0,"meter renders");
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=rc.right;
    bi.bmiHeader.biHeight=rc.bottom;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(rc.right*rc.bottom*4);SelectObject(mem,old);
    check(GetDIBits(mem,image,0,rc.bottom,pixels.data(),&bi,DIB_RGB_COLORS)!=0,"meter pixels");
    BITMAPFILEHEADER bf{};bf.bfType=0x4d42;bf.bfOffBits=sizeof(bf)+sizeof(BITMAPINFOHEADER);bf.bfSize=bf.bfOffBits+(DWORD)pixels.size();
    FILE *picture=std::fopen("meter-test.bmp","wb");check(picture,"screenshot file");
    std::fwrite(&bf,sizeof(bf),1,picture);std::fwrite(&bi.bmiHeader,sizeof(BITMAPINFOHEADER),1,picture);
    std::fwrite(pixels.data(),pixels.size(),1,picture);std::fclose(picture);
    DeleteObject(image);DeleteDC(mem);ReleaseDC(meter,screen);
    obs_properties_destroy(uiProps);obs_source_release(uiSource);
    for(int attempt=0;attempt<100&&IsWindow(meter);attempt++) Sleep(10);
    check(!IsWindow(meter),"source destruction closes meter");
    obs_source_info input{};input.id="voice_test_input";input.type=OBS_SOURCE_TYPE_INPUT;input.output_flags=OBS_SOURCE_AUDIO;
    input.get_name=[](void*){return "Test signal";};input.create=[](obs_data_t*,obs_source_t *s)->void*{return s;};
    input.destroy=[](void*){};obs_register_source(&input);
    auto *parent=obs_source_create_private("voice_test_input","signal",nullptr);
    auto *cfg=obs_data_create();obs_data_set_double(cfg,"threshold",-20);obs_data_set_double(cfg,"ratio",4);
    obs_data_set_double(cfg,"highpass",0);obs_data_set_double(cfg,"knee",0);
    auto *filter=obs_source_create_private("voice_compressor_filter","compression",cfg);obs_data_release(cfg);
    check(parent&&filter,"signal and filter create");obs_source_filter_add(parent,filter);
    obs_source_add_audio_capture_callback(parent,capture,nullptr);
    std::vector<float> l(480,.5f),r(480,.25f);
    obs_source_audio signal{};signal.data[0]=(uint8_t*)l.data();signal.data[1]=(uint8_t*)r.data();
    signal.frames=480;signal.speakers=SPEAKERS_STEREO;signal.format=AUDIO_FORMAT_FLOAT_PLANAR;signal.samples_per_sec=48000;
    const uint64_t start=os_gettime_ns();
    for(int i=0;i<100;i++) {signal.timestamp=start+uint64_t(i)*10000000;obs_source_output_audio(parent,&signal);}
    check(blocks>0,"audio callback invoked");
    check(std::fabs(20*std::log10(std::fabs(captured))-(-20+(20*std::log10(.5f)+20)/4))<.05,"actual OBS audio is compressed");
    obs_source_set_enabled(filter,false);signal.timestamp+=10000000;obs_source_output_audio(parent,&signal);
    check(std::fabs(captured-.5f)<.0001,"OBS bypass passes original signal");
    obs_source_remove_audio_capture_callback(parent,capture,nullptr);
    obs_source_filter_remove(parent,filter);obs_source_release(filter);obs_source_release(parent);
    for(int i=0;i<50;i++) {
        auto *s=obs_source_create_private("voice_compressor_filter","test",nullptr);check(s,"create filter");
        auto *settings=obs_source_get_settings(s);check(obs_data_get_double(settings,"threshold")==-18,"defaults");
        obs_data_set_double(settings,"threshold",-24);obs_source_update(s,settings);
        auto *props=obs_source_properties(s);check(props,"properties");
        check(obs_properties_get(props,"meter"),"meter button");
        check(obs_properties_get(props,"highpass"),"detector highpass");
        obs_properties_destroy(props);obs_data_release(settings);obs_source_release(s);
    }
    obs_shutdown();std::puts("PASS: OBS 32.2.2 DLL load, audio compression, bypass, properties, 50 lifecycle iterations");
}
