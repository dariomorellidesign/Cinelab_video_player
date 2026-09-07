#include "SubtitlePlayer.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include "Log.h"
#endif

// STEP 04G-2 progressive subtitle publication

namespace {
std::wstring WidenUtf8(const std::string&s){
#ifdef _WIN32
    if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);if(n<=0)return std::wstring(s.begin(),s.end());std::wstring w(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),w.data(),n);return w;
#else
    return std::wstring(s.begin(),s.end());
#endif
}
std::wstring StripTags(std::wstring s){std::wstring o;bool tag=false;for(wchar_t c:s){if(c==L'<'){tag=true;continue;}if(c==L'>'){tag=false;continue;}if(!tag)o.push_back(c);}return o;}
bool ParseTime(const std::string&s,double&v){int h=0,m=0;double sec=0.0;char c1=0,c2=0;std::string t=s;std::replace(t.begin(),t.end(),',','.');std::istringstream in(t);if(!(in>>h>>c1>>m>>c2>>sec)||c1!=':'||c2!=':')return false;v=double(h)*3600.0+double(m)*60.0+sec;return std::isfinite(v)&&v>=0.0;}

void NormalizeSrtNewlines(std::string& s){
    if(s.find('\r')==std::string::npos)return;
    std::string out;out.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]!='\r'){out.push_back(s[i]);continue;}
        if(i+1<s.size()){
            if(s[i+1]=='\n'){out.push_back('\n');++i;}
            else out.push_back('\n');
        }else{
            // Keep a trailing CR until the next chunk arrives; it may be the first
            // half of a CRLF pair split across ReadFile calls.
            out.push_back('\r');
        }
    }
    s.swap(out);
}

#ifdef _WIN32
std::wstring Q(const std::wstring&s){return L"\""+s+L"\"";}
std::wstring FindFFmpeg(){namespace fs=std::filesystem;wchar_t p[32768]{};if(GetModuleFileNameW(nullptr,p,DWORD(std::size(p)))){fs::path b=fs::path(p).parent_path();const fs::path c[]={b/L"ffmpeg.exe",b/L"ffmpeg"/L"bin"/L"ffmpeg.exe",b.parent_path()/L"ffmpeg"/L"bin"/L"ffmpeg.exe"};for(const auto&x:c){std::error_code ec;if(fs::is_regular_file(x,ec))return x.wstring();}}wchar_t f[32768]{};DWORD n=SearchPathW(nullptr,L"ffmpeg.exe",nullptr,DWORD(std::size(f)),f,nullptr);return(n&&n<std::size(f))?std::wstring(f):std::wstring();}
#endif
}

bool ParseSrtSubtitleText(const std::string& utf8,std::vector<SubtitleCue>& cues){
    cues.clear();std::string normalized=utf8;for(size_t p=0;(p=normalized.find("\r\n",p))!=std::string::npos;)normalized.replace(p,2,"\n");std::istringstream in(normalized);std::string line;
    while(std::getline(in,line)){
        if(line.empty())continue;std::string timeLine=line;if(timeLine.find("-->")==std::string::npos){if(!std::getline(in,timeLine))break;}const auto arrow=timeLine.find("-->");if(arrow==std::string::npos)continue;std::string a=timeLine.substr(0,arrow),b=timeLine.substr(arrow+3);auto trim=[](std::string x){while(!x.empty()&&(x.front()==' '||x.front()=='\t'))x.erase(x.begin());while(!x.empty()&&(x.back()==' '||x.back()=='\t'||x.back()=='\r'))x.pop_back();return x;};double s=0,e=0;if(!ParseTime(trim(a),s)||!ParseTime(trim(b),e)||e<s)continue;std::string txt;while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())break;if(!txt.empty())txt+='\n';txt+=line;}SubtitleCue c;c.startSec=s;c.endSec=e;c.text=StripTags(WidenUtf8(txt));if(!c.text.empty())cues.push_back(std::move(c));
    }
    std::sort(cues.begin(),cues.end(),[](const SubtitleCue&a,const SubtitleCue&b){return a.startSec<b.startSec;});return !cues.empty();
}

