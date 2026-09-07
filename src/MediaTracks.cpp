#include "MediaTracks.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include "Log.h"
#endif

namespace {
std::wstring WidenUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);
    if(n<=0) return std::wstring(s.begin(),s.end());
    std::wstring w(size_t(n),L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),w.data(),n);
    return w;
#else
    return std::wstring(s.begin(),s.end());
#endif
}
std::string Trim(std::string s){while(!s.empty()&&(s.back()=='\r'||s.back()=='\n'||s.back()==' '||s.back()=='\t'))s.pop_back();size_t p=0;while(p<s.size()&&(s[p]==' '||s[p]=='\t'))++p;return s.substr(p);}
std::wstring Lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (c>=L'A'&&c<=L'Z')?wchar_t(c-L'A'+L'a'):c;});return s;}
std::wstring NiceCodec(std::wstring c){if(c.empty())return L"unknown";std::replace(c.begin(),c.end(),L'_',L' ');return c;}
#ifdef _WIN32
std::wstring Quote(const std::wstring&s){return L"\""+s+L"\"";}
std::wstring FindTool(const wchar_t* exeName){
    namespace fs=std::filesystem;wchar_t modulePath[32768]{};
    if(GetModuleFileNameW(nullptr,modulePath,DWORD(std::size(modulePath)))){
        const fs::path base=fs::path(modulePath).parent_path();
        const fs::path cands[]={base/exeName,base/L"ffmpeg"/exeName,base/L"ffmpeg"/L"bin"/exeName,base.parent_path()/L"ffmpeg"/L"bin"/exeName};
        for(const auto&p:cands){std::error_code ec;if(fs::is_regular_file(p,ec))return p.wstring();}
    }
    wchar_t found[32768]{};DWORD n=SearchPathW(nullptr,exeName,nullptr,DWORD(std::size(found)),found,nullptr);return(n&&n<std::size(found))?std::wstring(found):std::wstring();
}
bool RunCapture(const std::wstring&exe,const std::wstring&args,std::string&out){
    out.clear();if(exe.empty())return false;SECURITY_ATTRIBUTES sa{};sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;HANDLE rp=nullptr,wp=nullptr;if(!CreatePipe(&rp,&wp,&sa,0))return false;SetHandleInformation(rp,HANDLE_FLAG_INHERIT,0);
    HANDLE nul=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(nul==INVALID_HANDLE_VALUE)nul=nullptr;
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=nul;si.hStdOutput=wp;si.hStdError=nul;PROCESS_INFORMATION pi{};
    std::wstring cmd=Quote(exe)+L" "+args;std::vector<wchar_t>buf(cmd.begin(),cmd.end());buf.push_back(L'\0');BOOL ok=CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);CloseHandle(wp);if(nul)CloseHandle(nul);if(!ok){CloseHandle(rp);return false;}
    char tmp[8192];for(;;){DWORD got=0;if(!ReadFile(rp,tmp,sizeof(tmp),&got,nullptr)||got==0)break;out.append(tmp,tmp+got);}CloseHandle(rp);WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0;
}
#endif
}

bool MediaTrackCatalog::IsTextSubtitleCodec(const std::wstring& codec){
    const auto c=Lower(codec);return c==L"subrip"||c==L"srt"||c==L"ass"||c==L"ssa"||c==L"mov_text"||c==L"webvtt"||c==L"text"||c==L"ttml";
}
bool MediaTrackCatalog::IsBitmapSubtitleCodec(const std::wstring& codec){
    const auto c=Lower(codec);return c==L"hdmv_pgs_subtitle"||c==L"dvd_subtitle"||c==L"dvb_subtitle"||c==L"xsub";
}

