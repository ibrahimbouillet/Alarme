// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "widget_state.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace alarmapp;
namespace {
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions; if (!condition) throw std::runtime_error(message);
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    require(bool(stream),"Published state is readable");
    return {std::istreambuf_iterator<char>(stream),{}};
}

// Independent small JSON reader for the flat public contract. It consumes the
// entire document and decodes standard string escapes, including UTF-16 units.
class Json {
    std::string text_; std::size_t at_ = 0;
    char take() { if (at_ == text_.size()) throw std::runtime_error("Truncated JSON"); return text_[at_++]; }
    void space() { while (at_<text_.size() && (text_[at_]==' '||text_[at_]=='\n'||text_[at_]=='\r'||text_[at_]=='\t')) ++at_; }
    std::wstring string() {
        if (take()!='"') throw std::runtime_error("Expected JSON string");
        std::wstring value;
        for (;;) {
            const unsigned char c=take();
            if(c=='"') return value;
            if(c<32) throw std::runtime_error("Raw JSON control");
            if(c!='\\') {value.push_back(c);continue;}
            const char escape=take();
            switch(escape) {
            case '"':value+=L'"';break;case '\\':value+=L'\\';break;case '/':value+=L'/';break;
            case 'b':value+=L'\b';break;case 'f':value+=L'\f';break;case 'n':value+=L'\n';break;
            case 'r':value+=L'\r';break;case 't':value+=L'\t';break;
            case 'u': {
                unsigned unit=0;
                for(int i=0;i<4;++i) {const char h=take();const int n=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;if(n<0)throw std::runtime_error("Invalid JSON hex");unit=(unit<<4)|n;}
                value+=static_cast<wchar_t>(unit);break;
            }
            default:throw std::runtime_error("Invalid JSON escape");
            }
        }
    }
public:
    explicit Json(std::string text):text_(std::move(text)) {}
    std::map<std::wstring,std::wstring> parse() {
        std::map<std::wstring,std::wstring> fields;space();if(take()!='{')throw std::runtime_error("Expected JSON object");
        for (;;) {
            space();if(at_<text_.size()&&text_[at_]=='}') {++at_;break;}
            const auto key=string();space();if(take()!=':')throw std::runtime_error("Expected colon");space();
            std::wstring value;
            if(at_<text_.size()&&text_[at_]=='"') value=string();
            else {while(at_<text_.size()&&((text_[at_]>='a'&&text_[at_]<='z')||(text_[at_]>='0'&&text_[at_]<='9')))value+=take();if(value.empty())throw std::runtime_error("Expected scalar");}
            if(!fields.emplace(key,value).second)throw std::runtime_error("Duplicate field");
            space();const char separator=take();if(separator=='}')break;if(separator!=',')throw std::runtime_error("Expected separator");
        }
        space();if(at_!=text_.size())throw std::runtime_error("Trailing JSON");return fields;
    }
};