size_t ParseAvailableSrtSubtitleText(std::string& pendingUtf8,std::vector<SubtitleCue>& published,bool flushTail){
    NormalizeSrtNewlines(pendingUtf8);
    size_t added=0;
    for(;;){
        const size_t sep=pendingUtf8.find("\n\n");
        if(sep==std::string::npos)break;
        const std::string block=pendingUtf8.substr(0,sep+2);
        pendingUtf8.erase(0,sep+2);
        std::vector<SubtitleCue> cues;
        if(ParseSrtSubtitleText(block,cues)){
            added+=cues.size();
            published.insert(published.end(),std::make_move_iterator(cues.begin()),std::make_move_iterator(cues.end()));
        }
    }
    if(flushTail){
        NormalizeSrtNewlines(pendingUtf8);
        if(!pendingUtf8.empty()){
            std::vector<SubtitleCue> cues;
            if(ParseSrtSubtitleText(pendingUtf8,cues)){
                added+=cues.size();
                published.insert(published.end(),std::make_move_iterator(cues.begin()),std::make_move_iterator(cues.end()));
            }
            pendingUtf8.clear();
        }
    }
    return added;
}

std::wstring SubtitleTextAt(const std::vector<SubtitleCue>& cues,double t){
    if(cues.empty()||!std::isfinite(t))return{};auto it=std::upper_bound(cues.begin(),cues.end(),t,[](double x,const SubtitleCue&c){return x<c.startSec;});std::wstring out;size_t begin=it==cues.begin()?0:size_t((it-cues.begin())-1);while(begin>0&&cues[begin-1].endSec>=t)--begin;for(size_t i=begin;i<cues.size()&&cues[i].startSec<=t;++i){if(t>=cues[i].startSec&&t<=cues[i].endSec){if(!out.empty())out+=L"\n";out+=cues[i].text;}}return out;
}

struct SubtitlePlayer::State {
    mutable std::mutex mutex;
    std::vector<SubtitleCue> cues;
    uint64_t generation=0;
    bool loading=false;
    bool loaded=false;
    int streamIndex=-1;
#ifdef _WIN32
    HANDLE process=nullptr; // worker owns CloseHandle; cancellation only terminates under mutex
#endif
};

SubtitlePlayer::SubtitlePlayer():m_state(std::make_shared<State>()){}
SubtitlePlayer::~SubtitlePlayer(){Clear();}

void SubtitlePlayer::Clear(){
    if(!m_state)return;
    std::lock_guard<std::mutex> lock(m_state->mutex);
    ++m_state->generation;
#ifdef _WIN32
    // Terminate while holding the state mutex so the worker cannot clear+close the
    // same HANDLE between our load and TerminateProcess call. This remains non-blocking.
    if(m_state->process)TerminateProcess(m_state->process,0);
#endif
    m_state->cues.clear();m_state->loading=false;m_state->loaded=false;m_state->streamIndex=-1;
}

bool SubtitlePlayer::Loaded()const{if(!m_state)return false;std::lock_guard<std::mutex> lock(m_state->mutex);return m_state->loaded;}
bool SubtitlePlayer::Loading()const{if(!m_state)return false;std::lock_guard<std::mutex> lock(m_state->mutex);return m_state->loading;}
size_t SubtitlePlayer::CueCount()const{if(!m_state)return 0;std::lock_guard<std::mutex> lock(m_state->mutex);return m_state->cues.size();}
std::wstring SubtitlePlayer::TextAt(double seconds)const{if(!m_state)return{};std::lock_guard<std::mutex> lock(m_state->mutex);return SubtitleTextAt(m_state->cues,seconds);}

