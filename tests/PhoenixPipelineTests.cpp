#include "PhoenixPipelineClient.h"
#include "PhoenixExecution.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "NeuralCache.h"
#include "CompareImageIO.h"
#include "nlohmann/json.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <DirectXPackedVector.h>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void hr(HRESULT value){check(SUCCEEDED(value),"D3D12 test operation failed");}
float half_value(uint16_t bits){
    const unsigned exponent=(bits>>10)&31, fraction=bits&1023;
    check(exponent!=31,"Nonfinite pipeline output");
    const float value=exponent?std::ldexp(float(1024+fraction),int(exponent)-25):std::ldexp(float(fraction),-24);
    return bits&0x8000?-value:value;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)));return result;
}
void barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER value{};value.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;value.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};cmd->ResourceBarrier(1,&value);
}
Json renderer_check(unsigned w=64,unsigned h=40,bool nv12=false,
    const std::filesystem::path& raw={},const std::filesystem::path& snapshots={},bool scaled=false){
    WNDCLASSW window_class{};window_class.lpfnWndProc=DefWindowProcW;window_class.hInstance=GetModuleHandleW(nullptr);window_class.lpszClassName=L"PhoenixNativeRendererCheck";
    RegisterClassW(&window_class);
    const unsigned view_w=scaled?1100:w,view_h=scaled?unsigned(std::lround(double(h)*1100/w)):h;
    // A hidden borderless window avoids the OS minimum caption width changing
    // tiny WARP fixture geometry. No desktop interaction is involved.
    HWND window=CreateWindowExW(0,window_class.lpszClassName,L"Phoenix renderer test",WS_POPUP,0,0,int(view_w),int(view_h),nullptr,nullptr,window_class.hInstance,nullptr);
    check(window!=nullptr,"Cannot create hidden renderer test window");
    struct WindowOwner{HWND value;~WindowOwner(){DestroyWindow(value);}} owner{window};
    auto renderer=MakeD3D12Renderer();check(bool(renderer),"Cannot allocate renderer");
    renderer->SetPresentFollowsWindow(true);
    renderer->SetHdrOutputAllowed(true);
    if(nv12){
        renderer->SetSourceLayout(PixelLayout::Nv12);
        renderer->SetSourceColor({ColorMatrix::Bt709,ColorRange::Limited,ColorPrimaries::Bt709,ColorTransfer::Bt709});
    }
    check(renderer->Initialize(window,w,h,w,h,8,5,DefaultNeuralCarrierQuality()),"Direct Phoenix renderer initialization failed");
    std::vector<uint8_t> input(w*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const size_t p=(size_t(y)*w+x)*4;
        input[p]=uint8_t(20+(x*3+y*5)%210);input[p+1]=uint8_t(25+(x/9*31+y/7*19)%200);
        input[p+2]=uint8_t(30+(x*x/17+y*11)%190);input[p+3]=255;
    }
    std::vector<uint8_t> planar;
    if(nv12){
        planar.resize(size_t(w)*h*3/2,128);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            const auto value=uint8_t(20+(x/9*13+y/7*11)%210);planar[size_t(y)*w+x]=value;
            const auto expected=uint8_t(std::clamp(std::lround((int(value)-16)*255.0/219),0L,255L));
            const auto p=(size_t(y)*w+x)*4;input[p]=input[p+1]=input[p+2]=expected;
        }
    }
    if(!raw.empty()){
        auto& source=nv12?planar:input;
        check(std::filesystem::file_size(raw)==source.size(),"Raw comparison source has the wrong byte count");
        std::ifstream file(raw,std::ios::binary);file.read(reinterpret_cast<char*>(source.data()),std::streamsize(source.size()));
        check(bool(file),"Raw comparison source read failed");
    }
    FrameIdentity identity{};GuideFrame guide{};
    for(unsigned i=0;i<3;++i){identity.frameNumber=i;identity.pts100ns=i*166667;identity.reset=i?HistoryReset::None:HistoryReset::FirstFrame;guide.id=identity;
        const auto& source=nv12?planar:input;
        check(renderer->RenderFrame(source.data(),source.size(),identity,guide,16.6667f),"Direct renderer frame failed");
        check(renderer->LastFrameUsedDLSS(),"Direct renderer presented fallback instead of SDK result");}
    const auto evaluations=renderer->DLSSEvaluations();check(evaluations==3,"Renderer repeated or skipped a model evaluation");
    unsigned modes=0;unsigned maximum_original_error=0;
    std::vector<uint8_t> original,neural,wipe,split;
    for(auto mode:{ComparisonMode::Neural,ComparisonMode::Original,ComparisonMode::SplitVertical,ComparisonMode::Wipe,ComparisonMode::Difference,
        ComparisonMode::SideBySide,ComparisonMode::Quad}){
        ComparisonSettings settings;settings.mode=mode;settings.labels=false;renderer->SetComparison(settings);
        check(renderer->PresentCurrent(),"Comparison presentation failed");
        std::vector<uint8_t> pixels;uint32_t width=0,height=0;
        check(renderer->CaptureComposedView(pixels,width,height),"Comparison readback failed");
        check(width==view_w&&height==view_h&&pixels.size()==size_t(view_w)*view_h*4,"Comparison geometry changed");
        if(mode==ComparisonMode::Original&&!scaled&&(!nv12||raw.empty()))for(size_t p=0;p<pixels.size();p+=4){
            const int expected[]={input[p+2],input[p+1],input[p],255};for(unsigned c=0;c<4;++c){const auto error=unsigned(std::abs(int(pixels[p+c])-expected[c]));
                maximum_original_error=std::max(maximum_original_error,error);}}
        if(mode==ComparisonMode::Original)original=pixels;
        if(mode==ComparisonMode::Neural)neural=pixels;
        if(mode==ComparisonMode::Wipe)wipe=pixels;
        if(mode==ComparisonMode::SplitVertical)split=pixels;
        if(!snapshots.empty()){
            std::vector<uint8_t> bgr(size_t(width)*height*3);
            for(size_t p=0,q=0;p<pixels.size();p+=4,q+=3){bgr[q]=pixels[p+2];bgr[q+1]=pixels[p+1];bgr[q+2]=pixels[p];}
            hr(compare_image::SavePngBgr(snapshots/(std::to_wstring(int(mode))+L".png"),bgr.data(),width,height,width*3));
        }
        check(renderer->DLSSEvaluations()==evaluations,"Comparison reran the model");++modes;
    }
    if(maximum_original_error>2)std::cerr<<"Original max error "<<maximum_original_error<<" layout="<<nv12<<" first RGB="<<unsigned(original[0])<<","<<unsigned(original[1])<<","<<unsigned(original[2])<<" expected="<<unsigned(input[2])<<","<<unsigned(input[1])<<","<<unsigned(input[0])<<'\n';
    check(maximum_original_error<=(nv12?2u:1u),"Original comparison does not match the submitted source");
    uint64_t difference_sum=0,changed_channels=0;unsigned maximum_difference=0,maximum_wipe_error=0,maximum_split_error=0;
    for(unsigned y=0;y<view_h;++y)for(unsigned x=0;x<view_w;++x)for(unsigned c=0;c<3;++c){
        const auto p=(size_t(y)*view_w+x)*4+c;
        const unsigned difference=unsigned(std::abs(int(neural[p])-int(original[p])));
        difference_sum+=difference;changed_channels+=difference>1;maximum_difference=std::max(maximum_difference,difference);
        const auto expected=x<view_w/2?original[p]:neural[p];
        maximum_split_error=std::max(maximum_split_error,unsigned(std::abs(int(split[p])-int(expected))));
        if(std::abs(int(x)-int(view_w/2))>4){
            maximum_wipe_error=std::max(maximum_wipe_error,unsigned(std::abs(int(wipe[p])-int(expected))));
        }
    }
    check(maximum_wipe_error<=1,"Wipe does not show original and processed pixels on the correct halves");
    check(maximum_split_error<=1,"Split does not show original and processed pixels on the correct halves");
    auto runtime=Json::parse(renderer->DirectPhoenixStatus());check(runtime.at("frames")==3,"Missing direct renderer completion evidence");
    if(runtime.at("backend")=="native_rtx")
        check(maximum_difference>2&&changed_channels>0,"Neural view is identical to the source");
    check(runtime.at("media_frame").at("frame_number")==2&&runtime.at("media_frame").at("pts_100ns")==333334,
        "Comparison changed the completed source identity");
    Json tuning_check=nullptr;
    if((runtime.at("backend")=="native_rtx"||runtime.at("backend")=="portable_pipeline_d3d12")&&raw.empty()){
        check(renderer->SetDirectPhoenixTuning("{\"intensity\":0,\"style\":0,\"network_passes\":2}"),"Live Phoenix tuning was refused");
        check(!renderer->SetDirectPhoenixTuning("{\"network_passes\":0}"),"Invalid tuning was accepted");
        identity.frameNumber=3;identity.pts100ns=500001;identity.reset=HistoryReset::Seek;++identity.historyGeneration;guide.id=identity;
        const auto& source=nv12?planar:input;
        check(renderer->RenderFrame(source.data(),source.size(),identity,guide,16.6667f),"Frame after live controls failed");
        ComparisonSettings settings;settings.mode=ComparisonMode::Neural;settings.labels=false;renderer->SetComparison(settings);
        check(renderer->PresentCurrent(),"Live tuning presentation failed");
        std::vector<uint8_t> pixels;uint32_t pw=0,ph=0;check(renderer->CaptureComposedView(pixels,pw,ph),"Live tuning readback failed");
        unsigned error=0;for(size_t p=0;p<pixels.size();p+=4)for(unsigned c=0;c<3;++c)error=std::max(error,unsigned(std::abs(int(pixels[p+c])-int(original[p+c]))));
        check(error<=2,"Zero intensity did not restore source pixels");
        const auto tuned=Json::parse(renderer->DirectPhoenixStatus());
        check(tuned.at("independent_temporal_stages")==2,"Live passes do not own separate histories");
        check(tuned.at("media_recipe").at("tuning").at("network_passes")==2,"Export recipe did not capture applied tuning");
        tuning_check={{"passed",true},{"zero_intensity_max_byte_error",error},{"independent_pass_histories",2},{"invalid_change_preserves_working",true},{"export_recipe_updated",true}};
        if(!nv12){
            std::vector<uint32_t> pq(size_t(w)*h);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)pq[size_t(y)*w+x]=(100+(x*7+y*11)%800)|((110+(x*13+y*3)%800)<<10)|((120+(x*3+y*17)%800)<<20)|(3u<<30);
            ++identity.frameNumber;identity.pts100ns+=166667;identity.reset=HistoryReset::Seek;++identity.historyGeneration;guide.id=identity;
            renderer->SetNextSourcePq(true);
            check(renderer->RenderFrame(reinterpret_cast<const uint8_t*>(pq.data()),pq.size()*4,identity,guide,16.6667f),"Direct HDR neural frame failed");
            check(renderer->LastFrameUsedDLSS(),"HDR source silently bypassed the model");
            std::vector<uint8_t> hdr_original,hdr_neural;
            for(const auto mode:{ComparisonMode::Original,ComparisonMode::Neural}){
                settings.mode=mode;renderer->SetComparison(settings);check(renderer->PresentCurrent(),"HDR comparison presentation failed");
                auto& hdr_pixels=mode==ComparisonMode::Original?hdr_original:hdr_neural;check(renderer->CaptureComposedView(hdr_pixels,pw,ph),"HDR comparison readback failed");
            }
            unsigned hdr_error=0;size_t worst_hdr=0;for(size_t p=0;p<hdr_original.size();p+=4)for(unsigned c=0;c<3;++c){const auto hdr_difference=unsigned(std::abs(int(hdr_original[p+c])-int(hdr_neural[p+c])));if(hdr_difference>hdr_error){hdr_error=hdr_difference;worst_hdr=p+c;}}
            if(hdr_error>3)std::cerr<<"HDR compare max="<<hdr_error<<" at "<<worst_hdr<<" original="<<unsigned(hdr_original[worst_hdr])<<" processed="<<unsigned(hdr_neural[worst_hdr])<<" domain="<<Json::parse(renderer->DirectPhoenixStatus()).at("media_color_domain")<<'\n';
            check(hdr_error<=3,"HDR original/processed comparison has inconsistent display conversion");
            check(Json::parse(renderer->DirectPhoenixStatus()).at("media_color_domain").get<std::string>().find("HDR")!=std::string::npos,"HDR path lacks source-domain evidence");
            tuning_check["hdr_pq_source_zero_intensity_max_byte_error"]=hdr_error;
            check(renderer->SetDirectPhoenixTuning("{\"intensity\":1,\"network_passes\":1}"),"Cannot enable neural HDR edits");
            ++identity.frameNumber;identity.pts100ns+=166667;guide.id=identity;renderer->SetNextSourcePq(true);
            check(renderer->RenderFrame(reinterpret_cast<const uint8_t*>(pq.data()),pq.size()*4,identity,guide,16.6667f),"HDR neural edits did not execute");
            settings.mode=ComparisonMode::Neural;renderer->SetComparison(settings);check(renderer->PresentCurrent(),"HDR edit presentation failed");
            check(renderer->CaptureComposedView(hdr_neural,pw,ph),"HDR edit readback failed");unsigned hdr_changed=0;
            for(size_t p=0;p<hdr_neural.size();p+=4)for(unsigned c=0;c<3;++c)hdr_changed+=std::abs(int(hdr_original[p+c])-int(hdr_neural[p+c]))>2;
            check(hdr_changed>100,"HDR model output was bypassed by the transfer bridge");tuning_check["hdr_changed_channels"]=hdr_changed;
        }
        check(renderer->ApplyDirectPhoenixSettings(runtime.at("media_recipe").at("pipeline").dump(),
            runtime.at("media_recipe").value("tuning",Json::object()).dump()),"Cannot restore the test recipe between source layouts");
        renderer->SetDirectPhoenixProcessing(false);const auto before_cached=renderer->DLSSEvaluations();
        std::vector<uint8_t> external_reference(size_t(w)*h*4);
        for(size_t p=0;p<external_reference.size();p+=4){external_reference[p]=17;external_reference[p+1]=83;external_reference[p+2]=209;external_reference[p+3]=255;}
        check(renderer->UploadReferenceFrame(external_reference.data(),external_reference.size()),"Cannot upload the cached pair's original reference");
        ++identity.frameNumber;identity.pts100ns+=166667;guide.id=identity;
        check(renderer->RenderFrame(source.data(),source.size(),identity,guide,16.6667f),"Cached bypass frame failed");
        check(renderer->DLSSEvaluations()==before_cached&&!renderer->LastFrameUsedDLSS(),"Cached output was processed by the model again");
        settings.mode=ComparisonMode::Original;renderer->SetComparison(settings);check(renderer->PresentCurrent(),"Cached original presentation failed");
        check(renderer->CaptureComposedView(pixels,pw,ph),"Cached original readback failed");
        for(size_t p=0;p<pixels.size();p+=4)for(unsigned c=0;c<3;++c){const unsigned expected=c==0?209:c==1?83:17;
            check(std::abs(int(pixels[p+c])-int(expected))<=1,"Direct snapshot overwrote a cached pair's external original");}
        tuning_check["cached_bypass_no_model_rerun"]=true;tuning_check["external_reference_preserved"]=true;
        auto monitored_options=runtime.at("media_recipe").at("pipeline");monitored_options["PipelineMonitoring"]=1;
        check(renderer->ApplyDirectPhoenixSettings(monitored_options.dump(),"{}"),"Cannot enable latency monitoring");
        renderer->SetDirectPhoenixProcessing(true);renderer->SetNextSourcePq(false);
        const auto received=std::chrono::steady_clock::now()-std::chrono::milliseconds(15);
        const auto received_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(received.time_since_epoch()).count());
        renderer->SetSourceReadyTime(received_ns,received_ns+5000000);
        ++identity.frameNumber;identity.pts100ns+=166667;guide.id=identity;
        check(renderer->RenderFrame(source.data(),source.size(),identity,guide,16.6667f),"Monitored source frame failed");
        const auto latency_status=Json::parse(renderer->DirectPhoenixStatus());
        check(latency_status.at("caller_latency_available")==true&&latency_status.at("source_to_sdk_completion_ms").get<double>()>=10,"Player did not forward eligible source age");
        const auto latency=latency_status.at("playback_telemetry");
        check(latency.at("source_ready_to_present_return_ms").get<double>()>=15&&latency.at("planned_media_hold_excluded_from_budget")==true,"Playback source readiness/deadline evidence missing");
        check(renderer->PresentCurrent(),"Repeated-frame latency presentation failed");
        const auto repeated=Json::parse(renderer->DirectPhoenixStatus()).at("playback_telemetry");
        check(repeated.at("presentation_reuses_completed_frame")==true&&repeated.at("sdk_to_present_return_ms")==latency.at("sdk_to_present_return_ms"),"Paused redraw changed the controller's presentation-tail sample");
        check(renderer->ApplyDirectPhoenixSettings(runtime.at("media_recipe").at("pipeline").dump(),"{}"),"Cannot disable latency monitoring");
        check(Json::parse(renderer->DirectPhoenixStatus()).at("playback_telemetry").at("monitoring")==0,"Monitoring-off retained timing work");
        tuning_check["source_ready_and_deadline_latency_forwarded"]=true;tuning_check["paused_redraw_does_not_resample_tail"]=true;
        if(!nv12&&runtime.at("media_recipe").contains("portable_package")){
            const auto active=phoenix_player::ExecutionDevice(runtime.at("media_recipe"));
            const std::string other=active=="cpu"?"native":"cpu";
            const auto pipeline=runtime.at("media_recipe").at("pipeline").dump();
            const std::string cheap="{\"central_blocks\":1}";
            check(renderer->ApplyDirectPhoenixSettings(pipeline,cheap,other),"Cannot switch the active processing backend");
            ++identity.frameNumber;identity.pts100ns+=166667;guide.id=identity;
            check(renderer->RenderFrame(source.data(),source.size(),identity,guide,16.6667f),"Frame after backend switch failed");
            const auto switched=Json::parse(renderer->DirectPhoenixStatus());
            check(switched.at("backend")==std::string(other==std::string("native")?"native_rtx":"portable_pipeline_d3d12"),"Backend choice did not change execution");
            check(!renderer->ApplyDirectPhoenixSettings(pipeline,cheap,"cuda:unavailable"),"Unavailable backend selection was accepted");
            check(Json::parse(renderer->DirectPhoenixStatus()).at("backend")==switched.at("backend"),"Refused backend destroyed the working session");
            check(renderer->ApplyDirectPhoenixSettings(pipeline,runtime.at("media_recipe").value("tuning",Json::object()).dump(),active),"Cannot restore the selected portable backend");
            tuning_check["live_backend_switch_and_failed_selection_preserved"]=true;
        }

    }
    return {{"passed",true},{"source_layout",nv12?"NV12":"BGRA"},{"window_compositor",true},{"frames",3},{"comparison_modes",modes},{"comparison_changes_reran_model",false},
        {"original_max_byte_error",maximum_original_error},{"neural_original_max_byte_difference",maximum_difference},
        {"neural_original_mean_byte_difference",double(difference_sum)/(view_w*view_h*3)},
        {"changed_channel_fraction",double(changed_channels)/(view_w*view_h*3)},
        {"wipe_max_byte_error",maximum_wipe_error},{"split_max_byte_error",maximum_split_error},
        {"source_extent",{w,h}},{"view_extent",{view_w,view_h}},
        {"live_neural_controls",tuning_check},{"runtime_backend",runtime.at("backend")},
        {"model",runtime.at("model_id")},{"weights_sha256",runtime.at("packed_weights_sha256")},{"motion_provider",runtime.at("motion_provider")}};
}
}
int wmain(int argc,wchar_t** argv){
    try{
        check(argc==3||argc==4||argc==5||argc==8||argc==9,"Usage: PhoenixPipelineTests <native DLL> <scratch directory> [renderer recipe [raw source width height BGRA|NV12 [scaled]]]");
        if(argc==5&&std::wstring_view(argv[4])==L"--portable"){
            hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
            check(SetEnvironmentVariableW(L"PHOENIX_MEDIA_RECIPE",argv[3])!=0,"Cannot select portable renderer recipe");
            auto report=renderer_check(32,24);check(report.at("runtime_backend")=="portable_pipeline_d3d12","Wrong portable renderer backend");
            report["nv12"]=renderer_check(32,24,true);std::cout<<report.dump()<<'\n';CoUninitialize();return 0;
        }
        if(argc>=8){
            hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
            check(SetEnvironmentVariableW(L"PHOENIX_MEDIA_RECIPE",argv[3])!=0,"Cannot select renderer recipe");
            const auto report=renderer_check(unsigned(std::stoul(argv[5])),unsigned(std::stoul(argv[6])),
                std::wstring_view(argv[7])==L"NV12",argv[4],argv[2],argc==9);
            CoUninitialize();std::cout<<report.dump()<<'\n';return 0;
        }
        const bool hardware_transfer=argc==4&&std::wstring_view(argv[3])==L"--transfer-gpu";
        if(argc==4&&!hardware_transfer){
            if(std::wstring_view(argv[3])==L"--standalone") {
                check(GetEnvironmentVariableW(L"PHOENIX_MEDIA_RECIPE",nullptr,0)==0,"Standalone test must not inherit a recipe override");
                check(PhoenixPipelineClient::Requested(),"Standalone launch did not discover the saved recipe");
            } else check(SetEnvironmentVariableW(L"PHOENIX_MEDIA_RECIPE",argv[3])!=0,"Cannot select renderer recipe");
            auto report=renderer_check(320,180);check(report.at("runtime_backend")=="native_rtx","Neural renderer did not use native RTX");
            check(report.at("weights_sha256").get<std::string>().size()==64,"Neural model identity missing");
            report["nv12"]=renderer_check(320,180,true);
            std::cout<<report.dump()<<'\n';return 0;
        }
        const std::filesystem::path library=argv[1],scratch=argv[2];
        const auto hash=Sha256File(library);check(bool(hash),"Cannot hash test DLL");
        const auto recipe_path=scratch/L"media-recipe.json";
        const auto path_text=library.u8string();
        Json recipe{{"schema","phoenix-media/1"},{"native_library",std::string(path_text.begin(),path_text.end())},
            {"native_library_sha256",*hash},{"runtime",{{"spatial_only",true}}},{"pipeline",{{"PipelineMonitoring",0}}}};
        {std::ofstream file(recipe_path);file<<recipe.dump();}
        check(SetEnvironmentVariableW(L"PHOENIX_MEDIA_RECIPE",recipe_path.c_str())!=0,"Cannot set test recipe");
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        if(hardware_transfer){for(unsigned index=0;;++index){hr(factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));
            DXGI_ADAPTER_DESC1 description{};hr(adapter->GetDesc1(&description));if(description.VendorId==0x10de&&!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))break;adapter.Reset();}}
        else hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        constexpr unsigned width=31,height=19;
        PhoenixPipelineClient client;check(client.Initialize(device.Get(),queue.Get(),width,height,width,height),client.Error().c_str());
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=width;texture.Height=height;
        texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> source,output;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&source)));
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&output)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};uint64_t bytes=0;device->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto upload=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD),readback=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK);
        void* mapped=nullptr;D3D12_RANGE empty{};hr(upload->Map(0,&empty,&mapped));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            auto* pixel=reinterpret_cast<uint16_t*>(static_cast<char*>(mapped)+footprint.Footprint.RowPitch*y)+x*4;
            pixel[0]=uint16_t(0x2800+(x*31)%0x1000);pixel[1]=0x3800;pixel[2]=0x3400;pixel[3]=0x3c00;
        }upload->Unmap(0,nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> command;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)));
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(command.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        hr(command->Close());ID3D12CommandList* lists[]={command.Get()};queue->ExecuteCommandLists(1,lists);
        FrameIdentity identity{};
        for(unsigned i=0;i<3;++i){identity.frameNumber=i;identity.pts100ns=i*166667;
            check(client.Evaluate(source.Get(),D3D12_RESOURCE_STATE_COMMON,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,i==0,&identity,false),client.Error().c_str());}
        check(client.EvaluationCount()==3,"Phoenix did not process each source frame");
        hr(allocator->Reset());hr(command->Reset(allocator.Get(),nullptr));
        from={};from.pResource=output.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to={};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        command->CopyTextureRegion(&to,0,0,0,&from,nullptr);hr(command->Close());queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));hr(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Fence event allocation failed");hr(fence->SetEventOnCompletion(1,event));const auto waited=WaitForSingleObject(event,30000);CloseHandle(event);check(waited==WAIT_OBJECT_0,"Readback timeout");
        D3D12_RANGE range{0,size_t(bytes)};hr(readback->Map(0,&range,&mapped));
        unsigned maximum_half_units=0;float maximum_linear_error=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const auto* pixel=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped)+footprint.Footprint.RowPitch*y)+x*4;
            const uint16_t expected[]={uint16_t(0x2800+(x*31)%0x1000),0x3800,0x3400,0x3c00};
            for(unsigned c=0;c<4;++c){const unsigned difference=unsigned(std::abs(int(pixel[c])-int(expected[c])));maximum_half_units=std::max(maximum_half_units,difference);
                const float error=std::abs(half_value(pixel[c])-half_value(expected[c]));maximum_linear_error=std::max(maximum_linear_error,error);
                check(error<=.001f,"Linear/proxy roundtrip exceeds the FP16 transfer tolerance");
                if(c==3)check(pixel[c]==expected[c],"Alpha changed during the color transfer");}
        }readback->Unmap(0,&empty);
        const auto status=Json::parse(client.Status());check(status.at("frames")==3,"Missing SDK completion evidence");
        check(status.at("media_frame").at("frame_number")==2,"Completed frame identity missing");
        const std::array<std::array<float,3>,8> hdr_colors{{{0,0,0},{.001f,.002f,.0005f},{.18f,.3f,.09f},{1,1,1},{4,2,1},{16,8,2},{100,40,10},{4,-.1f,.5f}}};
        hr(upload->Map(0,&empty,&mapped));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto* pixel=reinterpret_cast<uint16_t*>(static_cast<char*>(mapped)+footprint.Footprint.RowPitch*y)+x*4;
            const auto& values=hdr_colors[(x+y)%hdr_colors.size()];for(unsigned c=0;c<3;++c)pixel[c]=DirectX::PackedVector::XMConvertFloatToHalf(values[c]);pixel[3]=0x3c00;}
        upload->Unmap(0,nullptr);hr(allocator->Reset());hr(command->Reset(allocator.Get(),nullptr));
        D3D12_TEXTURE_COPY_LOCATION hdr_from{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,footprint},hdr_to{source.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,0};
        barrier(command.Get(),source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);command->CopyTextureRegion(&hdr_to,0,0,0,&hdr_from,nullptr);
        barrier(command.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);hr(command->Close());queue->ExecuteCommandLists(1,lists);
        identity.frameNumber=3;identity.pts100ns=500001;identity.reset=HistoryReset::Seek;
        check(client.Evaluate(source.Get(),D3D12_RESOURCE_STATE_COMMON,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,true,&identity,true),client.Error().c_str());
        hr(allocator->Reset());hr(command->Reset(allocator.Get(),nullptr));command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        hr(command->Close());queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),2));
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"HDR fence event allocation failed");hr(fence->SetEventOnCompletion(2,event));
        const auto hdr_waited=WaitForSingleObject(event,30000);CloseHandle(event);check(hdr_waited==WAIT_OBJECT_0,"HDR readback timeout");
        hr(readback->Map(0,&range,&mapped));float hdr_error=0,hdr_peak=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto* pixel=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped)+footprint.Footprint.RowPitch*y)+x*4;
            const auto& values=hdr_colors[(x+y)%hdr_colors.size()];for(unsigned c=0;c<3;++c){const float actual=half_value(pixel[c]);
                const float expected=half_value(DirectX::PackedVector::XMConvertFloatToHalf(values[c]));
                const float error=std::abs(actual-expected)/std::max(1.f,std::abs(expected));hdr_error=std::max(hdr_error,error);hdr_peak=std::max(hdr_peak,actual);
                check(error<.004f,"HDR bridge does not preserve scene light within FP16 proxy tolerance");}
            check(pixel[3]==0x3c00,"HDR bridge changed alpha");}
        readback->Unmap(0,&empty);check(hdr_peak>99,"HDR highlights were clipped");
        const auto hdr_status=Json::parse(client.Status());auto invalid=identity;invalid.pts100ns=INT64_MAX;
        check(!client.Evaluate(source.Get(),D3D12_RESOURCE_STATE_COMMON,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,false,&invalid,false),"Invalid source identity was accepted");
        check(Json::parse(client.Status()).at("media_frame")==hdr_status.at("media_frame"),"Failed evaluation changed completed frame identity");
        const auto renderer_report=renderer_check();
        std::cout<<Json{{"passed",true},{"frames",4},{"renderer",renderer_report},{"roundtrip_max_half_units",maximum_half_units},
            {"roundtrip_max_linear_error",maximum_linear_error},{"alpha_exact",true},{"hdr_proxy_roundtrip",true},{"hardware_transfer",hardware_transfer},
            {"hdr_max_normalized_error",hdr_error},{"hdr_peak_linear",hdr_peak},{"runtime",hdr_status}}.dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
