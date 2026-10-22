#include "PhoenixMediaJob.h"
#include "VideoDecoder.h"
#include "NeuralCache.h"
#include "AtomicFile.h"
#include "Utf8Text.h"
#include "phoenix_native.h"
#include "nlohmann/json.hpp"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <fstream>
#include <stdexcept>
#include <cmath>
#include <memory>
#include <array>
#include <algorithm>
#include <thread>
#include <chrono>
using Json=nlohmann::json;
using Microsoft::WRL::ComPtr;
namespace {
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct FileHandle {HANDLE value=INVALID_HANDLE_VALUE;~FileHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
void NoLinks(std::filesystem::path path){
    for(;!path.empty();){const auto attributes=GetFileAttributesW(path.c_str());
        Require(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Media cache/output paths cannot traverse links");
        const auto parent=path.parent_path();if(parent==path)break;path=parent;}
}
std::filesystem::path Utf8Path(const std::string& text){return std::filesystem::path(std::u8string(text.begin(),text.end()));}
Json Read(const std::filesystem::path& path){
    Require(std::filesystem::is_regular_file(path)&&std::filesystem::file_size(path)<=4*1024*1024,"Missing or oversized Phoenix media manifest");
    std::ifstream file(path,std::ios::binary);return Json::parse(file);
}
void Save(const std::filesystem::path& path,const Json& value){Require(bool(atomic_file::Replace(path,value.dump(2))),"Cannot publish Phoenix media manifest");}
std::string Hash(const std::filesystem::path& path,std::stop_token stop={}){
    const auto digest=Sha256File(path,stop);Require(bool(digest),"Cannot hash Phoenix media file");return *digest;
}
struct HostRuntime {
    HMODULE module=nullptr;PhoenixNativeSession* session=nullptr;
    decltype(&phoenix_native_create) create=nullptr;decltype(&phoenix_native_destroy) destroy=nullptr;
    decltype(&phoenix_native_frame_host) frame=nullptr;decltype(&phoenix_native_status) status=nullptr;
    decltype(&phoenix_native_set_frame_info) identity=nullptr;decltype(&phoenix_native_error) error=nullptr;
    decltype(&phoenix_native_device) device=nullptr;
    Json environment;
    ~HostRuntime(){if(session&&destroy&&destroy(session)!=0)module=nullptr;if(module)FreeLibrary(module);}
    template<class T>void Load(T& target,const char* name){target=reinterpret_cast<T>(GetProcAddress(module,name));Require(target!=nullptr,name);}
    Json Status(){const auto bytes=status(session,nullptr,0);Require(bytes>0&&bytes<=4*1024*1024,"Invalid Phoenix media status");
        std::string text(bytes,'\0');Require(status(session,text.data(),text.size())==bytes,"Phoenix status changed");return Json::parse(text.c_str());}
    HostRuntime(const Json& recipe,unsigned w,unsigned h,unsigned ow,unsigned oh){
        Require(recipe.at("schema")=="phoenix-media/1","Unsupported Phoenix media recipe");
        const auto library=Utf8Path(recipe.at("native_library").get<std::string>());
        Require(library.is_absolute()&&Hash(library)==recipe.at("native_library_sha256").get<std::string>(),"Phoenix native library checksum differs");
        module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(module!=nullptr,"Cannot load Phoenix native library");
        try{
            Load(create,"phoenix_native_create");Load(destroy,"phoenix_native_destroy");Load(frame,"phoenix_native_frame_host");
            Load(status,"phoenix_native_status");Load(identity,"phoenix_native_set_frame_info");Load(error,"phoenix_native_error");Load(device,"phoenix_native_device");
            auto config=recipe.at("runtime");config["width"]=w;config["height"]=h;config["output_width"]=ow;config["output_height"]=oh;
            config["pipeline"]=recipe.at("pipeline");config["engine_motion_valid"]=false;
            if(recipe.contains("tuning"))config["tuning"]=recipe.at("tuning");
            // Offline exports never adapt quality to their own completion speed.
            config["pipeline"]["PipelinePriority"]=0;config["offline"]=true;
            session=create(config.dump().c_str(),nullptr,nullptr);Require(session!=nullptr,error());
            ComPtr<ID3D12Device> gpu;Require(device(session,&gpu,nullptr)==0,error());
            const auto luid=gpu->GetAdapterLuid();ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
            Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))),"Cannot identify media GPU");
            DXGI_ADAPTER_DESC1 desc{};Require(SUCCEEDED(adapter->GetDesc1(&desc)),"Cannot identify media adapter");
            LARGE_INTEGER driver{};const bool driver_known=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));
            environment={{"vendor",desc.VendorId},{"device",desc.DeviceId},{"subsystem",desc.SubSysId},
                {"adapter_luid",{luid.LowPart,luid.HighPart}},{"driver",driver_known?Json(driver.QuadPart):Json(nullptr)}};
            environment["portable_neural_stages"]=Json::array();
            for(const auto& stage:Status().value("portable_neural_stages",Json::array())){
                Json stage_identity;
                for(const auto key:{"backend","device","runtime","model_id","model_architecture","model_resource_sha256","stage","width","height"})
                    if(stage.contains(key))stage_identity[key]=stage.at(key);
                environment["portable_neural_stages"].push_back(std::move(stage_identity));
            }
            // Processing may use an adapter other than the presentation GPU.
            // Bind cache entries to every installed adapter's driver identity,
            // avoiding unstable worker PIDs, allocator sizes and timing fields.
            environment["adapter_drivers"]=Json::array();
            for(unsigned index=0;;++index){
                ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(index,&candidate)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 details{};Require(SUCCEEDED(candidate->GetDesc1(&details)),"Cannot identify processing driver");
                LARGE_INTEGER version{};const auto known=SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice),&version));
                environment["adapter_drivers"].push_back({{"vendor",details.VendorId},{"device",details.DeviceId},
                    {"subsystem",details.SubSysId},{"driver",known?Json(version.QuadPart):Json(nullptr)}});
            }
        }catch(...){if(session&&destroy&&destroy(session)!=0)module=nullptr;if(module)FreeLibrary(module);module=nullptr;session=nullptr;throw;}
    }
};
}
PhoenixMediaJobResult RunPhoenixMediaJob(const PhoenixMediaJob& job,std::stop_token stop,
    const std::function<void(uint64_t,uint64_t)>& progress){
    PhoenixMediaJobResult result;
    try{
        Require(job.source.is_absolute()&&job.cacheRoot.is_absolute()&&(job.cachedPlayback||job.output.is_absolute()),"Media job paths must be absolute");
        NoLinks(job.cacheRoot);NoLinks(job.output);
        Require(job.cachedPlayback||!std::filesystem::exists(job.output),"Media output already exists; choose a new filename");
        Require(job.segmentFrames>=1&&job.segmentFrames<=3600&&job.range.start100ns>=0&&
            job.firstSegmentFrames<=3600&&
            (!job.range.end100ns||job.range.end100ns>job.range.start100ns),"Invalid media range or segment length");
        // Hold the exact source against writes/deletion for the entire job. A
        // decoder child may open a second read handle while this one is held.
        FileHandle source;source.value=CreateFileW(job.source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(source.value!=INVALID_HANDLE_VALUE,"Source is unavailable or being modified");
        const auto source_hash=Hash(job.source,stop);
        if(stop.stop_requested()){result.cancelled=true;return result;}
        VideoDecoder::Settings settings;settings.helperDirectory=job.helpers.wstring();
        VideoDecoder decoder(settings);
        Require(decoder.OpenSequential(job.source.wstring(),MediaSourceKind::LocalFile,stop,false),"Cannot decode Phoenix media source");
        Require(!decoder.IsStillImage()&&decoder.ConstantFrameRate(),"Phoenix resumable export currently requires constant-frame-rate video");
        const auto recipe=Json::parse(job.recipe);
        const double scale=recipe.value("output_scale",1.0);Require(std::isfinite(scale)&&scale>=1&&scale<=2,"Invalid media output scale");
        const unsigned w=decoder.Width(),h=decoder.Height(),ow=job.outputWidth?job.outputWidth:unsigned(std::lround(w*scale)),oh=job.outputHeight?job.outputHeight:unsigned(std::lround(h*scale));
        Require(w&&h&&ow&&oh&&ow<=16384&&oh<=16384,"Invalid media extent");
        result.width=ow;result.height=oh;
        HostRuntime runtime(recipe,w,h,ow,oh);
        const auto initial=runtime.Status();
        Json identity{{"revision","phoenix-media-job/1"},{"source_sha256",source_hash},{"recipe",recipe},
            {"weights_sha256",initial.at("packed_weights_sha256")},{"environment",runtime.environment},
            {"source_extent",{w,h}},{"output_extent",{ow,oh}},{"fps",utf8_text::FromWide(FrameRateText(decoder.FrameRate()))},
            {"range",{job.range.start100ns,job.range.end100ns}},{"segment_frames",job.segmentFrames},
            {"quality",std::string(EncoderQualityName(job.quality))},{"encoder","software H264 / FFV1 lossless rung"},
            {"capture","SDR BGRA8 proxy, no presentation overlays"}};
        if(job.firstSegmentFrames)identity["first_segment_frames"]=job.firstSegmentFrames;
        const auto key=Sha256Bytes(identity.dump());Require(bool(key),"Cannot identify media job");
        auto entry_guard=std::make_unique<phoenix_media_cache::EntryGuard>(job.cacheRoot);
        Require(entry_guard->held,"Native cache maintenance is busy");
        const auto directory=job.cacheRoot/Utf8Path(*key);NoLinks(directory);std::filesystem::create_directories(directory);
        NoLinks(directory/L"job.lock");
        FileHandle lock;lock.value=CreateFileW((directory/L"job.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(lock.value!=INVALID_HANDLE_VALUE,"This media job is already running");
        entry_guard.reset();
        const auto manifest_path=directory/L"manifest.json";
        NoLinks(manifest_path);
        Json manifest={{"schema","phoenix-media-job/1"},{"identity",identity},{"segments",Json::array()},{"complete",false}};
        if(std::filesystem::exists(manifest_path)){
            manifest=Read(manifest_path);Require(manifest.at("schema")=="phoenix-media-job/1"&&manifest.at("identity")==identity,"Media cache identity differs");
        }
        if(manifest.contains("pending_file")){
            const auto pending=manifest.at("pending_file").get<std::string>();
            Require(pending.starts_with("segment.tmp-")&&pending.ends_with(".mkv")&&pending.size()<160&&
                pending.find_first_of("/\\:")==std::string::npos,"Invalid interrupted media staging path");
            const auto path=directory/Utf8Path(pending);NoLinks(path);
            if(std::filesystem::exists(path))Require(DeleteFileW(path.c_str())!=0,"Cannot retire interrupted media segment");
            manifest.erase("pending_file");Save(manifest_path,manifest);
        }
        Require(manifest.at("segments").is_array()&&manifest.at("segments").size()<=100000,"Invalid media segment list");
        std::vector<std::filesystem::path> parts;
        auto publish_segment=[&](const Json& segment,const std::filesystem::path& path,uint64_t index){
            if(!job.segmentReady)return;
            const auto first=segment.value("source_frame",uint64_t(std::ceil(double(job.range.start100ns)*decoder.FrameRate()/10000000.))+segment.at("first").get<uint64_t>());
            const auto count=segment.at("count").get<uint64_t>();
            job.segmentReady({path,index,first,count,
                segment.value("source_pts",int64_t(std::llround(double(first)*10000000./decoder.FrameRate()))),
                segment.value("source_end",int64_t(std::llround(double(first+count)*10000000./decoder.FrameRate())))});
        };
        uint64_t committed=0;
        for(const auto& segment:manifest.at("segments")){
            const auto expected="segment-"+std::to_string(parts.size())+".mkv";
            Require(segment.at("file").get<std::string>()==expected&&segment.at("first")==committed&&segment.at("count").get<unsigned>()>0&&
                segment.at("count").get<unsigned>()<=std::max(job.segmentFrames,job.firstSegmentFrames),"Invalid media segment lineage");
            auto path=directory/Utf8Path(expected);
            NoLinks(path);
            Require(Hash(path,stop)==segment.at("sha256").get<std::string>(),"A cached media segment changed; preserved for inspection");
            committed+=segment.at("count").get<unsigned>();parts.push_back(path);
            publish_segment(segment,path,parts.size()-1);
        }
        result.reusedFrames=committed;
        const auto output_path=job.cachedPlayback?directory/L"render.mkv":job.output;
        NoLinks(output_path);
        if(job.cachedPlayback&&manifest.value("complete",false)&&manifest.contains("playback")){
            const auto& playback=manifest.at("playback");
            Require(playback.at("file")=="render.mkv"&&Hash(output_path,stop)==playback.at("sha256").get<std::string>(),
                "Cached playback changed; preserved for inspection");
            Require(committed==manifest.at("frames").get<uint64_t>(),"Cached playback frame lineage differs");
            result.playbackLease=phoenix_media_cache::Lease::Acquire(directory);
            result.frames=committed;result.payload=output_path;result.receipt=manifest_path;result.cacheHit=true;result.ok=true;return result;
        }
        if(job.lookupOnly){result.ok=true;result.preparedOnly=true;return result;}
        Require(!std::filesystem::exists(output_path),"An unpublished media output exists; preserved for inspection");
        const uint64_t expected_total=uint64_t(std::ceil(std::max(0.,decoder.DurationSeconds()-double(job.range.start100ns)*1e-7)*decoder.FrameRate()));
        if(!manifest.value("complete",false)){
            Save(manifest_path,manifest);
            std::vector<uint16_t> input(size_t(w)*h*4),output(size_t(ow)*oh*4);
            std::vector<uint8_t> bgra(size_t(ow)*oh*4);
            std::array<uint16_t,256> half{};for(unsigned i=0;i<256;++i)half[i]=DirectX::PackedVector::XMConvertFloatToHalf(float(i)/255.f);
            std::unique_ptr<RawVideoEncoder> encoder;std::filesystem::path partial;
            uint64_t selected=0,evaluated=0;unsigned chunk=0;
            uint64_t chunk_first_frame=0;int64_t chunk_first_pts=0,chunk_end_pts=0;
            auto commit=[&]{
                Require(encoder&&encoder->Finish(stop)==EncodeError::None,"Cannot finish media segment");encoder.reset();
                const auto filename="segment-"+std::to_string(parts.size())+".mkv";
                const auto final=directory/Utf8Path(filename);
                NoLinks(final);
                // A crash can leave an unpublished segment. Its manifest owns
                // no entry, so only this exact job-owned filename is replaced.
                Require(MoveFileExW(partial.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish media segment");partial.clear();
                manifest["segments"].push_back({{"file",filename},{"first",selected-chunk},{"count",chunk},{"sha256",Hash(final,stop)},
                    {"source_frame",chunk_first_frame},{"source_pts",chunk_first_pts},{"source_end",chunk_end_pts}});
                manifest.erase("pending_file");
                Save(manifest_path,manifest);parts.push_back(final);chunk=0;
                publish_segment(manifest["segments"].back(),final,parts.size()-1);
            };
            VideoFrame decoded;
            try{
                while(true){
                    while(job.pauseEvent&&WaitForSingleObject(job.pauseEvent,0)==WAIT_OBJECT_0&&!stop.stop_requested())
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if(stop.stop_requested()){result.cancelled=true;break;}
                    const auto read=decoder.ReadNextBlocking(decoded,stop);
                    if(read==VideoReadResult::EndOfStream)break;
                    if(read==VideoReadResult::Cancelled){result.cancelled=true;break;}
                    Require(read==VideoReadResult::FrameReady,"Source decode failed before completion");
                    if(job.range.end100ns&&decoded.timestamp100ns>=job.range.end100ns)break;
                    Require(decoded.layout==VideoPixelLayout::Bgra&&!decoded.pq&&decoded.bgra.size()==input.size(),"Media decoder did not provide SDR BGRA8");
                    for(size_t p=0;p<input.size();p+=4){input[p]=half[decoded.bgra[p+2]];input[p+1]=half[decoded.bgra[p+1]];input[p+2]=half[decoded.bgra[p]];input[p+3]=half[decoded.bgra[p+3]];}
                    Require(decoded.timestamp100ns<=INT64_MAX/100&&decoded.timestamp100ns>=INT64_MIN/100,"Media timestamp overflow");
                    PhoenixNativeFrameInfo info{sizeof(info),1,1,decoded.frameNumber,decoded.timestamp100ns*100,0,0};
                    Require(runtime.identity(runtime.session,&info)==0,runtime.error());
                    Require(runtime.frame(runtime.session,input.data(),input.size()*2,w*8,nullptr,0,0,output.data(),output.size()*2,ow*8,nullptr)==0,runtime.error());++evaluated;
                    decoder.RecycleFrameBuffer(std::move(decoded.bgra));
                    if(decoded.timestamp100ns<job.range.start100ns){++result.replayedFrames;continue;}
                    if(selected++<committed){++result.replayedFrames;continue;}
                    if(!encoder){
                        chunk_first_frame=decoded.frameNumber;chunk_first_pts=decoded.timestamp100ns;
                        partial=atomic_file::TemporaryBeside(directory/L"segment").wstring()+L".mkv";
                        const auto partial_name=partial.filename().u8string();
                        manifest["pending_file"]=std::string(partial_name.begin(),partial_name.end());Save(manifest_path,manifest);
                        EncoderSpec spec{ow,oh,decoder.FrameRate(),EncoderKind::H264Software};spec.quality=job.quality;
                        if(job.quality==EncoderQuality::Lossless)spec.kind=EncoderKind::Ffv1;
                        encoder=std::make_unique<RawVideoEncoder>(job.helpers);
                        auto started=encoder->Start(spec,partial);
                        if(ShouldRetryWithSoftware(spec.kind,started)){spec.kind=EncoderKind::H264Software;started=encoder->Start(spec,partial);}
                        Require(started==EncodeError::None,"Cannot start media segment encoder");
                    }
                    for(size_t p=0;p<output.size();p+=4)for(unsigned c=0;c<4;++c){
                        const float value=DirectX::PackedVector::XMConvertHalfToFloat(output[p+(c==0?2:c==2?0:c)]);
                        Require(std::isfinite(value),"Nonfinite Phoenix media output");bgra[p+c]=uint8_t(std::lround(std::clamp(value,0.f,1.f)*255.f));}
                    Require(encoder->WriteFrame(bgra,stop)==EncodeError::None,"Cannot encode media frame");++chunk;
                    chunk_end_pts=decoded.timestamp100ns+int64_t(std::llround(10000000./decoder.FrameRate()));
                    if(chunk==(parts.empty()&&job.firstSegmentFrames?job.firstSegmentFrames:job.segmentFrames))commit();
                    if(progress)progress(selected,expected_total);
                }
                if(result.cancelled){if(encoder)encoder->Cancel();if(!partial.empty())DeleteFileW(partial.c_str());manifest.erase("pending_file");Save(manifest_path,manifest);return result;}
                if(chunk)commit();
                Require(selected>=committed&&selected>0,"Source ended before the cached frame lineage");
                manifest["complete"]=true;manifest["frames"]=selected;manifest["runtime"]=runtime.Status();manifest["evaluated_frames"]=evaluated;
                Save(manifest_path,manifest);
            }catch(...){if(encoder)encoder->Cancel();if(!partial.empty())DeleteFileW(partial.c_str());throw;}
        }
        Require(!parts.empty(),"Media job produced no segments");
        Require(committed==0||!manifest.at("complete").get<bool>()||
            manifest.at("frames").get<uint64_t>()>=committed,"Complete cache has inconsistent frame count");
        // Validate a unique staging file before publishing without replacement.
        const auto staging=atomic_file::TemporaryBeside(output_path).wstring()+L".mkv";
        struct StagingOwner{std::filesystem::path path;~StagingOwner(){if(!path.empty())DeleteFileW(path.c_str());}} staged{staging};
        const auto concatenated=ConcatenateMedia(job.helpers,parts,staged.path,stop);
        if(concatenated==EncodeError::Cancelled){result.cancelled=true;return result;}
        Require(concatenated==EncodeError::None,"Cannot assemble Phoenix media segments");
        const auto probe=ProbeMedia(job.helpers,staged.path,stop,MediaProbeMode::FullValidation);
        Require(probe.ok&&probe.frameCount==manifest.at("frames").get<uint64_t>()&&probe.width==ow&&probe.height==oh,"Assembled media failed frame/geometry validation");
        const auto output_hash=Hash(staged.path,stop);
        Require(MoveFileExW(staged.path.c_str(),output_path.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish media output; existing files are preserved");staged.path.clear();
        if(job.cachedPlayback){manifest["playback"]={{"file","render.mkv"},{"sha256",output_hash}};Save(manifest_path,manifest);result.playbackLease=phoenix_media_cache::Lease::Acquire(directory);}
        result.frames=manifest.at("frames");result.receipt=manifest_path;result.payload=output_path;result.ok=true;
    }catch(const std::exception& error){result.cancelled=stop.stop_requested();result.detail=utf8_text::ToWide(error.what());}
    return result;
}
