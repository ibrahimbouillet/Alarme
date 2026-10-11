// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <iomanip>
#include <vector>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "alarm_audio_policy.h"
#include "elapsed_cycle.h"
#include "glass.h"
#include "ui_ids.h"
#include "tray.h"
#include "notifications.h"
#include "widget_state.h"

namespace {
using namespace alarmapp;
using namespace alarmapp::ui;
constexpr UINT_PTR countdown_timer=1, integration_timeout=2, audio_recovery_timer=3, lock_prelude_timer=4, lock_stop_timer=5, lock_timeout_timer=6, animation_timer=7, preview_timer=8, audio_status_timer=9;

std::wstring local_time(TimePoint at) {
    const auto raw=Clock::to_time_t(at); std::tm tm{};
    localtime_s(&tm,&raw);
    std::wostringstream out; out << std::put_time(&tm,L"%d/%m/%Y %H:%M:%S"); return out.str();
}
std::wstring countdown(TimePoint due) {
    auto seconds=std::max<long long>(0,std::chrono::duration_cast<std::chrono::seconds>(due-Clock::now()).count());
    std::wostringstream out; out << std::setfill(L'0') << std::setw(2) << seconds/3600 << L':' << std::setw(2) << (seconds/60)%60 << L':' << std::setw(2) << seconds%60;
    return out.str();
}

class App {
public:
    // A cold audio driver may need more than one second to initialize. Keep
    // each accelerated ringing interval long enough to verify playback; the
    // production five-minute/ten-minute cycle remains unchanged.
    App(std::filesystem::path directory,bool integration,bool wake_test,bool background,UINT open_message) : integration_(integration), wake_test_(wake_test), background_(background), open_message_(open_message), directory_(std::move(directory)),
        cycle_(integration ? std::chrono::seconds(3) : std::chrono::minutes(5),
                                        integration ? std::chrono::seconds(2) : std::chrono::minutes(10)) {}
    ~App() { shutdown(); if(font_) DeleteObject(font_); if(input_font_) DeleteObject(input_font_); }
    void observe_keyboard(const MSG& input) {
        if(input.message!=WM_KEYDOWN&&input.message!=WM_SYSKEYDOWN)return;
        keyboard_focus_=true;
        if(input.wParam==VK_ESCAPE)cancel_button_gesture();
        invalidate();
    }
    HWND window() const { return window_; }
    HANDLE timer_handle() const { return timer_.handle(); }
    int exit_code() const { return integration_ && !integration_finished_ ? 1 : exit_code_; }
    static LRESULT CALLBACK window_proc(HWND hwnd,UINT msg,WPARAM wparam,LPARAM lparam) {
        App* self=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) { self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); self->window_=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if(!self) return DefWindowProcW(hwnd,msg,wparam,lparam);
        try { return self->message(msg,wparam,lparam); }
        catch(const std::exception& e) {
            const std::string narrow=e.what();
            self->fail(L"Exception: " + std::wstring(narrow.begin(),narrow.end()));
            return 0;
        }
    }
    bool create(HINSTANCE instance) {
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=window_proc;
        wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101)); wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR)); wc.hbrBackground=nullptr; wc.lpszClassName=L"ALARM_MVP_Window";
        if(!RegisterClassExW(&wc)) return false;
        const DWORD style=WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
        const auto initial_dpi=GetDpiForSystem();
        RECT r{0,0,MulDiv(580,initial_dpi,96),MulDiv(720,initial_dpi,96)};
        window_=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"ALARM",style,CW_USEDEFAULT,CW_USEDEFAULT,r.right,r.bottom,nullptr,nullptr,instance,this);
        if(!window_) return false;
        if(integration_) {
            verify_button_gestures();
            SetTimer(window_,integration_timeout,15000,nullptr);
            arm_test(std::chrono::milliseconds(200));
        } else {
            ShowWindow(window_,background_ && tray_.available() ? SW_HIDE : SW_SHOW); UpdateWindow(window_);
            if(wake_test_) arm_test(std::chrono::seconds(30));
        }
        return true;
    }
    void timer_signaled() {
        reconcile();
    }