std::vector<MediaTrackInfo> ParseFFprobeTrackDump(const std::string& text){
    std::vector<MediaTrackInfo> out;MediaTrackInfo cur{};bool in=false;std::string type;
    std::istringstream ss(text);std::string line;
    auto finish=[&](){if(!in)return;if(type=="audio"){cur.kind=MediaTrackKind::Audio;out.push_back(cur);}else if(type=="subtitle"){cur.kind=MediaTrackKind::Subtitle;cur.textSubtitleSupported=MediaTrackCatalog::IsTextSubtitleCodec(cur.codec);cur.bitmapSubtitle=MediaTrackCatalog::IsBitmapSubtitleCodec(cur.codec);out.push_back(cur);}cur=MediaTrackInfo{};type.clear();in=false;};
    while(std::getline(ss,line)){
        line=Trim(line);if(line=="[STREAM]"){finish();in=true;continue;}if(line=="[/STREAM]"){finish();continue;}if(!in)continue;const auto eq=line.find('=');if(eq==std::string::npos)continue;const std::string k=line.substr(0,eq),v=line.substr(eq+1);
        try{if(k=="index")cur.streamIndex=std::stoi(v);else if(k=="codec_type")type=v;else if(k=="codec_name")cur.codec=WidenUtf8(v);else if(k=="channels")cur.channels=std::stoi(v);else if(k=="channel_layout")cur.channelLayout=WidenUtf8(v);else if(k=="TAG:language")cur.language=WidenUtf8(v);else if(k=="TAG:title")cur.title=WidenUtf8(v);else if(k=="DISPOSITION:default")cur.defaultDisposition=(v=="1");else if(k=="DISPOSITION:forced")cur.forcedDisposition=(v=="1");}catch(...){ }
    }
    finish();return out;
}

void MediaTrackCatalog::Clear(){m_audio.clear();m_subtitles.clear();}
const MediaTrackInfo* MediaTrackCatalog::FindAudio(int i)const{for(const auto&t:m_audio)if(t.streamIndex==i)return&t;return nullptr;}
const MediaTrackInfo* MediaTrackCatalog::FindSubtitle(int i)const{for(const auto&t:m_subtitles)if(t.streamIndex==i)return&t;return nullptr;}
int MediaTrackCatalog::DefaultAudioStream()const{for(const auto&t:m_audio)if(t.defaultDisposition)return t.streamIndex;return m_audio.empty()?-1:m_audio.front().streamIndex;}
std::wstring MediaTrackCatalog::ShortLabel(const MediaTrackInfo&t){std::wstring s=!t.language.empty()?t.language:L"und";if(!t.title.empty())s+=L":"+t.title;s+=L"/"+NiceCodec(t.codec);return s;}
std::wstring MediaTrackCatalog::MenuLabel(const MediaTrackInfo&t){
    std::wstring s;if(!t.title.empty())s=t.title;else if(!t.language.empty())s=t.language;else s=L"Track "+std::to_wstring(t.streamIndex);if(!t.language.empty()&&t.title.find(t.language)==std::wstring::npos)s+=L" | "+t.language;s+=L" | "+NiceCodec(t.codec);
    if(t.kind==MediaTrackKind::Audio){if(!t.channelLayout.empty())s+=L" | "+t.channelLayout;else if(t.channels>0)s+=L" | "+std::to_wstring(t.channels)+L" ch";if(t.defaultDisposition)s+=L" | default";}
    else{if(t.forcedDisposition)s+=L" | forced";if(t.bitmapSubtitle)s+=L" | bitmap (unsupported)";else if(!t.textSubtitleSupported)s+=L" | unsupported";}
    return s;
}

bool MediaTrackCatalog::Probe(const std::wstring& mediaPath){
    Clear();
#ifdef _WIN32
    const std::wstring ffprobe=FindTool(L"ffprobe.exe");if(ffprobe.empty()){LOG("[Tracks] ffprobe.exe not found");return false;}
    const std::wstring args=L"-v error -show_entries stream=index,codec_type,codec_name,channels,channel_layout:stream_tags=language,title:stream_disposition=default,forced -of default=noprint_wrappers=0:nokey=0 "+Quote(mediaPath);
    std::string dump;if(!RunCapture(ffprobe,args,dump)){LOG("[Tracks] ffprobe stream enumeration failed");return false;}auto all=ParseFFprobeTrackDump(dump);for(auto&t:all){if(t.kind==MediaTrackKind::Audio)m_audio.push_back(std::move(t));else m_subtitles.push_back(std::move(t));}
    LOG("[Tracks] audio="<<m_audio.size()<<" subtitles="<<m_subtitles.size());return true;
#else
    (void)mediaPath;return false;
#endif
}
