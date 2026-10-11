// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "notifications.h"
#include <roapi.h>
#include <winstring.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <iterator>

namespace alarmapp {
namespace {
using Microsoft::WRL::ComPtr;
namespace Toast = ABI::Windows::UI::Notifications;
namespace Xml = ABI::Windows::Data::Xml::Dom;
constexpr wchar_t identity[] = L"Ibrahim.ALARM.Desktop";
constexpr wchar_t uri[] = L"ibrahim-alarm://open";
constexpr wchar_t protocol_key[] = L"Software\\Classes\\ibrahim-alarm";
// A stub CLSID deliberately has no COM server. Microsoft supports this option
// for unpackaged ToastGeneric notifications when every activation is protocol.
constexpr GUID stub_clsid{0xca528276,0x90a5,0x4e7b,{0xb0,0x84,0x4b,0x9d,0x2d,0xfb,0x3c,0x11}};

std::wstring hresult_text(HRESULT hr) {
    std::wostringstream stream;
    stream << L"HRESULT 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill(L'0') << static_cast<unsigned long>(hr);
    return stream.str();
}
struct HString {
    HSTRING value = nullptr;
    HRESULT result;
    explicit HString(const std::wstring& text)
        : result(WindowsCreateString(text.data(),static_cast<UINT32>(text.size()),&value)) {}
    ~HString() { WindowsDeleteString(value); }
    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;
};
template<class T> HRESULT factory(const wchar_t* name, ComPtr<T>& result) {
    HString class_name(name);
    if(FAILED(class_name.result)) return class_name.result;
    return RoGetActivationFactory(class_name.value,__uuidof(T),reinterpret_cast<void**>(result.GetAddressOf()));
}
std::wstring xml_text(const std::wstring& text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    // XML 1.0 control characters are never valid in a text node.
    for(wchar_t c:text) {
        if(c < 0x20 && c!=L'\t' && c!=L'\n' && c!=L'\r') continue;
        switch(c) {
        case L'&': escaped+=L"&amp;"; break;
        case L'<': escaped+=L"&lt;"; break;
        case L'>': escaped+=L"&gt;"; break;
        case L'\"': escaped+=L"&quot;"; break;
        case L'\'': escaped+=L"&apos;"; break;
        default: escaped+=c; break;
        }
    }
    return escaped;
}
LONG set_string(HKEY key,const wchar_t* name,const std::wstring& value) {
    return RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
}
HRESULT register_protocol(const std::filesystem::path& executable) {
    HKEY key=nullptr;
    DWORD disposition=0;
    LONG code=RegCreateKeyExW(HKEY_CURRENT_USER,protocol_key,0,nullptr,0,
                             KEY_SET_VALUE|KEY_QUERY_VALUE,nullptr,&key,&disposition);
    if(code!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(code);
    if(disposition==REG_OPENED_EXISTING_KEY) {
        wchar_t owner[128]{}; DWORD bytes=sizeof(owner),type=0;
        code=RegQueryValueExW(key,L"ALARMIdentity",nullptr,&type,reinterpret_cast<BYTE*>(owner),&bytes);
        owner[127]=L'\0';
        if(code!=ERROR_SUCCESS || type!=REG_SZ || std::wstring(owner)!=identity) {
            RegCloseKey(key);
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }
    code=set_string(key,L"ALARMIdentity",identity);
    if(code==ERROR_SUCCESS) code=set_string(key,nullptr,L"URL:ALARM Protocol");
    if(code==ERROR_SUCCESS) code=set_string(key,L"URL Protocol",L"");
    RegCloseKey(key);
    if(code!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(code);
    const std::wstring command=L"\""+executable.wstring()+L"\" --open";
    const std::wstring command_key=std::wstring(protocol_key)+L"\\shell\\open\\command";
    code=RegCreateKeyExW(HKEY_CURRENT_USER,command_key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(code==ERROR_SUCCESS) {
        code=set_string(key,nullptr,command);
        RegCloseKey(key);
    }
    if(code!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(code);
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    return S_OK;
}
HRESULT register_identity() {
    const std::wstring subkey=L"Software\\Classes\\AppUserModelId\\"+std::wstring(identity);
    HKEY key=nullptr; DWORD disposition=0;
    LONG code=RegCreateKeyExW(HKEY_CURRENT_USER,subkey.c_str(),0,nullptr,0,
                             KEY_SET_VALUE|KEY_QUERY_VALUE,nullptr,&key,&disposition);
    if(code!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(code);
    if(disposition==REG_OPENED_EXISTING_KEY) {
        wchar_t owner[128]{}; DWORD bytes=sizeof(owner),type=0;
        code=RegQueryValueExW(key,L"ALARMIdentity",nullptr,&type,reinterpret_cast<BYTE*>(owner),&bytes);
        owner[127]=L'\0';
        if(code!=ERROR_SUCCESS || type!=REG_SZ || std::wstring(owner)!=identity) {
            RegCloseKey(key); return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }
    code=set_string(key,L"ALARMIdentity",identity);
    constexpr wchar_t display_name[]=L"ALARM";
    if(code==ERROR_SUCCESS)
        code=RegSetValueExW(key,L"DisplayName",0,REG_EXPAND_SZ,reinterpret_cast<const BYTE*>(display_name),sizeof(display_name));
    wchar_t clsid[40]{};
    if(!StringFromGUID2(stub_clsid,clsid,40)) code=ERROR_INVALID_DATA;
    if(code==ERROR_SUCCESS) code=set_string(key,L"CustomActivator",clsid);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(code);
}
HRESULT remove_owned_legacy_shortcut(const std::filesystem::path& path) {
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) return S_OK;
    ComPtr<IShellLinkW> link;
    HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()));
    ComPtr<IPersistFile> file;
    ComPtr<IPropertyStore> properties;
    if(SUCCEEDED(hr)) hr=link.As(&file);
    if(SUCCEEDED(hr)) hr=file->Load(path.c_str(),STGM_READ);
    if(SUCCEEDED(hr)) hr=link.As(&properties);
    // Unreadable or foreign shortcuts are preserved. Only a successfully read
    // AppUserModelID matching this application establishes ownership.
    if(FAILED(hr)) return S_FALSE;
    PROPVARIANT existing{};
    hr=properties->GetValue(PKEY_AppUserModel_ID,&existing);
    const bool ours=SUCCEEDED(hr) && existing.vt==VT_LPWSTR && existing.pwszVal &&
                    std::wstring(existing.pwszVal)==identity;
    PropVariantClear(&existing);
    if(!ours) return S_FALSE;
    properties.Reset(); file.Reset(); link.Reset();
    if(!DeleteFileW(path.c_str())) {
        const DWORD code=GetLastError();
        return code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(code);
    }
    SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW,path.c_str(),nullptr);
    return S_OK;
}
HRESULT register_shortcut_at(const std::filesystem::path& executable,const std::filesystem::path& directory) {
    const auto path=directory/L"ALARM.lnk";
    ComPtr<IShellLinkW> link;
    HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()));
    if(FAILED(hr)) return hr;
    ComPtr<IPropertyStore> properties;
    ComPtr<IPersistFile> file;
    hr=link.As(&properties);
    if(SUCCEEDED(hr)) hr=link.As(&file);
    if(FAILED(hr)) return hr;
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) {
        hr=file->Load(path.c_str(),STGM_READWRITE);
        if(FAILED(hr)) return hr;
        PROPVARIANT existing{};
        hr=properties->GetValue(PKEY_AppUserModel_ID,&existing);
        const bool ours=SUCCEEDED(hr) && existing.vt==VT_LPWSTR && existing.pwszVal &&
                        std::wstring(existing.pwszVal)==identity;
        const bool unclaimed=SUCCEEDED(hr) && (existing.vt==VT_EMPTY ||
                            (existing.vt==VT_LPWSTR && existing.pwszVal && !existing.pwszVal[0]));
        PropVariantClear(&existing);
        if(!ours) {
            // The per-user installer creates an ordinary link before the app
            // can stamp its notification identity. Adopt only an unclaimed
            // link to this exact file; foreign identities/targets stay intact.
            wchar_t target[32768]{};
            const HRESULT path_result=link->GetPath(target,static_cast<int>(std::size(target)),nullptr,SLGP_RAWPATH);
            std::error_code error;
            if(!unclaimed || FAILED(path_result) || !target[0] ||
               !std::filesystem::equivalent(std::filesystem::path(target),executable,error) || error)
                return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }
    hr=link->SetPath(executable.c_str());
    if(SUCCEEDED(hr)) hr=link->SetArguments(L"--open");
    if(SUCCEEDED(hr)) hr=link->SetDescription(L"Open ALARM");
    if(SUCCEEDED(hr)) hr=link->SetWorkingDirectory(executable.parent_path().c_str());
    if(SUCCEEDED(hr)) hr=link->SetIconLocation(executable.c_str(),0);
    PROPVARIANT value{};
    if(SUCCEEDED(hr)) hr=InitPropVariantFromString(identity,&value);
    if(SUCCEEDED(hr)) hr=properties->SetValue(PKEY_AppUserModel_ID,value);
    PropVariantClear(&value);
    if(SUCCEEDED(hr)) hr=InitPropVariantFromCLSID(stub_clsid,&value);
    if(SUCCEEDED(hr)) hr=properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID,value);
    PropVariantClear(&value);
    if(SUCCEEDED(hr)) hr=properties->Commit();
    if(SUCCEEDED(hr)) hr=file->Save(path.c_str(),TRUE);
    if(SUCCEEDED(hr)) {
        SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW,path.c_str(),nullptr);
        hr=remove_owned_legacy_shortcut(directory/L"ALARM Notifications.lnk");
    }
    return hr;
}
HRESULT register_shortcut(const std::filesystem::path& executable) {
    PWSTR programs=nullptr;
    const HRESULT hr=SHGetKnownFolderPath(FOLDERID_Programs,KF_FLAG_CREATE,nullptr,&programs);
    if(FAILED(hr)) return hr;
    const auto directory=std::filesystem::path(programs);
    CoTaskMemFree(programs);
    return register_shortcut_at(executable,directory);
}
struct Status {
    mutable std::mutex mutex;
    std::wstring error;
    HWND owner=nullptr;
    void set(std::wstring value) {
        HWND notify;
        { std::lock_guard lock(mutex); if(error==value) return; error=std::move(value); notify=owner; }
        if(notify) PostMessageW(notify,Notifications::status_message,0,0);
    }
};
using FailedDelegate=ABI::Windows::Foundation::ITypedEventHandler<Toast::ToastNotification*,Toast::ToastFailedEventArgs*>;
class FailedHandler final : public FailedDelegate {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Status> status_;
public:
    explicit FailedHandler(std::shared_ptr<Status> status):status_(std::move(status)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(iid==IID_IUnknown || iid==__uuidof(FailedDelegate)) {
            *result=static_cast<FailedDelegate*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count=--references_;
        if(!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Toast::IToastNotification*,Toast::IToastFailedEventArgs* args) override {
        HRESULT reason=E_FAIL;
        if(args) args->get_ErrorCode(&reason);
        status_->set(L"Windows notification refused: "+hresult_text(reason));
        return S_OK;
    }
};
std::wstring setting_error(Toast::NotificationSetting setting) {
    switch(setting) {
    case Toast::NotificationSetting_DisabledForApplication: return L"ALARM notifications are disabled in Windows settings.";
    case Toast::NotificationSetting_DisabledForUser: return L"Windows notifications are disabled for this user.";
    case Toast::NotificationSetting_DisabledByGroupPolicy: return L"Notifications are blocked by a Windows policy.";
    case Toast::NotificationSetting_DisabledByManifest: return L"Windows notification identity is unavailable.";
    default: return L"Windows notifications are unavailable.";
    }
}
}

struct Notifications::Impl {
    DWORD thread=0;
    bool ro_initialized=false;
    bool ready=false;
    ComPtr<Toast::IToastNotifier> notifier;
    ComPtr<Toast::IToastNotificationFactory> toast_factory;
    ComPtr<Toast::IToastNotificationHistory> history;
    ComPtr<Toast::IToastNotification> current;
    EventRegistrationToken failed_token{};
    bool subscribed=false;
    std::shared_ptr<Status> status=std::make_shared<Status>();
    ~Impl() {
        { std::lock_guard lock(status->mutex); status->owner=nullptr; }
        release_toast();
        history.Reset(); toast_factory.Reset(); notifier.Reset();
        if(ro_initialized) RoUninitialize();
    }
    void release_toast() {
        if(current && subscribed) current->remove_Failed(failed_token);
        current.Reset(); subscribed=false;
    }
    bool fail(const wchar_t* action,HRESULT hr) {
        status->set(std::wstring(action)+L" : "+hresult_text(hr)); return false;
    }
    bool same_thread() {
        if(thread && thread!=GetCurrentThreadId()) {
            status->set(L"Notifications must be used on the thread that initializes them."); return false;
        }
        return true;
    }
    bool check_setting() {
        if(!notifier) { status->set(L"Windows notifications are not initialized."); return false; }
        Toast::NotificationSetting setting{};
        const HRESULT hr=notifier->get_Setting(&setting);
        // On a newly registered identity Windows can return ERROR_NOT_FOUND
        // until its first Show creates the notification-settings record. This
        // was reproduced by the native probe. Let Show perform that first
        // submission; every other settings failure remains an explicit error.
        if(hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return true;
        if(FAILED(hr)) return fail(L"Read notification settings",hr);
        if(setting!=Toast::NotificationSetting_Enabled) { status->set(setting_error(setting)); return false; }
        return true;
    }
};
Notifications::Notifications():impl_(std::make_unique<Impl>()) {}
Notifications::~Notifications()=default;
const wchar_t* Notifications::app_id() { return identity; }
const wchar_t* Notifications::protocol_uri() { return uri; }
bool Notifications::initialize(HWND owner,const std::filesystem::path& executable) {
    if(!impl_->same_thread()) return false;
    impl_=std::make_unique<Impl>();
    auto& p=*impl_; p.thread=GetCurrentThreadId(); p.status->owner=owner;
    const DWORD attributes=GetFileAttributesW(executable.c_str());
    if(!executable.is_absolute() || executable.wstring().find(L'\"')!=std::wstring::npos ||
       attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY))
        return p.fail(L"Stable notification executable path",E_INVALIDARG);
    HRESULT hr=RoInitialize(RO_INIT_SINGLETHREADED);
    if(FAILED(hr)) return p.fail(L"Initialize Windows Runtime",hr);
    p.ro_initialized=true;
    hr=SetCurrentProcessExplicitAppUserModelID(identity);
    if(SUCCEEDED(hr)) hr=register_shortcut(executable);
    if(SUCCEEDED(hr)) hr=register_protocol(executable);
    if(SUCCEEDED(hr)) hr=register_identity();
    if(FAILED(hr)) return p.fail(L"Register Windows notifications",hr);
    ComPtr<Toast::IToastNotificationManagerStatics> manager;
    hr=factory(L"Windows.UI.Notifications.ToastNotificationManager",manager);
    HString id(identity);
    if(SUCCEEDED(hr)) hr=id.result;
    if(SUCCEEDED(hr)) hr=manager->CreateToastNotifierWithId(id.value,p.notifier.GetAddressOf());
    if(SUCCEEDED(hr)) hr=factory(L"Windows.UI.Notifications.ToastNotification",p.toast_factory);
    ComPtr<Toast::IToastNotificationManagerStatics2> manager2;
    if(SUCCEEDED(hr)) hr=manager.As(&manager2);
    if(SUCCEEDED(hr)) hr=manager2->get_History(p.history.GetAddressOf());
    if(FAILED(hr)) return p.fail(L"Access Windows notification platform",hr);
    p.ready=true;
    if(!p.check_setting()) return false;
    p.status->set(L""); return true;
}
bool Notifications::show(const std::wstring& title,const std::wstring& body,bool alarm) {
    auto& p=*impl_;
    if(!p.same_thread() || !p.ready || !p.check_setting()) return false;
    if(!p.toast_factory || !p.history) return p.fail(L"Incomplete notification platform",E_UNEXPECTED);
    std::wstring xml=L"<toast activationType=\"protocol\" launch=\""+std::wstring(uri)+L"\"";
    if(alarm) xml+=L" scenario=\"alarm\"";
    xml+=L"><visual><binding template=\"ToastGeneric\"><text>"+xml_text(title)+
         L"</text><text>"+xml_text(body)+L"</text></binding></visual>";
    xml+=L"<actions><action content=\"Open ALARM\" arguments=\""+std::wstring(uri)+
         L"\" activationType=\"protocol\"/></actions><audio silent=\"true\"/></toast>";
    HString document_name(L"Windows.Data.Xml.Dom.XmlDocument");
    HString document_xml(xml);
    HRESULT hr=document_name.result;
    if(SUCCEEDED(hr)) hr=document_xml.result;
    ComPtr<IInspectable> activated;
    if(SUCCEEDED(hr)) hr=RoActivateInstance(document_name.value,activated.GetAddressOf());
    ComPtr<Xml::IXmlDocument> document;
    ComPtr<Xml::IXmlDocumentIO> io;
    if(SUCCEEDED(hr)) hr=activated.As(&document);
    if(SUCCEEDED(hr)) hr=activated.As(&io);
    if(SUCCEEDED(hr)) hr=io->LoadXml(document_xml.value);
    ComPtr<Toast::IToastNotification> notification;
    if(SUCCEEDED(hr)) hr=p.toast_factory->CreateToastNotification(document.Get(),notification.GetAddressOf());
    ComPtr<Toast::IToastNotification2> identifiers;
    if(SUCCEEDED(hr)) hr=notification.As(&identifiers);
    HString tag(L"state"),group(L"alarm");
    if(SUCCEEDED(hr)) hr=tag.result;
    if(SUCCEEDED(hr)) hr=group.result;
    if(SUCCEEDED(hr)) hr=identifiers->put_Tag(tag.value);
    if(SUCCEEDED(hr)) hr=identifiers->put_Group(group.value);
    if(FAILED(hr)) return p.fail(L"Build Windows notification",hr);
    // Keep exactly one state toast. Hide the previous banner before replacing
    // its tag, and retain this instance so STOP can also hide an alarm banner.
    if(p.current) p.notifier->Hide(p.current.Get());
    p.release_toast();
    p.current=notification;
    ComPtr<FailedDelegate> failed;
    failed.Attach(new FailedHandler(p.status));
    hr=p.current->add_Failed(failed.Get(),&p.failed_token);
    if(FAILED(hr)) { p.release_toast(); return p.fail(L"Track notification errors",hr); }
    p.subscribed=true;
    p.status->set(L"");
    hr=p.notifier->Show(p.current.Get());
    if(FAILED(hr)) return p.fail(L"Send Windows notification",hr);
    return true;
}
bool Notifications::clear() {
    auto& p=*impl_;
    if(!p.same_thread()) return false;
    if(!p.notifier || !p.history) return p.fail(L"Clear uninitialized notifications",E_UNEXPECTED);
    HRESULT hidden=S_OK;
    if(p.current) hidden=p.notifier->Hide(p.current.Get());
    p.release_toast();
    HString id(identity);
    HRESULT hr=id.result;
    if(SUCCEEDED(hr)) hr=p.history->ClearWithId(id.value);
    if(FAILED(hr)) return p.fail(L"Clear notification center",hr);
    if(FAILED(hidden)) return p.fail(L"Hide Windows notification banner",hidden);
    p.status->set(L""); return true;
}
bool Notifications::available() const { return impl_->same_thread() && impl_->ready && impl_->check_setting(); }
HRESULT Notifications::register_shortcut_for_testing(const std::filesystem::path& executable,const std::filesystem::path& directory) {
    return register_shortcut_at(executable,directory);
}
std::wstring Notifications::error() const {
    std::lock_guard lock(impl_->status->mutex); return impl_->status->error;
}
}