class Fixtures {
public:
    std::filesystem::path root;
    Fixtures() {
        const auto parent=std::filesystem::canonical(std::filesystem::temp_directory_path());
        for(unsigned i=0;i<32;++i) {
            root=parent/(L"alarm-widget-tests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(i));
            if(std::filesystem::create_directory(root))break;
            root.clear();
        }
        require(!root.empty()&&root.parent_path()==parent,"Unique checked temporary fixture directory");
    }
    void cleanup() {
        for(const auto& entry:std::filesystem::directory_iterator(root)) {
            require(entry.path().parent_path()==root,"Cleanup remains inside the fixture directory");
            require(!entry.is_directory(),"No unexpected nested fixture directory");
            require(std::filesystem::remove(entry.path()),"Fixture file cleanup succeeds");
        }
        require(std::filesystem::remove(root),"Fixture directory cleanup succeeds");root.clear();
    }
    ~Fixtures() {if(!root.empty()){std::error_code ec;for(const auto& entry:std::filesystem::directory_iterator(root,ec))std::filesystem::remove(entry.path(),ec);std::filesystem::remove(root,ec);}}
};

void tests(Fixtures& fixture) {
    WidgetState state;state.enabled=true;state.hour=23;state.minute=59;state.phase=WidgetPhase::Scheduled;
    state.process_id=GetCurrentProcessId();state.next_transition_utc=L"2026-10-01T04:30:00Z";state.updated_utc=L"2026-09-30T18:00:00Z";
    state.sound_label=L"Tone \"ALARM\" \\ \n\t";state.sound_label+=L'\0';state.sound_label+=L" end \u00e9 \U0001f514";
    const auto path=fixture.root/L"widget-state.json";std::wstring error;
    require(publish_widget_state(fixture.root,state,error)&&error.empty(),"Initial publication succeeds");
    const auto original=read(path);const auto json=Json(original).parse();
    require(json.size()==8,"JSON exposes exactly the eight public fields");
    require(json.at(L"enabled")==L"true"&&json.at(L"hour")==L"23"&&json.at(L"minute")==L"59","Numeric and boolean contract values roundtrip");
    require(json.at(L"phase")==L"scheduled"&&json.at(L"nextTransitionUtc")==state.next_transition_utc,"Phase and next UTC roundtrip");
    require(json.at(L"updatedUtc")==state.updated_utc&&json.at(L"processId")==std::to_wstring(state.process_id),"Metadata roundtrip");
    require(json.at(L"soundLabel")==state.sound_label,"Quotes, slash, controls, NUL, accent and emoji roundtrip");
    require(original.find('\0')==std::string::npos&&original.find("\\u0000")!=std::string::npos,"NUL is escaped, never emitted raw");
    const auto modified=std::filesystem::last_write_time(path);
    state.updated_utc=L"2026-09-30T18:01:00Z";
    HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held!=INVALID_HANDLE_VALUE,"Hold destination against replacement");
    require(publish_widget_state(fixture.root,state,error),"Timestamp-only update performs no replacement even while destination is locked");
    require(read(path)==original&&std::filesystem::last_write_time(path)==modified,"Unchanged payload preserves bytes and modification time");
    state.phase=WidgetPhase::Ringing;
    require(!publish_widget_state(fixture.root,state,error)&&!error.empty(),"Changed publication reports destination-sharing failure");
    require(read(path)==original,"Failed atomic replacement preserves the old complete JSON");
    require(CloseHandle(held)!=FALSE,"Close destination test lock");
    require(std::distance(std::filesystem::directory_iterator(fixture.root),std::filesystem::directory_iterator{})==1,"Failed replacement cleans its temporary sibling");
    require(publish_widget_state(fixture.root,state,error)&&error.empty(),"Changed payload publishes after lock release");
    require(Json(read(path)).parse().at(L"phase")==L"ringing","Changed phase is readable");
    for(const auto& [phase,name]:std::map<WidgetPhase,std::wstring>{{WidgetPhase::Idle,L"idle"},{WidgetPhase::Silent,L"silent"},{WidgetPhase::Error,L"error"}}) {
        state.phase=phase;require(publish_widget_state(fixture.root,state,error),"All fixed phases publish");
        require(Json(read(path)).parse().at(L"phase")==name,"Fixed phases remain distinct in JSON, including audio error");
    }
    const auto good=read(path);
    const auto reject=[&](WidgetState invalid){require(!publish_widget_state(fixture.root,invalid,error)&&!error.empty(),"Invalid payload is rejected");require(read(path)==good,"Invalid payload preserves prior state");};
    auto invalid=state;invalid.hour=24;reject(invalid);invalid=state;invalid.minute=-1;reject(invalid);
    invalid=state;invalid.phase=static_cast<WidgetPhase>(99);reject(invalid);
    invalid=state;invalid.updated_utc=L"2026-02-29T01:00:00Z";reject(invalid);
    invalid=state;invalid.next_transition_utc=L"2026-10-01T25:00:00Z";reject(invalid);
    invalid=state;invalid.updated_utc=L"2026-09-30T18:00:00+02:00";reject(invalid);
    invalid=state;invalid.sound_label.assign(161,L'a');reject(invalid);
    invalid=state;invalid.sound_label.assign(1,static_cast<wchar_t>(0xd800));reject(invalid);
    state.sound_label.assign(160,L'é');state.next_transition_utc.clear();state.updated_utc=L"2028-02-29T00:00:00Z";state.process_id=0;
    require(publish_widget_state(fixture.root,state,error),"Boundary label, leap day, empty deadline and automatic PID publish");
    require(Json(read(path)).parse().at(L"processId")==std::to_wstring(GetCurrentProcessId()),"Zero PID resolves to current process");
    state.updated_utc.clear();state.sound_label=L"Built-in ALARM tone";
    require(publish_widget_state(fixture.root,state,error),"Automatic UTC timestamp publishes");
    const auto now=Json(read(path)).parse().at(L"updatedUtc");require(now.size()==20&&now.back()==L'Z',"Automatic timestamp has fixed UTC form");
    require(widget_utc(std::chrono::system_clock::time_point{std::chrono::seconds{0}})==L"1970-01-01T00:00:00Z","UTC helper converts Unix epoch exactly");
    const auto blocked=fixture.root/L"blocked";{std::ofstream file(blocked);file<<"preserve";}
    require(!publish_widget_state(blocked,state,error)&&!error.empty(),"File cannot serve as destination directory");
    require(read(blocked)=="preserve","Destination directory failure preserves existing file");
    require(!publish_widget_state({},state,error)&&!error.empty(),"Empty destination is rejected");
    std::wstring nul_directory=fixture.root.wstring();nul_directory+=L'\0';nul_directory+=L"suffix";
    require(!publish_widget_state(std::filesystem::path(nul_directory),state,error),"Embedded NUL in destination is rejected");
}
}

int main() {
    try {Fixtures fixture;tests(fixture);fixture.cleanup();std::cout<<"PASS widget JSON/atomic publication: "<<assertions<<" assertions\n";return 0;}
    catch(const std::exception& error) {std::cerr<<"FAIL widget state: "<<error.what()<<'\n';return 1;}
}