void SubtitlePlayer::LoadAsync(const std::wstring& mediaPath,int idx){
    if(!m_state)return;
    uint64_t generation=0;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
#ifdef _WIN32
        if(m_state->process)TerminateProcess(m_state->process,0);
#endif
        generation=++m_state->generation;
        m_state->cues.clear();m_state->loaded=false;m_state->loading=(idx>=0);m_state->streamIndex=idx;
    }
    if(idx<0)return;

    const auto state=m_state;
    std::thread([state,mediaPath,idx,generation](){
#ifdef _WIN32
        const auto ffmpeg=FindFFmpeg();
        if(ffmpeg.empty()){
            std::lock_guard<std::mutex> lock(state->mutex);if(state->generation==generation){state->loading=false;state->loaded=false;}LOG("[Subtitles Progressive] ffmpeg.exe not found");return;
        }

        SECURITY_ATTRIBUTES sa{};sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
        HANDLE rp=nullptr,wp=nullptr;
        if(!CreatePipe(&rp,&wp,&sa,1024*1024)){
            std::lock_guard<std::mutex> lock(state->mutex);if(state->generation==generation){state->loading=false;state->loaded=false;}LOG("[Subtitles Progressive] CreatePipe failed winerr="<<GetLastError());return;
        }
        SetHandleInformation(rp,HANDLE_FLAG_INHERIT,0);
        HANDLE nul=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(nul==INVALID_HANDLE_VALUE)nul=nullptr;
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=nul;si.hStdOutput=wp;si.hStdError=nul;
        std::wostringstream args;
        args<<L"-hide_banner -loglevel error -nostdin -i "<<Q(mediaPath)
            <<L" -map 0:"<<idx
            <<L" -vn -an -dn -c:s srt -flush_packets 1 -f srt pipe:1";
        std::wstring cmd=Q(ffmpeg)+L" "+args.str();std::vector<wchar_t>b(cmd.begin(),cmd.end());b.push_back(L'\0');PROCESS_INFORMATION pi{};
        BOOL ok=CreateProcessW(ffmpeg.c_str(),b.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
        CloseHandle(wp);if(nul)CloseHandle(nul);
        if(!ok){CloseHandle(rp);std::lock_guard<std::mutex> lock(state->mutex);if(state->generation==generation){state->loading=false;state->loaded=false;}LOG("[Subtitles Progressive] CreateProcess failed winerr="<<GetLastError());return;}
        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if(state->generation!=generation){TerminateProcess(pi.hProcess,0);CloseHandle(rp);CloseHandle(pi.hProcess);return;}
            state->process=pi.hProcess;
        }
        LOG("[Subtitles Progressive] extraction started stream="<<idx<<"; cues will publish before EOF");

        std::string pending;pending.reserve(32768);
        char tmp[8192];bool readOk=true;size_t bytesRead=0;bool loggedFirst=false;
        constexpr size_t MaxSubtitleBytes=64u*1024u*1024u;

        auto publish=[&](std::vector<SubtitleCue>& delta)->bool{
            if(delta.empty())return true;
            size_t count=0;double firstSec=0.0,lastSec=0.0;bool firstPublication=false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if(state->generation!=generation)return false;
                firstPublication=state->cues.empty();
                state->cues.insert(state->cues.end(),std::make_move_iterator(delta.begin()),std::make_move_iterator(delta.end()));
                if(!std::is_sorted(state->cues.begin(),state->cues.end(),[](const SubtitleCue&a,const SubtitleCue&b){return a.startSec<b.startSec;}))
                    std::sort(state->cues.begin(),state->cues.end(),[](const SubtitleCue&a,const SubtitleCue&b){return a.startSec<b.startSec;});
                state->loaded=!state->cues.empty();
                count=state->cues.size();
                if(count){firstSec=state->cues.front().startSec;lastSec=state->cues.back().endSec;}
            }
            if(firstPublication&&!loggedFirst){loggedFirst=true;LOG("[Subtitles Progressive] first cue available stream="<<idx<<" cues="<<count<<" first="<<firstSec<<"s last="<<lastSec<<"s");}
            else if(count>0 && (count%100u)<delta.size())LOG("[Subtitles Progressive] published stream="<<idx<<" cues="<<count<<" last="<<lastSec<<"s");
            return true;
        };

        for(;;){
            DWORD got=0;
            if(!ReadFile(rp,tmp,sizeof(tmp),&got,nullptr)||got==0)break;
            bytesRead+=got;
            if(bytesRead>MaxSubtitleBytes){readOk=false;LOG("[Subtitles Progressive] extraction exceeded 64 MiB safety cap stream="<<idx);TerminateProcess(pi.hProcess,0);break;}
            pending.append(tmp,tmp+got);
            std::vector<SubtitleCue> delta;
            ParseAvailableSrtSubtitleText(pending,delta,false);
            if(!publish(delta)){TerminateProcess(pi.hProcess,0);break;}
        }

        std::vector<SubtitleCue> tail;
        ParseAvailableSrtSubtitleText(pending,tail,true);
        publish(tail);

        CloseHandle(rp);WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
        size_t count=0;double firstSec=0.0,lastSec=0.0;bool current=false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if(state->process==pi.hProcess)state->process=nullptr;
            current=(state->generation==generation);
            if(current){
                state->loading=false;
                state->loaded=!state->cues.empty();
                count=state->cues.size();
                if(count){firstSec=state->cues.front().startSec;lastSec=state->cues.back().endSec;}
            }
        }
        CloseHandle(pi.hProcess);
        if(!current)return;
        if(readOk&&code==0){
            LOG("[Subtitles Progressive] complete stream="<<idx<<" cues="<<count<<" first="<<firstSec<<"s last="<<lastSec<<"s");
        }else{
            LOG("[Subtitles Progressive] extraction ended stream="<<idx<<" exitCode="<<code<<" cues="<<count);
        }
#else
        (void)mediaPath;(void)idx;
        std::lock_guard<std::mutex> lock(state->mutex);if(state->generation==generation){state->loading=false;state->loaded=false;}
#endif
    }).detach();
}
