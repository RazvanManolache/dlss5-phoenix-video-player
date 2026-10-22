#include "PhoenixMediaCache.h"
#include "NeuralCache.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <algorithm>
#include <atomic>

namespace phoenix_media_cache {
EntryGuard::EntryGuard(const std::filesystem::path& root){
    mutex.value=CreateMutexW(nullptr,FALSE,NeuralCacheRootLockName(root).c_str());
    if(!mutex.value){mutex.value=INVALID_HANDLE_VALUE;return;}
    const auto waited=WaitForSingleObject(mutex.value,250);held=waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED;
}
bool Key(const std::wstring& name){return name.size()==64&&std::all_of(name.begin(),name.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});}
bool RetiredName(const std::wstring& name){
    if(!name.starts_with(L"retired-")||name.size()<76||!Key(name.substr(8,64))||name[72]!=L'-')return false;
    const auto suffix=name.substr(73);const auto dash=suffix.find(L'-');
    return dash!=std::wstring::npos&&dash>0&&dash+1<suffix.size()&&
        std::all_of(suffix.begin(),suffix.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||c==L'-';})&&suffix.find(L'-',dash+1)==std::wstring::npos;
}
bool PlainPath(std::filesystem::path path){
    if(!path.is_absolute())return false;
    for(;!path.empty();){const auto a=GetFileAttributesW(path.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto parent=path.parent_path();if(parent==path)break;path=parent;}return true;
}
std::shared_ptr<Lease> Lease::Acquire(const std::filesystem::path& directory){
        EntryGuard guard(directory.parent_path());if(!guard.held)throw std::runtime_error("Native cache maintenance is busy");
        if(!PlainPath(directory)||!Key(directory.filename().wstring()))throw std::runtime_error("Invalid native cache lease path");
        const auto file=directory/L"playback.lock";
        if(!PlainPath(file))throw std::runtime_error("Native cache lease traverses a link");
        auto lease=std::make_shared<Lease>();
        lease->handle.value=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lease->handle.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Native cache entry is being retired");
        std::error_code error;std::filesystem::last_write_time(directory,std::filesystem::file_time_type::clock::now(),error);
        return lease;
    }
bool OwnedFiles(const std::filesystem::path& directory,std::vector<std::filesystem::path>& files){
    try{
        const auto manifest=directory/L"manifest.json";
        if(!PlainPath(manifest)||!std::filesystem::is_regular_file(manifest)||std::filesystem::file_size(manifest)>4*1024*1024)return false;
        std::ifstream input(manifest,std::ios::binary);const auto data=nlohmann::json::parse(input);
        if(data.at("schema")!="phoenix-media-job/1"||!data.at("segments").is_array()||data.at("segments").size()>100000)return false;
        std::vector<std::wstring> allowed{L"manifest.json",L"job.lock",L"playback.lock",L"render.mkv"};
        for(size_t i=0;i<data.at("segments").size();++i){
            const auto name=L"segment-"+std::to_wstring(i)+L".mkv";
            if(data["segments"][i].at("file").get<std::string>()!="segment-"+std::to_string(i)+".mkv")return false;
            allowed.push_back(name);
        }
        if(data.contains("pending_file")){
            const auto name=data.at("pending_file").get<std::string>();
            if(!name.starts_with("segment.tmp-")||!name.ends_with(".mkv")||name.size()>160||name.find_first_of("/\\:")!=std::string::npos)return false;
            allowed.emplace_back(name.begin(),name.end());
        }
        for(const auto& child:std::filesystem::directory_iterator(directory)){
            if(!PlainPath(child.path())||!child.is_regular_file()||std::find(allowed.begin(),allowed.end(),child.path().filename().wstring())==allowed.end())return false;
            files.push_back(child.path());
        }return true;
    }catch(...){return false;}
}
// No recursive deletion. Only a bounded manifest's own flat, plain files may
// be retired; links, unknown files and active jobs/readers preserve the entry.
Maintenance Maintain(const std::filesystem::path& root,uintmax_t freeFloor,bool clear){
    Maintenance result;
    if(!PlainPath(root)){++result.deferred;return result;}
    std::error_code error;if(!std::filesystem::exists(root,error)){const auto space=std::filesystem::space(root.parent_path(),error);result.floorMet=!error&&space.available>=freeFloor;return result;}
    struct Entry {std::filesystem::path path;std::filesystem::file_time_type used;bool retired;};std::vector<Entry> entries;
    for(std::filesystem::directory_iterator it(root,error),end;!error&&it!=end;it.increment(error)){
        const auto name=it->path().filename().wstring();const bool retired=RetiredName(name);
        if((!Key(name)&&!retired)||!PlainPath(it->path())||!it->is_directory(error))continue;
        const auto time=std::filesystem::last_write_time(it->path(),error);if(!error)entries.push_back({it->path(),time,retired});
    }
    if(error){++result.deferred;error.clear();}
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.retired!=b.retired?a.retired:a.used<b.used;});
    auto floor_met=[&]{const auto space=std::filesystem::space(root,error);return !error&&space.available>=freeFloor;};
    static std::atomic<unsigned> serial=0;
    for(const auto& entry:entries){
        if(!entry.retired&&!clear&&floor_met())break;
        EntryGuard guard(root);if(!guard.held){++result.deferred;continue;}
        if(entry.retired&&std::filesystem::is_empty(entry.path,error)){
            if(std::filesystem::remove(entry.path,error))++result.removed;else ++result.deferred;
            error.clear();continue;
        }
        Handle job,readers;
        job.value=CreateFileW((entry.path/L"job.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_DELETE,nullptr,entry.retired?OPEN_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(job.value==INVALID_HANDLE_VALUE){++result.deferred;continue;}
        readers.value=CreateFileW((entry.path/L"playback.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(readers.value==INVALID_HANDLE_VALUE){++result.deferred;continue;}
        std::vector<std::filesystem::path> files;
        if(!OwnedFiles(entry.path,files)){++result.deferred;continue;}
        uintmax_t bytes=0;for(const auto& file:files){bytes+=std::filesystem::file_size(file,error);if(error)break;}
        if(error){++result.deferred;error.clear();continue;}
        const auto retired=entry.retired?entry.path:root/(L"retired-"+entry.path.filename().wstring()+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial));
        // Windows refuses renaming a directory with our child handles open.
        // The root mutex now excludes all producer/reader lock acquisitions.
        job.close();readers.close();
        if(!entry.retired&&!MoveFileExW(entry.path.c_str(),retired.c_str(),MOVEFILE_WRITE_THROUGH)){++result.deferred;continue;}
        // Keep ownership evidence until payload deletion succeeds, so a later
        // pass can finish an antivirus/sharing failure without guessing names.
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.filename()!=L"manifest.json"&&b.filename()==L"manifest.json";});
        bool removed=true;for(const auto& file:files){std::filesystem::remove(retired/file.filename(),error);if(error){removed=false;error.clear();break;}}
        if(removed)removed=std::filesystem::remove(retired,error)&&!error;
        if(removed){++result.removed;result.freed+=bytes;}else ++result.deferred;
    }
    result.floorMet=floor_met();return result;
}
} // namespace phoenix_media_cache