private:
    struct AudioNotice {std::atomic<HWND> window{nullptr};std::atomic<bool> pending{false};};
    std::shared_ptr<AudioNotice> audio_notice_=std::make_shared<AudioNotice>();
    HWND window_=nullptr;
    HFONT font_=nullptr;
    HFONT input_font_=nullptr;
    Glass glass_;
    Tray tray_;
    Notifications notifications_;
    HPOWERNOTIFY power_notify_=nullptr;
    HPOWERNOTIFY display_notify_=nullptr;
    bool session_registered_=false, integration_=false, wake_test_=false, config_writable_=true, shutting_down_=false;
    bool background_=false, settings_open_=false, diagnostics_open_=false, editing_time_=false;
    bool preview_active_=false;
    bool deferred_audio_start_=false,audio_recovery_=false,audio_execution_accepted_=true,lock_prelude_pending_=false;
    float starting_session_volume_=0;
    bool starting_endpoint_override_=false;
    std::uint64_t audio_generation_=0,audio_announced_generation_=0,audio_cleanup_generation_=0;
    unsigned integration_audio_starts_=0;
    bool animation_running_=false;
    bool device_open_=false,render_failure_reported_=false;
    bool keyboard_focus_=false;
    struct ButtonGesture { HWND window=nullptr; int id=0; float x=0,y=0; bool right=false,moved=false; } button_gesture_;
    int hovered_control_=0,last_page_=-1;
    UINT open_message_=0;
    ULONGLONG preview_due_=0;
    ULONGLONG preview_loading_due_=0;
    float pointer_x_=290, pointer_y_=330;
    std::wstring system_integration_status_;
    std::wstring last_alarm_notification_;
    bool system_suspended_=false;
    bool lock_test_active_=false;
    bool lock_waiting_alarm_=false, session_locked_=false;
    ULONGLONG audio_recovery_due_=0, lock_prelude_due_=0, lock_sample_due_=0, lock_timeout_due_=0;
    bool integration_finished_=false;
    DWORD display_state_=255;
    std::filesystem::path directory_;
    std::filesystem::path widget_bridge_;
    Config config_{};
    WakeTimer timer_;
    AudioPlayer audio_;
    ElapsedCycle cycle_;
    std::optional<TimePoint> scheduled_;
    bool temporary_=false;
    Cycle::State applied_=Cycle::State::Idle;
    std::wstring schedule_status_,audio_status_,load_error_;
    std::wstring last_stop_status_;
    AudioOptions test_audio_options_;
    std::vector<AudioDevice> devices_;
    std::vector<Cycle::State> integration_states_;
    int exit_code_=0;

    HWND control(int id) const { return GetDlgItem(window_,id); }
    static std::wstring window_text(HWND h) {
        const auto length=GetWindowTextLengthW(h);
        if(length<0||length>32768) return {};
        std::wstring result(static_cast<std::size_t>(length)+1,L'\0');
        GetWindowTextW(h,result.data(),length+1);result.resize(static_cast<std::size_t>(length));return result;
    }
    void set_text(int id,const std::wstring& text) { if(window_text(control(id))!=text) SetWindowTextW(control(id),text.c_str()); }
    void update_fonts(UINT dpi) {
        const auto standard=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto clock=CreateFontW(-MulDiv(84,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI Variable Display");
        if(!standard||!clock){if(standard)DeleteObject(standard);if(clock)DeleteObject(clock);return;}
        const auto old_standard=font_,old_clock=input_font_;font_=standard;input_font_=clock;
        for(HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetDlgCtrlID(child)==TimeInput?input_font_:font_),FALSE);
        if(old_standard)DeleteObject(old_standard);
        if(old_clock)DeleteObject(old_clock);
        if(const auto list=control(DeviceList))SendMessageW(list,LB_SETITEMHEIGHT,0,MulDiv(34,dpi,96));
        if(const auto device=control(Device)){SendMessageW(device,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),MulDiv(30,dpi,96));SendMessageW(device,CB_SETITEMHEIGHT,0,MulDiv(30,dpi,96));}
    }
    bool visible_graphics() const {return !integration_&&!session_locked_&&!system_suspended_&&display_state_!=0&&IsWindowVisible(window_)&&!IsIconic(window_);}
    void invalidate() {if(visible_graphics()) InvalidateRect(window_,nullptr,FALSE);}
    bool update_pointer(float x,float y) {
        if(std::abs(x-pointer_x_)<.01f&&std::abs(y-pointer_y_)<.01f)return false;
        pointer_x_=x;pointer_y_=y;return true;
    }
    bool preparing_audio() const {
        return cycle_.state()==Cycle::State::Ringing&&(deferred_audio_start_||audio_.status().state==AudioState::Starting);
    }
    HWND add(const wchar_t* type,const wchar_t* label,int id,DWORD extra=0) {
        if(std::wstring(type)==L"BUTTON" && id!=Enable && id!=Override && id!=Startup) extra=(extra&~BS_TYPEMASK)|BS_OWNERDRAW;
        const auto h=CreateWindowExW(0,type,label,WS_CHILD|extra,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        SetWindowSubclass(h,button_proc,1,reinterpret_cast<DWORD_PTR>(this));
        return h;
    }
    static bool gel_button(HWND h,int id) {
        if(id==Startup||id==Enable||id==Override||id==Time||id==CaptionMinimize||id==CaptionMaximize||id==CaptionClose)return false;
        wchar_t type[16]{};GetClassNameW(h,type,16);
        return lstrcmpiW(type,L"BUTTON")==0;
    }
    void cancel_button_gesture() {
        const auto previous=button_gesture_;
        button_gesture_={};
        if(!previous.window)return;
        glass_.end_drag();glass_.press(previous.id,false);
        if(GetCapture()==previous.window)ReleaseCapture();
        request_animation();
    }
    void begin_button_gesture(HWND h,int id,LPARAM coordinates,bool right) {
        cancel_button_gesture();keyboard_focus_=false;
        if(!integration_)SetFocus(h);
        POINT point{GET_X_LPARAM(coordinates),GET_Y_LPARAM(coordinates)};
        MapWindowPoints(h,window_,&point,1);const auto scale=96.f/GetDpiForWindow(window_);
        const float x=point.x*scale,y=point.y*scale;
        update_pointer(x,y);button_gesture_={h,id,x,y,right,false};
        if(!integration_){SetCapture(h);if(GetCapture()!=h){button_gesture_={};return;}}
        glass_.begin_drag(id,x,y);request_animation();
    }
    void drag_button_gesture(HWND h,LPARAM coordinates) {
        if(button_gesture_.window!=h)return;
        POINT point{GET_X_LPARAM(coordinates),GET_Y_LPARAM(coordinates)};
        MapWindowPoints(h,window_,&point,1);const auto scale=96.f/GetDpiForWindow(window_);
        const float x=point.x*scale,y=point.y*scale;update_pointer(x,y);
        if(std::hypot(x-button_gesture_.x,y-button_gesture_.y)>4.f)button_gesture_.moved=true;
        glass_.drag(x,y);request_animation();
    }
    void finish_button_gesture(HWND h,LPARAM coordinates,bool right) {
        if(button_gesture_.window!=h||button_gesture_.right!=right)return;
        drag_button_gesture(h,coordinates);
        const auto gesture=button_gesture_;
        RECT bounds{};GetClientRect(h,&bounds);
        const POINT point{GET_X_LPARAM(coordinates),GET_Y_LPARAM(coordinates)};
        const bool activate=!gesture.right&&!gesture.moved&&PtInRect(&bounds,point);
        cancel_button_gesture();
        if(activate&&IsWindow(h)&&IsWindowEnabled(h))
            SendMessageW(window_,WM_COMMAND,MAKEWPARAM(gesture.id,BN_CLICKED),reinterpret_cast<LPARAM>(h));
    }
    void verify_button_gestures() {
        // Only this integration process's hidden child HWNDs receive messages.
        // No SendInput, system capture, focus change, sound or desktop is used.
        const auto button=control(Settings);RECT bounds{};GetClientRect(button,&bounds);
        const int x=(bounds.right-bounds.left)/2,y=(bounds.bottom-bounds.top)/2;
        const LPARAM origin=MAKELPARAM(x,y),pulled=MAKELPARAM(x+MulDiv(16,GetDpiForWindow(window_),96),y);
        const auto check=[&](bool condition){if(!condition)throw std::runtime_error("Integration: elastic gesture activated an unexpected command");};
        SendMessageW(button,WM_RBUTTONDOWN,MK_RBUTTON,origin);
        SendMessageW(button,WM_MOUSEMOVE,MK_RBUTTON,pulled);
        SendMessageW(button,WM_RBUTTONUP,0,pulled);
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,origin);
        SendMessageW(button,WM_MOUSEMOVE,MK_LBUTTON,pulled);
        SendMessageW(button,WM_MOUSEMOVE,MK_LBUTTON,origin);
        SendMessageW(button,WM_LBUTTONUP,0,origin);
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_LBUTTONDBLCLK,MK_LBUTTON,origin);
        SendMessageW(button,WM_MOUSEMOVE,MK_LBUTTON,pulled);
        SendMessageW(button,WM_LBUTTONUP,0,pulled);
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_RBUTTONDBLCLK,MK_RBUTTON,origin);
        SendMessageW(button,WM_RBUTTONUP,0,origin);
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,origin);
        SendMessageW(button,WM_CAPTURECHANGED,0,0);
        SendMessageW(button,WM_LBUTTONUP,0,origin);
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,origin);
        SendMessageW(button,WM_LBUTTONUP,0,MAKELPARAM(bounds.right+10,y));
        check(!settings_open_&&!button_gesture_.window);
        SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,origin);
        SendMessageW(button,WM_LBUTTONUP,0,origin);
        check(settings_open_&&!button_gesture_.window);
        SendMessageW(window_,WM_COMMAND,MAKEWPARAM(Back,BN_CLICKED),reinterpret_cast<LPARAM>(control(Back)));
        check(!settings_open_);
        append_log(directory_,L"Elastic gesture integration: right hold, drag-return, lost capture and outside release cancelled; ordinary click activated.");
    }
    static LRESULT CALLBACK button_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<App*>(data);
        const int id=GetDlgCtrlID(h);
        if(msg==WM_LBUTTONDOWN||msg==WM_RBUTTONDOWN||msg==WM_LBUTTONDBLCLK||msg==WM_RBUTTONDBLCLK)app->keyboard_focus_=false;
        if(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)app->keyboard_focus_=true;
        if(msg==WM_KEYDOWN&&wp==VK_ESCAPE&&app->button_gesture_.window){app->cancel_button_gesture();return 0;}
        if(msg==WM_KEYDOWN&&wp==VK_F12){SendMessageW(app->window_,WM_KEYDOWN,wp,lp);return 0;}
        // Native HWNDs retain input, accessibility and keyboard semantics. The
        // composition visual paints every control together above their surfaces.
        if(msg==WM_PAINT) {PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
        if(msg==WM_ERASEBKGND||msg==WM_PRINTCLIENT) return 1;
        if(gel_button(h,id)) {
            if((msg==WM_LBUTTONDOWN||msg==WM_RBUTTONDOWN||msg==WM_LBUTTONDBLCLK||msg==WM_RBUTTONDBLCLK)&&IsWindowEnabled(h)) {
                app->begin_button_gesture(h,id,lp,msg==WM_RBUTTONDOWN||msg==WM_RBUTTONDBLCLK);return 0;
            }
            if(msg==WM_MOUSEMOVE&&app->button_gesture_.window==h){app->drag_button_gesture(h,lp);return 0;}
            if(msg==WM_LBUTTONUP||msg==WM_RBUTTONUP){app->finish_button_gesture(h,lp,msg==WM_RBUTTONUP);return 0;}
            if(msg==WM_CONTEXTMENU)return 0;
        }
        if((msg==WM_CAPTURECHANGED||msg==WM_CANCELMODE||msg==WM_KILLFOCUS||msg==WM_NCDESTROY||((msg==WM_SHOWWINDOW||msg==WM_ENABLE)&&!wp))&&app->button_gesture_.window==h)app->cancel_button_gesture();
        if(msg==WM_LBUTTONDOWN||(msg==WM_KEYDOWN&&wp==VK_SPACE)){app->glass_.press(id,true);app->request_animation();}
        if(msg==WM_LBUTTONUP||msg==WM_CAPTURECHANGED||msg==WM_KILLFOCUS||(msg==WM_KEYUP&&wp==VK_SPACE)){app->glass_.press(id,false);app->request_animation();}
        if(id==Device && (msg==WM_LBUTTONDOWN||(msg==WM_KEYDOWN&&(wp==VK_F4||wp==VK_SPACE||wp==VK_RETURN)))) {app->toggle_devices();return 0;}
        if(id==Device && msg==WM_LBUTTONUP) return 0;
        if(id==Device && ((msg==WM_SYSKEYDOWN&&wp==VK_DOWN)||(msg==CB_SHOWDROPDOWN&&wp))) {if(!app->device_open_)app->toggle_devices();return TRUE;}
        if(id==Device&&msg==CB_SHOWDROPDOWN&&!wp){if(app->device_open_){app->device_open_=false;app->layout();}return TRUE;}
        if(id==DeviceList && msg==WM_KEYDOWN && wp==VK_ESCAPE) {app->device_open_=false;app->layout();SetFocus(app->control(Device));return 0;}
        if(id==DeviceList && msg==WM_GETDLGCODE && (wp==VK_RETURN||wp==VK_ESCAPE))return DefSubclassProc(h,msg,wp,lp)|DLGC_WANTMESSAGE;
        if(id==Device && msg==WM_GETDLGCODE && (wp==VK_RETURN||wp==VK_SPACE))return DefSubclassProc(h,msg,wp,lp)|DLGC_WANTMESSAGE;
        if(id==DeviceList && msg==WM_KEYDOWN && wp==VK_RETURN){PostMessageW(app->window_,WM_APP+42,0,0);return 0;}
        if(msg==WM_MOUSEMOVE) {
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};MapWindowPoints(h,app->window_,&point,1);const auto scale=96.f/GetDpiForWindow(app->window_);const bool moved=app->update_pointer(point.x*scale,point.y*scale);
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0}; TrackMouseEvent(&tracking);
            if(app->hovered_control_!=id) {if(app->hovered_control_)app->glass_.hover(app->hovered_control_,false);app->hovered_control_=id;app->glass_.hover(id,true);app->request_animation();}
            else if(moved)app->invalidate();
        } else if(msg==WM_MOUSELEAVE) {if(app->hovered_control_==id)app->hovered_control_=0;app->glass_.hover(id,false);app->request_animation();}
        else if(msg==WM_NCDESTROY) RemoveWindowSubclass(h,button_proc,1);
        const auto result=DefSubclassProc(h,msg,wp,lp);
        if(id==DeviceList&&msg==WM_LBUTTONUP)PostMessageW(app->window_,WM_APP+42,0,0);
        if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==WM_KEYDOWN||msg==WM_CHAR||msg==BM_SETCHECK||msg==BM_SETSTATE||msg==WM_ENABLE) app->invalidate();
        return result;
    }
    static LRESULT CALLBACK time_input_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<App*>(data);
        if(msg==WM_GETDLGCODE && (wp==VK_RETURN || wp==VK_ESCAPE)) return DefSubclassProc(h,msg,wp,lp)|DLGC_WANTMESSAGE;
        if(msg==WM_CHAR && (wp==VK_RETURN || wp==VK_ESCAPE)) return 0;
        if(msg==WM_KEYDOWN && wp==VK_RETURN) {app->finish_time_edit();return 0;}
        if(msg==WM_KEYDOWN && wp==VK_ESCAPE) {app->editing_time_=false;app->layout();SetFocus(app->control(Time));return 0;}
        if(msg==WM_NCDESTROY) RemoveWindowSubclass(h,time_input_proc,1);
        return DefSubclassProc(h,msg,wp,lp);
    }
    void request_animation() {
        if(!shutting_down_&&visible_graphics()) {
            if(glass_.animation_enabled() && !animation_running_) {SetTimer(window_,animation_timer,16,nullptr);animation_running_=true;}
            invalidate();
        }
    }
    void show_window() {
        ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);layout();refresh();
    }
    GlassState visual_state() const {
        GlassState state;RECT client{};GetClientRect(window_,&client);const float scale=96.f/GetDpiForWindow(window_);
        state.width=client.right*scale;state.height=client.bottom*scale;state.pointer_x=pointer_x_;state.pointer_y=pointer_y_;
        state.enabled=config_.enabled;state.ringing=cycle_.state()==Cycle::State::Ringing&&audio_.playing();state.silent=cycle_.state()==Cycle::State::Silent;
        state.audio_starting=preparing_audio();
        state.audio_error=cycle_.state()==Cycle::State::Ringing&&!audio_.playing()&&!state.audio_starting;
        state.editing=editing_time_;state.active=GetForegroundWindow()==window_;
        state.preview=preview_active_;
        state.keyboard_focus=keyboard_focus_;
        state.settings=settings_open_;state.diagnostics=diagnostics_open_;
        state.sound=config_.wav_path.empty()?L"Built-in ALARM tone":std::filesystem::path(config_.wav_path).filename().wstring();
        if(cycle_.active()) state.next=L"Next transition in " + countdown(cycle_.next_transition(Clock::now(),GetTickCount64()));
        else if(scheduled_) state.next=(temporary_?L"Test scheduled: ":L"Next alarm: ")+local_time(*scheduled_)+L"\nIn "+countdown(*scheduled_);
        else state.next=L"Choose a time, then enable your alarm.";
        state.detail=audio_status_.empty()?system_integration_status_:audio_status_;
        if(!config_writable_) state.detail=L"Unreadable configuration: file preserved.";
        for(int id=Enable;id<=CaptionClose;++id) {
            const auto h=control(id);if(!h || !(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)) continue;
            RECT rect{};GetWindowRect(h,&rect);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);
            GlassControl c;c.id=id;c.left=rect.left*scale;c.top=rect.top*scale;c.right=rect.right*scale;c.bottom=rect.bottom*scale;
            c.text=window_text(h);c.enabled=IsWindowEnabled(h);c.focused=GetFocus()==h;
            c.pressed=(SendMessageW(h,BM_GETSTATE,0,0)&BST_PUSHED)!=0;
            if(id==Startup)c.checked=SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;
            if(id==TimeInput||id==SessionVolume){DWORD first=0,last=0;SendMessageW(h,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));c.selection_start=first;c.selection_end=last;}
            if(id==Device){const auto selected=SendMessageW(h,CB_GETCURSEL,0,0);const auto length=SendMessageW(h,CB_GETLBTEXTLEN,selected,0);if(length>=0&&length<=32768){c.text.resize(static_cast<std::size_t>(length)+1);SendMessageW(h,CB_GETLBTEXT,selected,reinterpret_cast<LPARAM>(c.text.data()));c.text.resize(static_cast<std::size_t>(length));}}
            state.controls.push_back(std::move(c));
        }
        if(device_open_) {
            state.devices.push_back(L"Default Windows device");for(const auto& d:devices_)state.devices.push_back(d.name);
            state.selected_device=static_cast<int>(SendMessageW(control(DeviceList),LB_GETCURSEL,0,0));state.device_top=static_cast<int>(SendMessageW(control(DeviceList),LB_GETTOPINDEX,0,0));
        }
        return state;
    }
    void initialize() {
        audio_notice_->window=window_;
        audio_.set_state_callback([notice=audio_notice_] {
            const auto window=notice->window.load();
            if(window&&!notice->pending.exchange(true)&&!PostMessageW(window,WM_APP+43,0,0))notice->pending=false;
        });
        const auto loaded=load_config(directory_/L"config.ini",config_,load_error_);
        if(!integration_&&!wake_test_) {
            std::error_code error;
            if(std::filesystem::equivalent(directory_,config_directory(),error)&&!error) {
                PWSTR profile=nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&profile))){widget_bridge_=std::filesystem::path(profile)/L".alarm";CoTaskMemFree(profile);}
            }
        }
        config_writable_=loaded!=ConfigLoadResult::Invalid && loaded!=ConfigLoadResult::IoError;
        if(!config_writable_) config_.enabled=false;
        if(integration_ && config_writable_) {
            config_.enabled=false; config_.session_volume=0; config_.override_endpoint=false;
            if(!save_config(directory_/L"config.ini",config_,load_error_)) throw std::runtime_error("Cannot prepare isolated integration config");
        }
        glass_.configure(window_);
        update_fonts(GetDpiForWindow(window_));
        add(L"STATIC",L"ALARM: OFF",State,SS_LEFT);
        add(L"BUTTON",L"Daily alarm",Enable,BS_AUTOCHECKBOX|WS_TABSTOP);
        add(L"BUTTON",L"06:00",Time,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Enable alarm",Save,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"STATIC",L"Alarm time",204);
        add(L"EDIT",L"06:00",TimeInput,ES_CENTER|ES_AUTOHSCROLL|WS_TABSTOP);SendMessageW(control(TimeInput),EM_SETLIMITTEXT,5,0);
        SetWindowSubclass(control(TimeInput),time_input_proc,1,reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(control(TimeInput),WM_SETFONT,reinterpret_cast<WPARAM>(input_font_),TRUE);
        add(L"BUTTON",L"Save time",TimeDone,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"STATIC",L"",Next,SS_LEFT);
        add(L"STATIC",L"Audio output",200);
        add(L"COMBOBOX",L"",Device,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP);
        add(L"LISTBOX",L"Audio outputs",DeviceList,LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP);
        SendMessageW(control(DeviceList),LB_SETITEMHEIGHT,0,MulDiv(34,GetDpiForWindow(window_),96));
        SendMessageW(control(Device),CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),MulDiv(30,GetDpiForWindow(window_),96));
        SendMessageW(control(Device),CB_SETITEMHEIGHT,0,MulDiv(30,GetDpiForWindow(window_),96));
        add(L"BUTTON",L"Choose a WAV",Browse,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Built-in sound",BuiltIn,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"STATIC",L"Sound: built-in tone",FileLabel,SS_LEFT);
        add(L"STATIC",L"Alarm volume (%)",202);
        add(L"EDIT",L"50",SessionVolume,ES_NUMBER|ES_CENTER|ES_AUTOHSCROLL|WS_TABSTOP);
        SendMessageW(control(SessionVolume),EM_SETLIMITTEXT,3,0);
        set_text(SessionVolume,std::to_wstring(static_cast<int>(std::lround(config_.session_volume*100))));
        add(L"STATIC",L"Windows at 100% during the alarm",Override,SS_LEFT);
        add(L"BUTTON",L"Start with Windows near the clock",Startup,BS_AUTOCHECKBOX|WS_TABSTOP);
        add(L"BUTTON",L"−",VolumeDown,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"+",VolumeUp,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"STATIC",L"Volume and mute are restored during silence and at STOP.",201);
        add(L"BUTTON",L"In 10 s",Test10,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"In 30 s",Test30,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Listen to sound",TestNow,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"STOP · silence alarm",Stop,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Quit",Quit,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Locked test (10 s)",TestLocked,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Settings",Settings,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Save",SettingsApply,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Back",Back,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Diagnostics",Diagnostics,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Minimize",CaptionMinimize,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Maximize",CaptionMaximize,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"BUTTON",L"Close to system tray",CaptionClose,BS_PUSHBUTTON|WS_TABSTOP);
        add(L"STATIC",L"After 3 s of sound, Windows locks.\nThe alarm rings 10 s later and stops after 20 s.",203,SS_LEFT);
        add(L"STATIC",L"",alarmapp::ui::AudioStatus,SS_LEFT);
        add(L"STATIC",L"",Policy,SS_LEFT);
        add(L"STATIC",L"Cycle: 5 min ringing / 10 min silent, until STOP.\nClosing hides the window and keeps the engine running. Quit stops ALARM.\nThe alarm may turn on the locked screen.",Help,SS_LEFT);
        SendMessageW(control(Enable),BM_SETCHECK,config_.enabled ? BST_CHECKED : BST_UNCHECKED,0);
        bool startup_enabled=false;std::wstring startup_error;
        if(!integration_) {
            const bool read=query_current_user_startup(executable_path(),startup_enabled,startup_error);
            config_.startup=read&&startup_enabled;
            EnableWindow(control(Startup),read?TRUE:FALSE);
            if(!startup_error.empty()) {system_integration_status_=startup_error;append_log(directory_,L"Startup: "+startup_error);}
        }
        SendMessageW(control(Startup),BM_SETCHECK,config_.startup ? BST_CHECKED : BST_UNCHECKED,0);
        wchar_t time[16]{}; swprintf_s(time,L"%02d:%02d",config_.hour,config_.minute); set_text(Time,time);
        std::wstring error; devices_=AudioPlayer::devices(error);
        SendMessageW(control(Device),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Default Windows device"));
        int selection=0;
        for(std::size_t i=0;i<devices_.size();++i) {
            SendMessageW(control(Device),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices_[i].name.c_str()));
            if(devices_[i].id==config_.device_id) selection=static_cast<int>(i+1);
        }
        if(!config_.device_id.empty() && selection==0) {
            devices_.push_back({config_.device_id,L"Saved device (unavailable)"});
            selection=static_cast<int>(devices_.size());
            SendMessageW(control(Device),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices_.back().name.c_str()));
        }
        SendMessageW(control(Device),CB_SETCURSEL,selection,0);
        SendMessageW(control(DeviceList),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Default Windows device"));
        for(const auto& d:devices_)SendMessageW(control(DeviceList),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(d.name.c_str()));
        SendMessageW(control(DeviceList),LB_SETCURSEL,selection,0);
        if(!error.empty()) audio_status_=error;
        if(!config_.wav_path.empty()) set_text(FileLabel,L"Sound: " + std::filesystem::path(config_.wav_path).filename().wstring());
        power_notify_=RegisterSuspendResumeNotification(window_,DEVICE_NOTIFY_WINDOW_HANDLE);
        const DWORD power_error=power_notify_ ? ERROR_SUCCESS : GetLastError();
        display_notify_=RegisterPowerSettingNotification(window_,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
        session_registered_=WTSRegisterSessionNotification(window_,NOTIFY_FOR_THIS_SESSION)!=FALSE;
        append_log(directory_,L"Engine started. " + wake_policy());
        if(!integration_) {
            std::wstring tray_error;
            if(!tray_.create(window_,GetModuleHandleW(nullptr),tray_error)) system_integration_status_=L"System tray icon unavailable: "+tray_error;
            if(!notifications_.initialize(window_,executable_path())) {
                system_integration_status_+=L" Notifications unavailable: "+notifications_.error();
                append_log(directory_,system_integration_status_);
            }
        }
        if(!power_notify_) append_log(directory_,L"WARNING: resume notification registration failed: " + system_error(power_error));
        if(!config_writable_) {
            schedule_status_=L"Unreadable configuration, file preserved: " + load_error_;
            EnableWindow(control(Save),FALSE); EnableWindow(control(Enable),FALSE);
        } else {
            if(loaded==ConfigLoadResult::Missing) {
                std::wstring save_error;
                if(!save_config(directory_/L"config.ini",config_,save_error)) { config_writable_=false; schedule_status_=save_error; }
            }
            schedule_daily();
        }
        layout(); refresh();
        if(config_.enabled && !integration_) notify_armed();
    }
    void toggle_devices() {
        device_open_=!device_open_;
        if(device_open_) {SendMessageW(control(DeviceList),LB_SETCURSEL,SendMessageW(control(Device),CB_GETCURSEL,0,0),0);glass_.reveal_devices();request_animation();}
        layout();SetFocus(control(device_open_?DeviceList:Device));
    }
    void layout() {
        RECT r{};GetClientRect(window_,&r);const auto dpi=GetDpiForWindow(window_);
        const int width=MulDiv(r.right,96,dpi);
        struct Position {int id,x,y,w,h;};
        std::vector<Position> positions;
        auto place=[&](int id,int x,int y,int w,int h){positions.push_back({id,x,y,w,h});};
        place(CaptionMinimize,width-126,5,36,32);place(CaptionMaximize,width-84,5,36,32);place(CaptionClose,width-42,5,36,32);
        if(!settings_open_) {
            device_open_=false;
            place(Settings,width-154,68,126,36);
            place(editing_time_?TimeInput:Time,54,248,width-108,103);
            place(editing_time_?TimeDone:Save,26,478,width-52,60);
            place(cycle_.active()||temporary_?Stop:TestNow,26,552,width-52,40);
        } else {
            place(Back,width-154,68,126,36);
            if(diagnostics_open_) {
                device_open_=false;
                place(Test10,48,262,(width-108)/2,44);place(Test30,width/2+6,262,(width-108)/2,44);
                place(TestLocked,48,318,width-96,34);place(Stop,26,587,width-52,46);
            } else {
                place(Device,48,235,width-96,44);
                place(VolumeDown,width-207,290,38,36);place(SessionVolume,width-159,290,59,36);place(VolumeUp,width-90,290,38,36);
                place(Browse,48,346,148,38);place(BuiltIn,width-186,346,138,38);
                place(Override,48,430,width-96,28);place(Startup,48,496,width-96,28);
                place(SettingsApply,26,580,width-52,52);place(Diagnostics,26,651,166,34);place(Quit,width-138,651,112,34);
                if(device_open_) place(DeviceList,48,278,width-96,std::min(170,static_cast<int>(devices_.size()+1)*34));
            }
        }
        HDWP batch=BeginDeferWindowPos(CaptionClose-Enable+1);
        for(int id=Enable;id<=CaptionClose;++id) {
            const auto h=control(id);if(!h)continue;
            const auto desired=std::find_if(positions.begin(),positions.end(),[id](const auto& p){return p.id==id;});
            const bool was_visible=(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0;
            if(desired==positions.end()) {
                if(was_visible) {
                    glass_.hover(id,false);if(hovered_control_==id)hovered_control_=0;
                    if(batch) batch=DeferWindowPos(batch,h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_HIDEWINDOW);
                    else ShowWindow(h,SW_HIDE);
                }
            } else {
                const auto& p=*desired;
                const auto flags=SWP_NOACTIVATE|SWP_NOREDRAW|SWP_SHOWWINDOW|(id==DeviceList?0:SWP_NOZORDER);
                if(batch) batch=DeferWindowPos(batch,h,id==DeviceList?HWND_TOP:nullptr,MulDiv(p.x,dpi,96),MulDiv(p.y,dpi,96),MulDiv(p.w,dpi,96),MulDiv(p.h,dpi,96),flags);
                else SetWindowPos(h,id==DeviceList?HWND_TOP:nullptr,MulDiv(p.x,dpi,96),MulDiv(p.y,dpi,96),MulDiv(p.w,dpi,96),MulDiv(p.h,dpi,96),flags);
            }
        }
        const bool positioned=batch&&EndDeferWindowPos(batch);
        if(!positioned) {
            // If the batch fails, Windows discards previously queued positions.
            // Replay the complete desired layout, including those early items.
            for(int id=Enable;id<=CaptionClose;++id) {
                const auto h=control(id);if(!h)continue;
                const auto desired=std::find_if(positions.begin(),positions.end(),[id](const auto& p){return p.id==id;});
                if(desired==positions.end()){ShowWindow(h,SW_HIDE);continue;}
                const auto& p=*desired;
                SetWindowPos(h,id==DeviceList?HWND_TOP:nullptr,MulDiv(p.x,dpi,96),MulDiv(p.y,dpi,96),MulDiv(p.w,dpi,96),MulDiv(p.h,dpi,96),SWP_NOACTIVATE|SWP_NOREDRAW|SWP_SHOWWINDOW|(id==DeviceList?0:SWP_NOZORDER));
            }
        }
        const int page=settings_open_?(diagnostics_open_?2:1):0;
        if(last_page_!=-1 && last_page_!=page){glass_.transition();request_animation();}
        last_page_=page;
        invalidate();
    }
    void update_countdown_timer() {
        if(visible_graphics() && (scheduled_ || cycle_.active())) SetTimer(window_,countdown_timer,1000,nullptr);
        else KillTimer(window_,countdown_timer);
    }
    void refresh() {
        WidgetState widget;widget.enabled=config_.enabled;widget.hour=config_.hour;widget.minute=config_.minute;
        widget.phase=cycle_.state()==Cycle::State::Ringing?(audio_.playing()?WidgetPhase::Ringing:preparing_audio()?WidgetPhase::Scheduled:WidgetPhase::Error):cycle_.state()==Cycle::State::Silent?WidgetPhase::Silent:scheduled_?WidgetPhase::Scheduled:WidgetPhase::Idle;
        if(cycle_.active()) widget.next_transition_utc=widget_utc(cycle_.next_transition(Clock::now(),GetTickCount64()));
        else if(scheduled_) widget.next_transition_utc=widget_utc(*scheduled_);
        widget.sound_label=config_.wav_path.empty()?L"Built-in ALARM tone":std::filesystem::path(config_.wav_path).filename().wstring();
        if(widget.sound_label.size()>160){widget.sound_label.resize(159);if(widget.sound_label.back()>=0xd800&&widget.sound_label.back()<=0xdbff)widget.sound_label.pop_back();widget.sound_label+=L"…";}
        std::wstring widget_error;if(!publish_widget_state(directory_,widget,widget_error)&&!widget_error.empty()) append_log(directory_,L"Widget state: "+widget_error);
        if(!widget_bridge_.empty()&&!publish_widget_state(widget_bridge_,widget,widget_error)) append_log(directory_,L"Widget bridge: "+widget_error);
        std::wstring state=L"ALARM: OFF";
        if(cycle_.state()==Cycle::State::Ringing) state=audio_.playing() ? L"RINGING — audio playback started" : preparing_audio()?L"Preparing audio":L"RINGING — audio error";
        else if(cycle_.state()==Cycle::State::Silent) state=L"SILENT — next ring after 10 minutes";
        else if(scheduled_) state=temporary_ ? L"TEST armed" : L"ALARM: ON";
        set_text(State,state);
        if(cycle_.active()) {
            const auto next=cycle_.next_transition(Clock::now(),GetTickCount64());
            set_text(Next,L"Next transition: " + local_time(next) + L"\nIn " + countdown(next));
        }
        else if(scheduled_) set_text(Next,L"Next alarm: " + local_time(*scheduled_) + L"\nIn " + countdown(*scheduled_));
        else set_text(Next,L"Windows local time. Enable and save, or use TEST.");
        if(cycle_.state()==Cycle::State::Ringing && !audio_.error().empty()) audio_status_=audio_.error();
        set_text(alarmapp::ui::AudioStatus,audio_status_);
        set_text(Policy,schedule_status_);
        EnableWindow(control(Stop),cycle_.active() || temporary_);
        set_text(Save,config_.enabled?L"Disable alarm":L"Enable alarm");
        EnableWindow(control(BuiltIn),config_writable_&&!config_.wav_path.empty());
        if(tray_.available()) {
            TrayStatus status;status.enabled=config_.enabled;status.can_stop=cycle_.active()||temporary_;status.hour=config_.hour;status.minute=config_.minute;
            status.state=cycle_.state()==Cycle::State::Ringing?(audio_.playing()?L"Ringing":preparing_audio()?L"Preparing audio":L"Audio error"):cycle_.state()==Cycle::State::Silent?L"Pause":config_.enabled?L"Enabled":L"Disabled";
            std::wstring error;if(!tray_.update(status,error)) {append_log(directory_,L"Tray: "+error);system_integration_status_=L"Icon unavailable: "+error;}
        }
        update_countdown_timer();
        if(cycle_.state()==Cycle::State::Ringing&&audio_.playing()) request_animation();
        // Redraw only the visible UI. Hidden mode has no graphics timer.
        if(visible_graphics()) {
            const bool stop_visible=IsWindowVisible(control(Stop))!=FALSE;
            if(!settings_open_ && stop_visible!=(cycle_.active()||temporary_)) layout();
            else invalidate();
        }
    }
    void notification_status() {
        if(!notifications_.error().empty()) {
            system_integration_status_=L"Windows notifications unavailable. See alarm.log for details.";
            append_log(directory_,L"Notification: "+notifications_.error());
        }
    }
    void notify_armed() {
        if(integration_ || !notifications_.available()) return;
        if(config_.enabled) {
            wchar_t hour[16]{};swprintf_s(hour,L"%02d:%02d",config_.hour,config_.minute);
            notifications_.show(L"Alarm enabled",L"Next alarm at "+std::wstring(hour)+L". Select to open ALARM after unlocking.");
        } else notifications_.clear();
        notification_status();
    }
    void arm_next() {
        if(cycle_.active()) {
            if(!timer_.arm(cycle_.next_transition(Clock::now(),GetTickCount64()),true,schedule_status_)) fail(L"Cycle timer: " + schedule_status_);
        } else if(scheduled_) {
            if(!timer_.arm(*scheduled_,true,schedule_status_)) { scheduled_.reset(); fail(L"Schedule: " + schedule_status_); }
        } else { timer_.cancel(); if(config_writable_) schedule_status_=wake_policy(); }
    }
    void schedule_daily() {
        scheduled_.reset(); temporary_=false;
        if(config_.enabled) scheduled_=next_daily_alarm(Clock::now(),config_.hour,config_.minute);
        arm_next();
    }
    void stop_audio() {
        deferred_audio_start_=false;audio_generation_=0;
        audio_.cancel();
        if(!audio_.status().finished)SetTimer(window_,audio_status_timer,50,nullptr);
    }
    void start_ringing_audio(bool recovery=false) {
        const auto flags=integration_ ? ES_CONTINUOUS|ES_SYSTEM_REQUIRED : ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED;
        audio_execution_accepted_=SetThreadExecutionState(flags)!=0;
        audio_recovery_=recovery;
        append_log(directory_,audio_execution_accepted_ ? L"RINGING: system/display execution request accepted" : L"WARNING: execution request failed; full resume unverified");
        const auto options=temporary_ ? test_audio_options_ : make_alarm_audio_options(config_,integration_?AudioPurpose::Integration:AudioPurpose::Alarm);
        starting_session_volume_=options.session_volume;
        starting_endpoint_override_=options.override_endpoint;
        std::wstring error;
        if(audio_.begin_start(options,error)) {
            KillTimer(window_,audio_status_timer);
            deferred_audio_start_=false;audio_generation_=audio_.status().generation;
            audio_status_=L"Preparing audio…";
            audio_changed();
        } else if(!audio_.status().finished) {
            deferred_audio_start_=true;audio_status_=L"Preparing audio · closing the previous output…";
            SetTimer(window_,audio_status_timer,50,nullptr);
        } else {
            audio_status_=L"Playback failed: "+error;SetThreadExecutionState(ES_CONTINUOUS);
            append_log(directory_,L"RINGING: audio begin FAILED. "+error);
            if(integration_)fail(L"Integration: audio did not start: "+error);
        }
    }
    void audio_changed() {
        if(shutting_down_)return;
        const auto status=audio_.status();
        if(status.finished&&status.generation&&status.generation!=audio_cleanup_generation_) {
            audio_cleanup_generation_=status.generation;last_stop_status_=status.message;
            if(!status.message.empty())append_log(directory_,L"Audio cleanup status: "+status.message);
        }
        if(status.finished||(!deferred_audio_start_&&(status.state==AudioState::Starting||status.state==AudioState::Playing)))KillTimer(window_,audio_status_timer);
        else SetTimer(window_,audio_status_timer,50,nullptr);
        if(cycle_.state()!=Cycle::State::Ringing||system_suspended_) {
            if(status.finished&&(cycle_.state()==Cycle::State::Idle||cycle_.state()==Cycle::State::Silent))
                audio_status_=status.message.empty()?(cycle_.state()==Cycle::State::Silent?L"Silent · audio state restored if changed.":L"Audio stopped · audio state restored if changed."):status.message;
            refresh();
            if(integration_&&integration_states_.size()==4&&status.finished&&cycle_.state()==Cycle::State::Silent)finish_integration();
            return;
        }
        if(deferred_audio_start_&&status.finished){deferred_audio_start_=false;start_ringing_audio(audio_recovery_);refresh();return;}
        if(status.generation!=audio_generation_){refresh();return;}
        if(status.initialized&&audio_announced_generation_!=status.generation) {
            audio_announced_generation_=status.generation;
            const auto ok=status.start_succeeded;
            if(ok)++integration_audio_starts_;
            if(preview_active_&&!preview_due_) {
                preview_loading_due_=0;preview_due_=GetTickCount64()+8000;
                SetTimer(window_,preview_timer,8000,nullptr);
            }
            append_log(directory_,audio_recovery_?std::wstring(L"Audio recovery after resume: ")+(ok?L"started. ":L"FAILED. ")+status.message:(ok?L"RINGING: audio started. ":L"RINGING: audio FAILED. ")+status.message);
            append_log(directory_,L"Audio context: session_locked="+std::to_wstring(session_locked_)+L", console_display="+std::to_wstring(display_state_));
            if(!ok&&integration_){fail(L"Integration: audio did not start: "+status.message);return;}
            if(!ok&&lock_prelude_pending_){stop_alarm();show_error(L"The initial sound did not start; Windows lock cancelled.");return;}
            if(ok&&lock_test_active_&&lock_prelude_pending_) {
                lock_prelude_pending_=false;lock_prelude_due_=GetTickCount64()+3000;
                SetTimer(window_,lock_prelude_timer,3000,nullptr);
            }
            if(ok&&lock_test_active_&&lock_waiting_alarm_) {
                lock_waiting_alarm_=false;KillTimer(window_,lock_timeout_timer);lock_timeout_due_=0;
                lock_sample_due_=GetTickCount64()+20000;SetTimer(window_,lock_stop_timer,20000,nullptr);
                append_log(directory_,L"LOCK TEST: post-lock audio started; auto STOP in 20 s");
            }
        }
        if(status.state==AudioState::Playing) {
            audio_status_=(starting_endpoint_override_?L"Windows at 100% · ":L"Windows volume preserved · ")+std::wstring(L"ALARM ")+std::to_wstring(static_cast<int>(std::lround(starting_session_volume_*100)))+L" %. "+status.message;
        } else if(status.state==AudioState::Starting)audio_status_=L"Preparing audio… "+status.message;
        else if(status.state==AudioState::Error) {
            audio_status_=L"Playback failed: "+status.message;SetThreadExecutionState(ES_CONTINUOUS);
            if(integration_){fail(L"Integration: asynchronous audio failure: "+status.message);return;}
        }
        if(!audio_execution_accepted_)audio_status_+=L" Full-resume request refused.";
        if(!integration_&&!preview_active_&&!(lock_test_active_&&(lock_prelude_pending_||lock_prelude_due_))) {
            const auto title=L"The time is "+local_time(Clock::now()).substr(11,5);
            const std::wstring text=audio_.playing()?L"Your alarm is ringing. Unlock Windows to open ALARM and stop the sound.":preparing_audio()?L"Preparing audio. Unlock Windows to open ALARM.":L"Audio output is unavailable. Unlock Windows to open ALARM and view the error.";
            const auto signature=title+L"\n"+text;
            if(signature!=last_alarm_notification_){last_alarm_notification_=signature;notifications_.show(title,text,true);notification_status();}
        }
        refresh();
    }
    void schedule_audio_recovery() {
        // One coalesced callback, cancelled by STOP/silence/suspend. Let Windows
        // complete device/display power transitions before reopening WASAPI.
        if(cycle_.state()==Cycle::State::Ringing && !system_suspended_) {
            audio_recovery_due_=GetTickCount64()+1000;
            SetTimer(window_,audio_recovery_timer,1000,nullptr);
        }
    }
    void apply_phase() {
        const auto state=cycle_.state();
        if(system_suspended_) return;
        if(state==applied_) return;
        stop_audio(); SetThreadExecutionState(ES_CONTINUOUS);
        applied_=state;
        if(state==Cycle::State::Ringing) {
            start_ringing_audio();
            if(shutting_down_) return;
        } else if(state==Cycle::State::Silent) {
            last_alarm_notification_.clear();
            if(!integration_ && notifications_.available()) {notifications_.clear();notification_status();}
            KillTimer(window_,audio_recovery_timer);
            audio_recovery_due_=0;
            audio_status_=audio_.status().finished?L"Silent · audio state restored if changed.":L"Stopping playback and restoring audio…";
            append_log(directory_,L"SILENT: audio cancellation requested. " + audio_.error());
        } else { KillTimer(window_,audio_recovery_timer); audio_recovery_due_=0; append_log(directory_,L"STOP: audio cancellation requested."); }
        if(integration_ && state!=Cycle::State::Idle) {
            integration_states_.push_back(state);
            if(integration_states_.size()==1) {
                SendMessageW(window_,WM_CLOSE,0,0);
                if(shutting_down_ || !IsWindow(window_)) { fail(L"Integration: closing UI stopped the engine"); return; }
            }
            if(integration_states_.size()==4&&audio_.status().finished) finish_integration();
        }
    }
    void reconcile() {
        if(shutting_down_) return;
        const auto now=Clock::now();
        if(!cycle_.active() && scheduled_ && now>=*scheduled_) {
            if(temporary_ || alarm_due(*scheduled_,now)) {
                append_log(directory_,L"Alarm due, delay ms=" + std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(now-*scheduled_).count()));
                scheduled_.reset(); cycle_.start(now,GetTickCount64());
            } else {
                append_log(directory_,L"Stale alarm (>24h) skipped"); schedule_daily();
            }
        }
        if(cycle_.active()) cycle_.advance(GetTickCount64());
        apply_phase();
        if(!shutting_down_) { arm_next(); refresh(); }
    }
    void stop_alarm(bool schedule=true) {
        last_alarm_notification_.clear();
        if(!integration_ && notifications_.available()) {notifications_.clear();notification_status();}
        KillTimer(window_,preview_timer);preview_active_=false;preview_due_=0;preview_loading_due_=0;
        KillTimer(window_,audio_recovery_timer);
        audio_recovery_due_=0;
        KillTimer(window_,lock_prelude_timer); KillTimer(window_,lock_stop_timer); KillTimer(window_,lock_timeout_timer);
        lock_test_active_=false; lock_waiting_alarm_=false;lock_prelude_pending_=false;
        lock_prelude_due_=0; lock_sample_due_=0; lock_timeout_due_=0;
        timer_.cancel(); scheduled_.reset(); temporary_=false;
        cycle_.stop(); apply_phase(); stop_audio(); SetThreadExecutionState(ES_CONTINUOUS);
        audio_status_=audio_.status().finished?(audio_.error().empty()?L"Audio stopped · audio state restored if changed.":audio_.error()):L"Stopping playback and restoring audio…";
        if(schedule && !shutting_down_) schedule_daily();
        refresh();
    }
    void begin_preview() {
        if(!arm_test(Clock::duration::zero(),AudioPurpose::Preview)) return;
        preview_active_=true;
        preview_loading_due_=GetTickCount64()+30000;SetTimer(window_,preview_timer,1000,nullptr);
        audio_status_=L"Preparing an 8-second preview. STOP cancels the preview.";refresh();
    }
    void edit_time() {
        wchar_t current[16]{};GetWindowTextW(control(Time),current,16);set_text(TimeInput,current);
        editing_time_=true;layout();SetFocus(control(TimeInput));SendMessageW(control(TimeInput),EM_SETSEL,0,-1);
    }
    bool finish_time_edit() {
        if(!editing_time_) return true;
        wchar_t buffer[16]{};GetWindowTextW(control(TimeInput),buffer,16);std::wstring value=buffer;
        if(value.size()==4 && value[1]==L':') value=L"0"+value;
        const auto digit=[](wchar_t c){return c>=L'0' && c<=L'9';};
        if(value.size()!=5 || value[2]!=L':' || !digit(value[0]) || !digit(value[1]) || !digit(value[3]) || !digit(value[4]) || (value[0]-L'0')*10+value[1]-L'0'>23 || (value[3]-L'0')*10+value[4]-L'0'>59) {
            show_error(L"Choose a time between 00:00 and 23:59.");return false;
        }
        set_text(Time,value);editing_time_=false;layout();SetFocus(control(Time));return save_fields();
    }
    void toggle_alarm() {
        if(!finish_time_edit()) return;
        SendMessageW(control(Enable),BM_SETCHECK,config_.enabled?BST_UNCHECKED:BST_CHECKED,0);
        if(!save_fields()) SendMessageW(control(Enable),BM_SETCHECK,config_.enabled?BST_CHECKED:BST_UNCHECKED,0);
        refresh();
    }
    bool arm_test(Clock::duration delay,AudioPurpose purpose=AudioPurpose::Alarm) {
        Config test_config=config_;
        if(!read_fields(test_config)) return false;
        test_audio_options_=make_alarm_audio_options(test_config,integration_?AudioPurpose::Integration:purpose);
        stop_alarm(false);
        scheduled_=Clock::now()+delay; temporary_=true;
        append_log(directory_,L"TEST armed, delay ms=" + std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()));
        arm_next(); refresh();
        return scheduled_.has_value();
    }
    void begin_locked_test() {
        // The user initiates this from the visible app, with timing instructions
        // beside the button. No chat prompt races a forced workstation lock.
        if(!arm_test(Clock::duration::zero())) return;
        if(test_audio_options_.session_volume==0) { stop_alarm(); show_error(L"Choose an alarm volume above 0 for this test."); return; }
        lock_test_active_=true;
        audio_status_=L"Test: 3 seconds of sound, then Windows locks. Listen without clicking; playback stops automatically.";
        lock_prelude_pending_=true;refresh();
        append_log(directory_,L"LOCK TEST: user initiated; 3 s audible prelude, lock, alarm in 10 s, 20 s sample");
    }
    bool read_fields(Config& candidate) {
        wchar_t text[16]{}; GetWindowTextW(control(Time),text,16);
        const std::wstring time=text;
        if(time.size()!=5 || time[2]!=L':' || !iswdigit(time[0]) || !iswdigit(time[1]) || !iswdigit(time[3]) || !iswdigit(time[4])) {
            show_error(L"Enter a time in HH:MM format (for example, 06:00)."); return false;
        }
        const int hour=(time[0]-L'0')*10+time[1]-L'0', minute=(time[3]-L'0')*10+time[4]-L'0';
        if(hour>23 || minute>59) { show_error(L"Invalid time: use 00:00 through 23:59."); return false; }
        candidate.hour=hour; candidate.minute=minute;
        wchar_t volume_text[8]{}; GetWindowTextW(control(SessionVolume),volume_text,8);
        wchar_t* volume_end=nullptr;
        const auto volume=wcstol(volume_text,&volume_end,10);
        if(!volume_text[0] || *volume_end || volume<0 || volume>100) { show_error(L"Alarm volume: 0 to 100%."); return false; }
        candidate.session_volume=static_cast<float>(volume)/100.f;
        candidate.enabled=SendMessageW(control(Enable),BM_GETCHECK,0,0)==BST_CHECKED;
        candidate.startup=SendMessageW(control(Startup),BM_GETCHECK,0,0)==BST_CHECKED;
        candidate.override_endpoint=true;
        candidate.endpoint_volume=1.f;
        const auto selected=SendMessageW(control(Device),CB_GETCURSEL,0,0);
        candidate.device_id=selected>0 && static_cast<std::size_t>(selected)<=devices_.size() ? devices_[selected-1].id : L"";
        return true;
    }
    bool save_fields() {
        if(!config_writable_) { show_error(L"Unreadable configuration: the original file is preserved."); return false; }
        Config next=config_;
        if(!read_fields(next)) return false;
        std::wstring error;
        const bool startup_changed=next.startup!=config_.startup;
        if(startup_changed && !set_current_user_startup(executable_path(),next.startup,error)) {show_error(error);return false;}
        if(!save_config(directory_/L"config.ini",next,error)) {
            if(startup_changed) {std::wstring rollback;set_current_user_startup(executable_path(),config_.startup,rollback);if(!rollback.empty()) append_log(directory_,L"Startup rollback: "+rollback);}
            show_error(error); return false;
        }
        const bool alarm_changed=next.enabled!=config_.enabled || next.hour!=config_.hour || next.minute!=config_.minute;
        config_=std::move(next);
        stop_alarm();
        append_log(directory_,L"Configuration saved");
        audio_status_=L"Preferences saved.";
        if(alarm_changed) notify_armed();
        refresh();return true;
    }
    void choose_file() {
        wchar_t file[32768]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=window_; ofn.lpstrFile=file; ofn.nMaxFile=32768;
        ofn.lpstrFilter=L"Audio WAV (PCM / IEEE float)\0*.wav\0\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&ofn)) return;
        if(!config_writable_) { show_error(L"Unreadable configuration: file preserved."); return; }
        Config next=config_; next.wav_path=file;
        std::wstring error;
        if(!save_config(directory_/L"config.ini",next,error)) { show_error(error); return; }
        config_=std::move(next); set_text(FileLabel,L"Sound: " + std::filesystem::path(file).filename().wstring());
        // Stop an existing cycle so one alarm never mixes devices/files or snapshots.
        stop_alarm();
    }
    void choose_built_in() {
        if(!config_writable_) return;
        Config next=config_;next.wav_path.clear();std::wstring error;
        if(!save_config(directory_/L"config.ini",next,error)) {show_error(error);return;}
        config_=std::move(next);set_text(FileLabel,L"Sound: built-in tone");stop_alarm();
    }
    void show_error(const std::wstring& error) { if(integration_) fail(error); else MessageBoxW(window_,error.c_str(),L"ALARM",MB_OK|MB_ICONERROR); }
    void fail(const std::wstring& error) {
        append_log(directory_,L"ERROR: " + error);
        schedule_status_=error; exit_code_=1;
        if(integration_) {
            std::ofstream out(directory_/L"integration.txt",std::ios::binary); out << "FAIL " << utf8(error) << '\n';
            DestroyWindow(window_);
        } else { stop_audio(); cycle_.stop(); applied_=Cycle::State::Idle; scheduled_.reset(); timer_.cancel(); SetThreadExecutionState(ES_CONTINUOUS); refresh(); }
    }
    void finish_integration() {
        const std::vector<Cycle::State> expected={Cycle::State::Ringing,Cycle::State::Silent,Cycle::State::Ringing,Cycle::State::Silent};
        if(integration_states_!=expected) { fail(L"Integration: unexpected cycle sequence"); return; }
        if(integration_audio_starts_!=2||!audio_.status().finished){fail(L"Integration: asynchronous audio start/cleanup sequence incomplete");return;}
        stop_alarm(false);
        if(cycle_.active() || audio_.playing()) { fail(L"Integration: STOP did not idle"); return; }
        SendMessageW(control(Save),BM_CLICK,0,0);
        if(!config_.enabled || !scheduled_) {fail(L"Integration: one-click activation did not persist/arm");return;}
        SendMessageW(control(Save),BM_CLICK,0,0);
        if(config_.enabled || scheduled_) {fail(L"Integration: one-click deactivation did not cancel");return;}
        const auto original_hour=config_.hour,original_minute=config_.minute;
        edit_time();set_text(TimeInput,L"07:35");
        MSG enter{};enter.hwnd=control(TimeInput);enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        if(!(SendMessageW(control(TimeInput),WM_GETDLGCODE,VK_RETURN,reinterpret_cast<LPARAM>(&enter))&DLGC_WANTMESSAGE)) {fail(L"Integration: time editor does not request Enter");return;}
        SendMessageW(control(TimeInput),WM_KEYDOWN,VK_RETURN,0);
        if(editing_time_ || config_.hour!=7 || config_.minute!=35) {fail(L"Integration: Enter did not save edited time");return;}
        edit_time();set_text(TimeInput,L"08:40");
        MSG escape{};escape.hwnd=control(TimeInput);escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;
        if(!(SendMessageW(control(TimeInput),WM_GETDLGCODE,VK_ESCAPE,reinterpret_cast<LPARAM>(&escape))&DLGC_WANTMESSAGE)) {fail(L"Integration: time editor does not request Escape");return;}
        SendMessageW(control(TimeInput),WM_KEYDOWN,VK_ESCAPE,0);
        if(editing_time_ || config_.hour!=7 || config_.minute!=35) {fail(L"Integration: Escape changed saved time");return;}
        wchar_t original[16]{};swprintf_s(original,L"%02d:%02d",original_hour,original_minute);set_text(Time,original);
        if(!save_fields()) {fail(L"Integration: restoring test configuration failed");return;}
        Config restored{}; std::wstring error;
        if(load_config(directory_/L"config.ini",restored,error)!=ConfigLoadResult::Loaded || restored.hour!=config_.hour || restored.minute!=config_.minute) {
            fail(L"Integration: persistent config round-trip failed: " + error); return;
        }
        std::ofstream out(directory_/L"integration.txt",std::ios::binary);
        out << "PASS real Windows timer + HWND message loop + WASAPI\nPASS closing UI keeps engine alive\nPASS RINGING -> SILENT -> RINGING -> SILENT -> STOP\nPASS audio stopped during silence and STOP\nPASS one-click daily activation/deactivation persists and arms/cancels timer\nPASS time editor requests Enter/Escape and saves/cancels changes\nPASS configuration read after save\nAccelerated test uses 3s ringing / 2s silent; normal mode uses 300s / 600s.\nPhysical sleep/hibernate/lock test remains pending.\n";
        out.close();
        if(!out) { fail(L"Integration: report write failed"); return; }
        integration_finished_=true;
        DestroyWindow(window_);
    }
    void shutdown() {
        if(shutting_down_) return;
        shutting_down_=true;audio_notice_->window=nullptr;audio_.set_state_callback({});
        cancel_button_gesture();
        timer_.cancel(); audio_.stop(); cycle_.stop(); SetThreadExecutionState(ES_CONTINUOUS);
        KillTimer(window_,audio_status_timer);
        KillTimer(window_,animation_timer);KillTimer(window_,preview_timer);animation_running_=false;
        tray_.remove();glass_.release_visuals();
        if(!integration_ && notifications_.available()) notifications_.clear();
        if(power_notify_) { UnregisterSuspendResumeNotification(power_notify_); power_notify_=nullptr; }
        if(display_notify_) { UnregisterPowerSettingNotification(display_notify_); display_notify_=nullptr; }
        if(session_registered_ && window_) { WTSUnRegisterSessionNotification(window_); session_registered_=false; }
        append_log(directory_,L"Engine exited; wake timer cancelled. " + audio_.error());
    }
    LRESULT message(UINT msg,WPARAM wparam,LPARAM lparam) {
        if(open_message_ && msg==open_message_) {if(wparam==1)DestroyWindow(window_);else show_window();return 0;}
        if(!integration_ && msg==tray_.taskbar_created_message()) {
            std::wstring error;if(!tray_.recreate(error)) {system_integration_status_=L"Icon unavailable: "+error;show_window();} else refresh();return 0;
        }
        if(msg==Tray::callback_message) {
            std::wstring error;const auto action=tray_.handle_callback(wparam,lparam,error);
            switch(action) {
                case TrayAction::Open:show_window();break;
                case TrayAction::ToggleEnabled:toggle_alarm();break;
                case TrayAction::Stop:stop_alarm();break;
                case TrayAction::Quit:DestroyWindow(window_);break;
                case TrayAction::None:break;
            }
            if(!error.empty()) append_log(directory_,L"Tray: "+error);
            return 0;
        }
        if(msg==Notifications::status_message) {notification_status();refresh();return 0;}
        switch(msg) {
        case WM_CREATE: initialize(); return 0;
        case WM_ERASEBKGND:return 1;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(window_,&point);
            RECT client{};GetClientRect(window_,&client);const auto dpi=GetDpiForWindow(window_);
            const int edge=MulDiv(7,dpi,96);
            if(!IsZoomed(window_)) {
                const bool left=point.x<edge,right=point.x>=client.right-edge,top=point.y<edge,bottom=point.y>=client.bottom-edge;
                if(top&&left)return HTTOPLEFT;
                if(top&&right)return HTTOPRIGHT;
                if(bottom&&left)return HTBOTTOMLEFT;
                if(bottom&&right)return HTBOTTOMRIGHT;
                if(left)return HTLEFT;
                if(right)return HTRIGHT;
                if(top)return HTTOP;
                if(bottom)return HTBOTTOM;
            }
            if(point.y<MulDiv(44,dpi,96)&&point.x<client.right-MulDiv(130,dpi,96))return HTCAPTION;
            return HTCLIENT;
        }
        case WM_ACTIVATE: if(LOWORD(wparam)==WA_INACTIVE)cancel_button_gesture();invalidate();return 0;
        case WM_PAINT: {
            PAINTSTRUCT painting{};BeginPaint(window_,&painting);
            if(visible_graphics()&&!glass_.render(window_,visual_state(),GetDpiForWindow(window_))&&!render_failure_reported_) {
                render_failure_reported_=true;
                append_log(directory_,L"Graphic initialization failed: "+glass_.error());
                PostMessageW(window_,WM_APP+41,0,0);
            }
            EndPaint(window_,&painting);return 0;
        }
        case WM_APP+41:
            show_error(L"Graphics rendering failed: "+glass_.error()+L". The engine is still running; close and reopen ALARM to retry.");
            return 0;
        case WM_APP+42:
            if(device_open_){const auto selection=SendMessageW(control(DeviceList),LB_GETCURSEL,0,0);if(selection!=LB_ERR)SendMessageW(control(Device),CB_SETCURSEL,selection,0);device_open_=false;layout();SetFocus(control(Device));}
            return 0;
        case WM_APP+43:
            audio_notice_->pending=false;audio_changed();return 0;
        case WM_DRAWITEM: return TRUE;
        case WM_MEASUREITEM: {
            const auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if(item&&(item->CtlType==ODT_COMBOBOX||item->CtlType==ODT_LISTBOX)){item->itemHeight=MulDiv(item->CtlType==ODT_LISTBOX?34:30,GetDpiForWindow(window_),96);return TRUE;}return FALSE;
        }
        case WM_MOUSEMOVE: {
            const auto scale=96.f/GetDpiForWindow(window_);if(update_pointer(GET_X_LPARAM(lparam)*scale,GET_Y_LPARAM(lparam)*scale))invalidate();return 0;
        }
        case WM_NCMOUSEMOVE: {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(window_,&point);const auto scale=96.f/GetDpiForWindow(window_);if(update_pointer(point.x*scale,point.y*scale))invalidate();break;
        }
        case WM_LBUTTONDOWN:
            keyboard_focus_=false;
            if(device_open_){device_open_=false;layout();}
            return 0;
        case WM_KEYDOWN:
            keyboard_focus_=true;
            if(wparam==VK_F12) {
                append_log(directory_,L"UI snapshot: frames="+std::to_wstring(glass_.frames())+L", generations="+std::to_wstring(glass_.generations())+L", lighting="+std::to_wstring(glass_.lighting_draws())+L", refraction="+std::to_wstring(glass_.refraction_draws())+L", visible="+std::to_wstring(visible_graphics())+L", timer="+std::to_wstring(animation_running_));
                return 0;
            }
            break;
        case WM_SIZE:
            cancel_button_gesture();
            if(wparam==SIZE_MINIMIZED) {KillTimer(window_,animation_timer);animation_running_=false;glass_.release_visuals();}
            else layout();
            update_countdown_timer();return 0;
        case WM_SHOWWINDOW:
            if(wparam) {render_failure_reported_=false;update_countdown_timer();if(cycle_.state()==Cycle::State::Ringing&&audio_.playing()) request_animation();}
            else {cancel_button_gesture();KillTimer(window_,countdown_timer);KillTimer(window_,animation_timer);animation_running_=false;glass_.release_visuals();}
            return DefWindowProcW(window_,msg,wparam,lparam);
        case WM_DPICHANGED: {
            cancel_button_gesture();
            update_fonts(HIWORD(wparam));
            const auto r=reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            auto m=reinterpret_cast<MINMAXINFO*>(lparam); const auto dpi=GetDpiForWindow(window_);
            m->ptMinTrackSize.x=MulDiv(580,dpi,96);m->ptMinTrackSize.y=MulDiv(720,dpi,96);
            MONITORINFO monitor{sizeof(monitor),{}, {},0};if(GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor)){m->ptMaxPosition.x=monitor.rcWork.left-monitor.rcMonitor.left;m->ptMaxPosition.y=monitor.rcWork.top-monitor.rcMonitor.top;m->ptMaxSize.x=monitor.rcWork.right-monitor.rcWork.left;m->ptMaxSize.y=monitor.rcWork.bottom-monitor.rcWork.top;}return 0;
        }
        case WM_TIMER:
            if(wparam==countdown_timer) refresh();
            else if(wparam==audio_status_timer)audio_changed();
            else if(wparam==animation_timer) {
                if(!visible_graphics() || !glass_.tick(cycle_.state()==Cycle::State::Ringing&&audio_.playing())) {KillTimer(window_,animation_timer);animation_running_=false;}
                invalidate();
            } else if(wparam==preview_timer) {
                if(preview_due_) {if(GetTickCount64()>=preview_due_)stop_alarm();}
                else if(preview_loading_due_&&GetTickCount64()>=preview_loading_due_) {
                    stop_alarm();audio_status_=L"Audio did not start within 30 seconds; preview cancelled.";refresh();
                }
            }
            else if(wparam==integration_timeout) fail(L"Integration test timed out");
            else if(wparam==audio_recovery_timer) {
                if(!audio_recovery_due_ || GetTickCount64()<audio_recovery_due_) return 0;
                audio_recovery_due_=0;
                KillTimer(window_,audio_recovery_timer);
                reconcile();
                if(!shutting_down_ && !system_suspended_ && cycle_.state()==Cycle::State::Ringing) {
                    stop_audio(); start_ringing_audio(true); refresh();
                }
            } else if(wparam==lock_prelude_timer) {
                if(!lock_test_active_ || !lock_prelude_due_ || GetTickCount64()<lock_prelude_due_) return 0;
                lock_prelude_due_=0;
                KillTimer(window_,lock_prelude_timer);
                if(!audio_.playing()) { stop_alarm(); show_error(L"The initial sound test failed; Windows lock cancelled."); return 0; }
                if(!arm_test(std::chrono::seconds(10))) return 0;
                lock_test_active_=true; lock_waiting_alarm_=true;
                append_log(directory_,L"LOCK TEST: requesting normal Windows lock; no unlock operation");
                if(!LockWorkStation()) { stop_alarm(); show_error(L"Windows refused the lock request."); return 0; }
                lock_timeout_due_=GetTickCount64()+120000;
                SetTimer(window_,lock_timeout_timer,120000,nullptr);
            } else if(wparam==lock_stop_timer) {
                if(!lock_test_active_ || !lock_sample_due_ || GetTickCount64()<lock_sample_due_) return 0;
                lock_sample_due_=0;
                KillTimer(window_,lock_stop_timer);
                if(lock_test_active_) {
                    append_log(directory_,L"LOCK TEST: auto STOP after 20 s sample");
                    stop_alarm();
                }
            } else if(wparam==lock_timeout_timer) {
                if(!lock_test_active_ || !lock_timeout_due_ || GetTickCount64()<lock_timeout_due_) return 0;
                KillTimer(window_,lock_timeout_timer); lock_timeout_due_=0;
                if(lock_waiting_alarm_) {
                    append_log(directory_,L"LOCK TEST: audio start timeout; test not validated");
                    stop_alarm(); audio_status_=L"Locked test: audio did not start. See alarm.log."; refresh();
                }
            }
            return 0;
        case WM_COMMAND:
            if(LOWORD(wparam)==DeviceList && HIWORD(wparam)==LBN_SELCHANGE) {
                invalidate();return 0;
            }
            if(HIWORD(wparam)==EN_CHANGE||HIWORD(wparam)==CBN_SELCHANGE){invalidate();return 0;}
            if(HIWORD(wparam)==BN_CLICKED) switch(LOWORD(wparam)) {
                case CaptionClose:SendMessageW(window_,WM_CLOSE,0,0);break;
                case CaptionMinimize:ShowWindow(window_,SW_MINIMIZE);break;
                case CaptionMaximize:ShowWindow(window_,IsZoomed(window_)?SW_RESTORE:SW_MAXIMIZE);break;
                case Save: toggle_alarm(); break;
                case Time:edit_time();break;
                case TimeDone:finish_time_edit();break;
                case Settings:if(finish_time_edit()) {settings_open_=true;diagnostics_open_=false;layout();}break;
                case Back:if(diagnostics_open_) diagnostics_open_=false;else settings_open_=false;layout();break;
                case SettingsApply:if(save_fields()) {settings_open_=false;diagnostics_open_=false;layout();}break;
                case Diagnostics:diagnostics_open_=true;layout();break;
                case VolumeDown:
                case VolumeUp: {
                    wchar_t text[8]{};GetWindowTextW(control(SessionVolume),text,8);const auto gain=std::clamp(_wtoi(text)+(LOWORD(wparam)==VolumeDown?-5:5),0,100);set_text(SessionVolume,std::to_wstring(gain));break;
                }
                case Browse: choose_file(); break;
                case BuiltIn:choose_built_in();break;
                case Test10: arm_test(std::chrono::seconds(10)); break;
                case Test30: arm_test(std::chrono::seconds(30)); break;
                case TestNow: begin_preview(); break;
                case Stop: stop_alarm(); break;
                case Quit: DestroyWindow(window_); break;
                case TestLocked: begin_locked_test(); break;
            }
            return 0;
        case WM_POWERBROADCAST:
            if(wparam==PBT_APMSUSPEND) {
                system_suspended_=true; KillTimer(window_,audio_recovery_timer); audio_recovery_due_=0;
                cancel_button_gesture();KillTimer(window_,animation_timer);KillTimer(window_,countdown_timer);animation_running_=false;glass_.release_visuals();
                append_log(directory_,L"System suspending"); stop_audio(); applied_=Cycle::State::Idle; SetThreadExecutionState(ES_CONTINUOUS);
            } else if(wparam==PBT_APMRESUMEAUTOMATIC || wparam==PBT_APMRESUMESUSPEND) {
                system_suspended_=false;
                append_log(directory_,L"System resumed"); reconcile(); schedule_audio_recovery();
            } else if(wparam==PBT_POWERSETTINGCHANGE) {
                const auto setting=reinterpret_cast<POWERBROADCAST_SETTING*>(lparam);
                if(setting && IsEqualGUID(setting->PowerSetting,GUID_CONSOLE_DISPLAY_STATE) && setting->DataLength>=sizeof(DWORD)) {
                    DWORD value=0; std::memcpy(&value,setting->Data,sizeof(value));
                    if(value!=display_state_) {
                        display_state_=value;
                        append_log(directory_,L"Console display state: " + std::to_wstring(value));
                        if(value==0){cancel_button_gesture();KillTimer(window_,animation_timer);KillTimer(window_,countdown_timer);animation_running_=false;glass_.release_visuals();}
                        else {if(value==1)schedule_audio_recovery();refresh();}
                    }
                }
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if(wparam==WTS_SESSION_LOCK) {
                session_locked_=true;append_log(directory_,L"Session locked (no secure desktop interaction)");
                cancel_button_gesture();KillTimer(window_,animation_timer);KillTimer(window_,countdown_timer);animation_running_=false;glass_.release_visuals();
                if(config_.enabled && !cycle_.active()) notify_armed();
            }
            else if(wparam==WTS_SESSION_UNLOCK) { session_locked_=false; append_log(directory_,L"Session unlocked"); reconcile(); }
            return 0;
        case WM_SETTINGCHANGE:
            if(wparam==SPI_SETCLIENTAREAANIMATION||wparam==SPI_SETHIGHCONTRAST){glass_.settings_changed();glass_.configure(window_);invalidate();}
            return 0;
        case WM_TIMECHANGE:
            append_log(directory_,L"Clock changed");
            reconcile();
            if(!cycle_.active() && !temporary_) { schedule_daily(); refresh(); }
            return 0;
        case WM_CLOSE:
            append_log(directory_,L"Window hidden; engine remains active");
            ShowWindow(window_,integration_ || tray_.available() ? SW_HIDE : SW_MINIMIZE); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if(wparam) DestroyWindow(window_); return 0;
        case WM_DESTROY: shutdown(); KillTimer(window_,countdown_timer); KillTimer(window_,integration_timeout); KillTimer(window_,audio_recovery_timer); PostQuitMessage(exit_code_); return 0;
        }
        return DefWindowProcW(window_,msg,wparam,lparam);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argc=0; LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::filesystem::path directory;
    bool integration=false,wake_test=false,background=false,quit=false;
    for(int i=1;i<argc;++i) {
        const std::wstring arg=args[i];
        if(arg==L"--data-dir" && i+1<argc) directory=args[++i];
        else if(arg==L"--integration-test") integration=true;
        else if(arg==L"--wake-test") wake_test=true;
        else if(arg==L"--background") background=true;
        else if(arg==L"--open") background=false;
        else if(arg==L"--quit") quit=true;
        else { LocalFree(args); MessageBoxW(nullptr,L"Usage: ALARM [--data-dir directory] [--background | --open | --quit | --integration-test | --wake-test]",L"ALARM",MB_OK); return 2; }
    }
    if(args) LocalFree(args);
    if((integration || wake_test) && directory.empty()) return 2;
    if(integration && wake_test) return 2;
    if(quit&&(integration||wake_test))return 2;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    HANDLE instance_mutex=nullptr;
    int result=1;
    try {
        if(directory.empty()) directory=config_directory();
        directory=std::filesystem::absolute(directory).lexically_normal();
        auto mutex_path=directory.wstring();
        std::transform(mutex_path.begin(),mutex_path.end(),mutex_path.begin(),[](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const auto mutex_name=L"Local\\ALARM_MVP_" + std::to_wstring(std::hash<std::wstring>{}(mutex_path));
        const UINT open_message=RegisterWindowMessageW(mutex_name.c_str());
        instance_mutex=CreateMutexW(nullptr,FALSE,mutex_name.c_str());
        if(!instance_mutex) throw std::runtime_error("Cannot create instance mutex");
        if(GetLastError()==ERROR_ALREADY_EXISTS) {
            if(quit&&open_message)PostMessageW(HWND_BROADCAST,open_message,1,0);
            else if(!integration && !background && open_message) PostMessageW(HWND_BROADCAST,open_message,0,0);
            result=integration?2:0;
        } else if(quit) {result=0;} else {
            App app(directory,integration,wake_test,background,open_message);
            if(!app.create(instance)) throw std::runtime_error("Cannot create ALARM window");
            HANDLE timer=app.timer_handle();
            bool running=true;
            while(running) {
                // No polling: Windows wakes this thread only for a deadline or message.
                const DWORD wait=MsgWaitForMultipleObjectsEx(1,&timer,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(wait==WAIT_OBJECT_0) app.timer_signaled();
                else if(wait==WAIT_FAILED) throw std::runtime_error("Message wait failed");
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) { running=false; break; }
                    app.observe_keyboard(message);
                    if(!IsDialogMessageW(app.window(),&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
                }
            }
            result=app.exit_code();
        }
    } catch(const std::exception& error) {
        const std::string narrow=error.what();
        const std::wstring message(narrow.begin(),narrow.end());
        if(!integration) MessageBoxW(nullptr,message.c_str(),L"ALARM — error",MB_OK|MB_ICONERROR);
        result=1;
    }
    if(instance_mutex) CloseHandle(instance_mutex);
    if(SUCCEEDED(com)) CoUninitialize();
    return result;
}
