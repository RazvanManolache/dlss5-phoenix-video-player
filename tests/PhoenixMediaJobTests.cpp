#include "PhoenixMediaJob.h"
#include "NeuralCache.h"
#include "SynchronizedPlayback.h"
#include "NeuralSegmentIndex.h"
#include "nlohmann/json.hpp"
#include <mfapi.h>
#include <fstream>
#include <iostream>
#include <thread>
using Json=nlohmann::json;
namespace {
void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Json Read(const std::filesystem::path& path){std::ifstream input(path);return Json::parse(input);}
}
int wmain(int argc,wchar_t** argv){
    try{
        Check(argc==6||argc==7,"Usage: PhoenixMediaJobTests <DLL> <scratch> <helpers> <input> <expected frames> [recipe]");
        Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM initialization");
        Check(SUCCEEDED(MFStartup(MF_VERSION)),"Media Foundation initialization");
        struct Cleanup{~Cleanup(){MFShutdown();CoUninitialize();}} cleanup;
        const std::filesystem::path library=argv[1],scratch=argv[2];
        Json recipe;
        if(argc==7)recipe=Read(argv[6]);
        else {const auto library_text=library.u8string();recipe={{"schema","phoenix-media/1"},
            {"native_library",std::string(library_text.begin(),library_text.end())},{"native_library_sha256",*Sha256File(library)},
            {"runtime",{{"spatial_only",true},{"warp",true}}},{"pipeline",{{"PipelineMonitoring",0}}}};}
        PhoenixMediaJob job;job.source=argv[4];job.helpers=argv[3];job.cacheRoot=scratch/L"resumed";
        job.output=scratch/L"resumed.mkv";job.segmentFrames=2;job.recipe=recipe.dump();
        const auto expected=std::stoull(argv[5]);Check(expected>=4,"Need at least four test frames");
        std::stop_source cancellation;
        const auto interrupted=RunPhoenixMediaJob(job,cancellation.get_token(),[&](uint64_t frames,uint64_t){if(frames>=3)cancellation.request_stop();});
        Check(interrupted.cancelled&&!interrupted.ok&&!std::filesystem::exists(job.output),"Interrupted job published an output");
        std::filesystem::path manifest_path;
        for(const auto& directory:std::filesystem::directory_iterator(job.cacheRoot))manifest_path=directory.path()/L"manifest.json";
        auto pending=Read(manifest_path);Check(pending.at("segments").size()==1&&!pending.at("complete").get<bool>(),"Interrupted job did not retain exactly one complete segment");
        const auto abandoned=manifest_path.parent_path()/L"segment.tmp-interrupted.mkv";
        const auto unrelated=manifest_path.parent_path()/L"keep-user-file.txt";
        {std::ofstream file(abandoned);file<<"unfinished segment";}
        {std::ofstream file(unrelated);file<<"preserve";}
        pending["pending_file"]="segment.tmp-interrupted.mkv";
        {std::ofstream file(manifest_path);file<<pending.dump();}
        const auto segment=manifest_path.parent_path()/L"segment-0.mkv";
        const auto segment_hash=Sha256File(segment);
        const auto segment_time=std::filesystem::last_write_time(segment);
        const auto resumed=RunPhoenixMediaJob(job,{});Check(resumed.ok,"Resume failed");
        Check(resumed.frames==expected&&resumed.reusedFrames==2&&resumed.replayedFrames==2,"Resume skipped frames or failed to rebuild temporal history");
        Check(Sha256File(segment)==segment_hash&&std::filesystem::last_write_time(segment)==segment_time,"Resume rewrote a committed segment");
        Check(!std::filesystem::exists(abandoned)&&std::filesystem::exists(unrelated),"Interrupted staging cleanup did not preserve unrelated files");
        auto baseline=job;baseline.cacheRoot=scratch/L"baseline";baseline.output=scratch/L"baseline.mkv";
        const auto continuous=RunPhoenixMediaJob(baseline,{});Check(continuous.ok&&continuous.frames==expected,"Continuous export failed");
        auto reuse=job;reuse.output=scratch/L"cached.mkv";const auto reused=RunPhoenixMediaJob(reuse,{});
        Check(reused.ok&&reused.reusedFrames==expected&&reused.replayedFrames==0,"Complete cache failed reuse");
        const auto image_hash=Sha256File(job.output);const auto overwrite=RunPhoenixMediaJob(job,{});
        Check(!overwrite.ok&&Sha256File(job.output)==image_hash,"Existing output was overwritten");
        auto playback=job;playback.cacheRoot=scratch/L"playback";playback.output.clear();playback.cachedPlayback=true;playback.lookupOnly=true;
        const auto miss=RunPhoenixMediaJob(playback,{});
        Check(miss.ok&&miss.preparedOnly&&miss.payload.empty(),"Prepare-only cache miss rendered a video");
        std::vector<PhoenixMediaSegment> arrived;
        playback.segmentReady=[&](const PhoenixMediaSegment& segment){
            Check(std::filesystem::is_regular_file(segment.path)&&segment.frames>0&&segment.end100ns>segment.firstTimestamp100ns,"Segment published before completion");
            Check(segment.index==arrived.size(),"Live segment order changed");arrived.push_back(segment);
        };
        playback.lookupOnly=false;auto published=RunPhoenixMediaJob(playback,{});
        Check(published.ok&&!published.cacheHit&&published.frames==expected,"Native playback cache failed publication");
        Check(arrived.size()==expected/2&&arrived.front().firstFrame==0&&arrived.back().firstFrame+arrived.back().frames==expected,"Native segment timeline differs");
        const auto carrier_hash=Sha256File(published.payload);const auto carrier_time=std::filesystem::last_write_time(published.payload);
        arrived.clear();playback.lookupOnly=true;auto hit=RunPhoenixMediaJob(playback,{});
        Check(hit.ok&&hit.cacheHit&&!hit.preparedOnly&&hit.payload==published.payload&&hit.replayedFrames==0,
            "Prepared open did not reuse authenticated native carrier");
        Check(Sha256File(hit.payload)==carrier_hash&&std::filesystem::last_write_time(hit.payload)==carrier_time,"Cache lookup rewrote its carrier");
        Check(arrived.size()==expected/2,"Authenticated segments were not republished for live playback");
        auto live_index=std::make_shared<NeuralSegmentIndex>();
        for(const auto& published_segment:arrived){NeuralSegment item{};item.path=published_segment.path;item.runId=1;item.index=published_segment.index;item.firstFrameNumber=published_segment.firstFrame;
            item.firstTimestamp100ns=published_segment.firstTimestamp100ns;item.end100ns=published_segment.end100ns;item.frameCount=published_segment.frames;live_index->Append(std::move(item));}
        SynchronizedPlayback live_pair;
        Check(live_pair.OpenLive(job.source,live_index,{}, {},{},false,hit.width,hit.height),"Native live segments could not open at output geometry");
        auto live_result=SynchronizedReadResult::NotReady;const auto live_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(live_result==SynchronizedReadResult::NotReady&&std::chrono::steady_clock::now()<live_deadline){live_result=live_pair.ReadNextAvailable();if(live_result==SynchronizedReadResult::NotReady)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        Check(live_result==SynchronizedReadResult::PairReady&&live_pair.CurrentPair()&&live_pair.CurrentPair()->neural.bgra.size()==FrameBytes(live_pair.Layout(),hit.width,hit.height),"Native live comparison failed source/segment matching");
        live_pair.Close();
        SynchronizedPlayback pair;
        Check(pair.Open(job.source,hit.payload,{}, {},false,{},hit.width,hit.height),"Native cached source/carrier geometry did not synchronize");
        auto pair_result=SynchronizedReadResult::NotReady;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(pair_result==SynchronizedReadResult::NotReady&&std::chrono::steady_clock::now()<deadline){
            pair_result=pair.ReadNextAvailable();if(pair_result==SynchronizedReadResult::NotReady)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(pair_result==SynchronizedReadResult::PairReady&&pair.CurrentPair()&&
            pair.CurrentPair()->original.bgra.size()==FrameBytes(pair.Layout(),hit.width,hit.height)&&
            pair.CurrentPair()->neural.bgra.size()==pair.CurrentPair()->original.bgra.size(),"Native cached comparison produced unmatched frames");
        pair.Close();
        {std::ofstream corrupt(hit.payload,std::ios::binary|std::ios::app);corrupt<<"changed carrier";}
        arrived.clear();const auto corrupt_carrier_hash=Sha256File(hit.payload);const auto rejected_carrier=RunPhoenixMediaJob(playback,{});
        Check(!rejected_carrier.ok&&Sha256File(hit.payload)==corrupt_carrier_hash,"Modified native carrier was accepted or replaced");
        Check(phoenix_media_cache::Maintain(playback.cacheRoot,0,true).removed==0,"Cleanup removed an active native carrier");
        hit.playbackLease.reset();published.playbackLease.reset();
        Check(phoenix_media_cache::Maintain(playback.cacheRoot,0,true).removed==1,"Explicit cleanup did not retire an inactive native entry");
        {std::ofstream corrupt(segment,std::ios::binary|std::ios::app);corrupt<<"changed";}
        reuse.output=scratch/L"corrupt.mkv";const auto refused=RunPhoenixMediaJob(reuse,{});
        Check(!refused.ok&&!std::filesystem::exists(reuse.output),"Corrupted cache was accepted");
        const auto manifest=Read(resumed.receipt);
        std::cout<<Json{{"passed",true},{"frames",resumed.frames},{"resumed_frames_reused",resumed.reusedFrames},
            {"history_frames_replayed",resumed.replayedFrames},{"committed_segment_preserved",true},
            {"complete_cache_reused",true},{"corrupt_cache_refused",true},{"existing_output_preserved",true},{"interrupted_staging_recovered",true},
            {"native_playback_cache_reused",true},{"prepare_only_does_not_render",true},{"corrupt_carrier_preserved_and_refused",true},
            {"cached_pair_synchronized",true},{"output_extent",{hit.width,hit.height}},
            {"live_segments_authenticated_and_republished",true},{"live_pair_synchronized",true},{"active_cache_preserved",true},{"inactive_native_cache_cleared",true},
            {"backend",manifest.at("runtime").at("backend")},{"model",manifest.at("runtime").at("model_id")},
            {"weights_sha256",manifest.at("runtime").at("packed_weights_sha256")}}.dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
