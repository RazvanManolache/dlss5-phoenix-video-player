#include "PhoenixMediaJob.h"
#include "nlohmann/json.hpp"
#include <mfapi.h>
#include <fstream>
#include <iostream>
#include <map>
namespace {
std::stop_source cancellation;
BOOL WINAPI ConsoleControl(DWORD event){
    if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT)return FALSE;
    cancellation.request_stop();return TRUE;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==2&&std::wstring_view(argv[1])==L"--help"){
            std::cout<<"phoenix-media --recipe recipe.json --input clip.mkv --output new.mkv --cache folder [--helpers folder] [--width pixels --height pixels]\n"
                <<"Native resumable SDR video processing. Output is video-only; complete segments are cached and history is replayed on resume.\n";return 0;}
        std::map<std::wstring,std::wstring> options;
        for(int i=1;i<argc;i+=2){if(i+1>=argc||!options.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("Expected distinct option/value pairs");}
        for(const auto& [key,value]:options){(void)value;if(key!=L"--recipe"&&key!=L"--input"&&key!=L"--output"&&key!=L"--cache"&&key!=L"--helpers"&&key!=L"--width"&&key!=L"--height")throw std::runtime_error("Unknown option");}
        PhoenixMediaJob job;job.source=options.at(L"--input");job.output=options.at(L"--output");job.cacheRoot=options.at(L"--cache");
        if(options.contains(L"--helpers"))job.helpers=options.at(L"--helpers");
        if(options.contains(L"--width"))job.outputWidth=std::stoul(options.at(L"--width"));
        if(options.contains(L"--height"))job.outputHeight=std::stoul(options.at(L"--height"));
        const std::filesystem::path recipe=options.at(L"--recipe");
        if(!recipe.is_absolute()||!std::filesystem::is_regular_file(recipe)||std::filesystem::file_size(recipe)>262144)throw std::runtime_error("Recipe must be an absolute path to a file below 256 KiB");
        std::ifstream file(recipe,std::ios::binary);job.recipe=nlohmann::json::parse(file).dump();
        if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))throw std::runtime_error("COM initialization failed");
        if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();throw std::runtime_error("Media Foundation initialization failed");}
        struct Cleanup{~Cleanup(){MFShutdown();CoUninitialize();}} cleanup;
        SetConsoleCtrlHandler(ConsoleControl,TRUE);
        const auto result=RunPhoenixMediaJob(job,cancellation.get_token(),[](uint64_t frames,uint64_t){if(frames%120==0)std::cerr<<"Processed "<<frames<<" frames\n";});
        SetConsoleCtrlHandler(ConsoleControl,FALSE);
        const auto receipt=result.receipt.u8string();
        std::cout<<nlohmann::json{{"ok",result.ok},{"cancelled",result.cancelled},{"frames",result.frames},
            {"reused_frames",result.reusedFrames},{"history_frames_replayed",result.replayedFrames},
            {"receipt",std::string(receipt.begin(),receipt.end())}}.dump()<<'\n';
        if(!result.ok)std::wcerr<<result.detail<<'\n';return result.ok?0:1;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
