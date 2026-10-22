#include "YouTubeResolver.h"
#include "VideoDecoder.h"
#include "nlohmann/json.hpp"
#include <mfapi.h>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
int wmain(int argc,wchar_t** argv){
    using Json=nlohmann::json;
    Json report{{"passed",false}};
    int outcome=1;
    try{
        if(argc!=2&&argc!=4)throw std::runtime_error("Usage: PhoenixYouTubeProbe <public YouTube URL> [frame output directory seek seconds]");
        const auto started=std::chrono::steady_clock::now();
        YouTubeResolver resolver;
        const auto result=resolver.Resolve(argv[1],argc==4?YouTubeSourceQuality::P1440:YouTubeSourceQuality::P1080,{});
        report["resolved"]=result.ok;report["selected_height"]=result.selectedHeight;
        report["duration_seconds"]=result.durationSeconds;
        report["resolve_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        report["error_code"]=int(result.error);
        if(!result.ok)throw std::runtime_error("Production YouTube resolver failed; see the probe log for the helper diagnostic");
        if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))throw std::runtime_error("COM initialization failed");
        if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();throw std::runtime_error("Media Foundation initialization failed");}
        struct Cleanup{~Cleanup(){MFShutdown();CoUninitialize();}} cleanup;
        VideoDecoder decoder;
        if(!decoder.Open(result.mediaUrl,MediaSourceKind::YouTube,{},false))throw std::runtime_error("Resolved stream could not be opened by the production decoder");
        if(argc==4&&!decoder.SeekSeconds(std::stod(argv[3])))throw std::runtime_error("Comparison frame seek failed");
        VideoFrame frame;
        if(decoder.ReadNextBlocking(frame)!=VideoReadResult::FrameReady)throw std::runtime_error("Resolved stream did not decode its first frame");
        report["first_frame_extent"]={decoder.Width(),decoder.Height()};
        report["first_frame_bytes"]=frame.bgra.size();report["first_frame_pts_100ns"]=frame.timestamp100ns;
        if(argc==4){
            const std::filesystem::path folder=argv[2];std::filesystem::create_directories(folder);
            std::ofstream file(folder/L"input.bgra",std::ios::binary);file.write(reinterpret_cast<const char*>(frame.bgra.data()),std::streamsize(frame.bgra.size()));
            if(!file)throw std::runtime_error("Cannot save comparison source frame");
        }
        report["total_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        report["passed"]=true;outcome=0;
    }catch(const std::exception& error){report["error"]=error.what();}
    // Signed media/audio URLs are deliberately absent from the public receipt.
    std::cout<<report.dump()<<'\n';return outcome;
}
