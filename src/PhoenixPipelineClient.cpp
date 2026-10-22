#include "PhoenixPipelineClient.h"
#include "phoenix_native.h"
#include "nlohmann/json.hpp"
#include "Log.h"
#include "NeuralCache.h"
#include "PhoenixRecipe.h"
#include "PhoenixPlaybackRecovery.h"
#include "pipeline/color_bridge.h"
#include "PhoenixPreferences.h"
#include <shlobj.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <limits>
#include <array>
#include <cmath>
#include <algorithm>
#include <cctype>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
namespace {
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void CheckHR(HRESULT value){if(FAILED(value))throw std::runtime_error("Phoenix media D3D12 operation failed: "+std::to_string(unsigned(value)));}
std::wstring Environment(const wchar_t* key){
    const DWORD size=GetEnvironmentVariableW(key,nullptr,0); if(!size)return {};
    Check(size<=32768,"Phoenix environment path is too long");std::wstring value(size,L'\0');
    const DWORD read=GetEnvironmentVariableW(key,value.data(),size);Check(read&&read<size,"Phoenix environment changed");value.resize(read);return value;
}
Json ReadJson(const std::filesystem::path& path){
    Check(path.is_absolute()&&std::filesystem::is_regular_file(path)&&std::filesystem::file_size(path)<=262144,"Phoenix media recipe is missing or oversized");
    std::ifstream input(path,std::ios::binary);Check(bool(input),"Cannot read Phoenix media recipe");return Json::parse(input);
}
std::filesystem::path SelectedRecipePath(){
    // Resolve once per process. Recreating a renderer after a seek or resize
    // must not silently switch to a recipe saved by another running app.
    static const auto selected=[] {
        std::wstring executable(32768,L'\0');
        const DWORD size=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
        Check(size&&size<executable.size(),"Cannot locate Phoenix Video Player");executable.resize(size);
        PWSTR folder=nullptr;std::filesystem::path local;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder))){local=folder;CoTaskMemFree(folder);}
        return SelectPhoenixRecipe(Environment(L"PHOENIX_MEDIA_RECIPE"),std::filesystem::path(executable).parent_path(),local);
    }();
    return selected;
}
const Json& BaseRecipe(){
    static const auto recipe=ReadJson(SelectedRecipePath());return recipe;
}
std::filesystem::path PreferencesPath(){
    PWSTR raw=nullptr;Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw)),"Phoenix preferences location is unavailable");
    std::filesystem::path root(raw);CoTaskMemFree(raw);
    return root/L"Phoenix"/L"MediaPreferences"/(phoenix_player::PreferenceIdentity(BaseRecipe())+".json");
}
const Json& SelectedRecipe(){
    static const auto selected=[](){
        const auto base=BaseRecipe();
        if(!Environment(L"PHOENIX_MEDIA_RECIPE").empty())return base;
        try{return phoenix_player::LoadPreferences(PreferencesPath(),base);}
        catch(const std::exception& error){LOG("Phoenix preferences ignored: "<<error.what());return base;}
    }();return selected;
}
phoenix_player::PlaybackRecovery& PlaybackRecovery(){
    static phoenix_player::PlaybackRecovery recovery(SelectedRecipe(),[]{
        auto path=phoenix_pipeline::application_recovery_path();const auto root=Environment(L"PHOENIX_RECOVERY_ROOT");
        if(!root.empty()){const std::filesystem::path directory(root);Check(directory.is_absolute(),"Phoenix recovery root must be absolute");path=directory/path.filename();}
        return path;
    }());return recovery;
}
void Barrier(ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    if(from==to)return;D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};command->ResourceBarrier(1,&barrier);
}
constexpr char ConversionShader[]=R"hlsl(
Texture2D<float4> Source:register(t0);Texture2D<float4> Original:register(t1);Texture2D<float4> Baseline:register(t2);RWTexture2D<float4> Destination:register(u0);
cbuffer Config:register(b0){uint Width,Height,Decode,Hdr;};
float3 sampleImage(Texture2D<float4> image,uint2 pixel){uint sw,sh;image.GetDimensions(sw,sh);
 if(sw==Width&&sh==Height)return image.Load(int3(pixel,0)).rgb;
 float2 p=(float2(pixel)+.5)*float2(sw,sh)/float2(Width,Height)-.5;
 int2 a=int2(floor(p)),hi=int2(sw-1,sh-1);float2 f=frac(p);
 return lerp(lerp(image.Load(int3(clamp(a,0,hi),0)).rgb,image.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,f.x),
   lerp(image.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,image.Load(int3(clamp(a+1,0,hi),0)).rgb,f.x),f.y);
}
float encode(float x){x=saturate(x);return x<=0.0031308?12.92*x:1.055*pow(x,1.0/2.4)-0.055;}
float decode(float x){x=saturate(x);return x<=0.04045?x/12.92:pow((x+0.055)/1.055,2.4);}
[numthreads(16,16,1)]void main(uint3 t:SV_DispatchThreadID){
 if(t.x>=Width||t.y>=Height)return;float4 c=Source.Load(int3(t.xy,0));
 if(Hdr){
   if(Decode){float3 original=sampleImage(Original,t.xy),baseline=sampleImage(Baseline,t.xy);
     // Transfer the neural edit around the authenticated, FP16 proxy actually
     // submitted. Subtract its round-trip bias, especially near HDR gamut
     // boundaries where a small proxy error can become a visible dark tint.
     c.rgb=original+(ReferenceDecode(original,c.rgb)-ReferenceDecode(original,baseline));
   }else c.rgb=ReferenceEncode(c.rgb);
 }else c.rgb=Decode?float3(decode(c.r),decode(c.g),decode(c.b)):float3(encode(c.r),encode(c.g),encode(c.b));
 // Make the intermediate FP16 quantization explicit. The two transfer-function
 // conversions remain lossy and are checked numerically by the adapter test.
 c.rgb=f16tof32(f32tof16(c.rgb));Destination[t.xy]=c;
})hlsl";
}
struct PhoenixPipelineClient::State {
    HMODULE module=nullptr;
    PhoenixNativeSession* session=nullptr;
    decltype(&phoenix_native_create) create=nullptr;
    decltype(&phoenix_native_destroy) destroy=nullptr;
    decltype(&phoenix_native_frame_d3d12) frame=nullptr;
    decltype(&phoenix_native_default_controls) defaults=nullptr;
    decltype(&phoenix_native_set_frame_info) identity=nullptr;
    decltype(&phoenix_native_status) status=nullptr;
    decltype(&phoenix_native_error) error=nullptr;
    decltype(&phoenix_native_set_tuning) tuning=nullptr;
    decltype(&phoenix_native_set_latency) latency=nullptr;
    decltype(&phoenix_native_reconfigure) reconfigure=nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;uint64_t signal=0;
    ComPtr<ID3D12Resource> proxy_input,proxy_output;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> conversion;
    std::array<ComPtr<ID3D12DescriptorHeap>,2> views;
    unsigned width=0,height=0,output_width=0,output_height=0;
    Json recipe, completed_frame=nullptr,completed_recipe=nullptr;
    bool hdr_source=false;
    Json saved_preferences;
    uint64_t source_ready_ns=0,source_due_ns=0,sdk_finished_ns=0;
    double presentation_tail_ms=0;bool pending_presentation=false;
    double sdk_processing_ms=0;
    std::wstring telemetry_text;
    Json playback_telemetry=Json::object();
    ~State(){
        if(session&&destroy&&destroy(session)!=0){LOG("Phoenix media session retained after failed GPU drain");module=nullptr;}
        if(module)FreeLibrary(module);if(event)CloseHandle(event);
    }
    template<class T>void Resolve(T& pointer,const char* name){pointer=reinterpret_cast<T>(GetProcAddress(module,name));Check(pointer!=nullptr,name);}
    void RequireExecutionContract(const Json& runtime){
        if(!runtime.contains("portable_runtime"))return;
        auto contract=reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"phoenix_native_portable_contract"));
        Check(contract&&contract()>=1,"Update the Phoenix SDK before selecting a portable processing backend");
    }
    void Begin(){CheckHR(allocator->Reset());CheckHR(command->Reset(allocator.Get(),nullptr));}
    void Submit(bool wait){
        CheckHR(command->Close());ID3D12CommandList* lists[]={command.Get()};queue->ExecuteCommandLists(1,lists);
        CheckHR(queue->Signal(fence.Get(),++signal));
        if(wait&&fence->GetCompletedValue()<signal){CheckHR(fence->SetEventOnCompletion(signal,event));Check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"Phoenix media conversion timed out");}
        CheckHR(device->GetDeviceRemovedReason());
    }
    ComPtr<ID3D12Resource> Image(unsigned w,unsigned h){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;CheckHR(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)));return result;
    }
    void Convert(ID3D12Resource* source,D3D12_RESOURCE_STATES before,ID3D12Resource* output,D3D12_RESOURCE_STATES after,bool decode,bool hdr=false,
        ID3D12Resource* original=nullptr,D3D12_RESOURCE_STATES original_state=D3D12_RESOURCE_STATE_COMMON){
        auto& heap=views[decode?1:0];auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(source,&srv,cpu);
        cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(original,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(decode&&hdr?proxy_input.Get():nullptr,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=srv.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output,nullptr,&uav,cpu);
        Begin();Barrier(command.Get(),source,before,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(original)Barrier(command.Get(),original,original_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(decode&&hdr)Barrier(command.Get(),proxy_input.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(command.Get(),output,after,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* raw=heap.Get();command->SetDescriptorHeaps(1,&raw);command->SetComputeRootSignature(root.Get());command->SetPipelineState(conversion.Get());
        command->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        const unsigned constants[]={decode?output_width:width,decode?output_height:height,unsigned(decode),unsigned(hdr)};
        command->SetComputeRoot32BitConstants(1,4,constants,0);command->Dispatch((constants[0]+15)/16,(constants[1]+15)/16,1);
        Barrier(command.Get(),source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,before);Barrier(command.Get(),output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,after);
        if(original)Barrier(command.Get(),original,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,original_state);
        if(decode&&hdr)Barrier(command.Get(),proxy_input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Submit(true);
    }
};
PhoenixPipelineClient::PhoenixPipelineClient()=default;
PhoenixPipelineClient::~PhoenixPipelineClient()=default;
bool PhoenixPipelineClient::Requested(){return !SelectedRecipePath().empty();}
bool PhoenixPipelineClient::MonitoringRequested()noexcept{
    try{return Requested()&&PlaybackRecovery().active().at("pipeline").value("PipelineMonitoring",0)!=0;}catch(...){return false;}
}
std::string PhoenixPipelineClient::ActiveRecipe(){return Requested()?PlaybackRecovery().active().dump():std::string{};}
bool PhoenixPipelineClient::ResolveOutputExtent(unsigned width,unsigned height,unsigned& output_width,unsigned& output_height){
    try{
        if(!Requested())return true;
        const auto& recipe=PlaybackRecovery().active();
        if(recipe.contains("output_scale")){
            const double scale=recipe.at("output_scale").get<double>();
            Check(std::isfinite(scale)&&scale>=1&&scale<=2,"Phoenix output scale must be between 1x and 2x");
            const auto w=std::lround(width*scale),h=std::lround(height*scale);
            Check(w>0&&h>0&&w<=16384&&h<=16384,"Phoenix scaled output exceeds the supported extent");
            output_width=unsigned(w);output_height=unsigned(h);
        }
        return true;
    }catch(const std::exception& error){LOG("Phoenix output geometry refused: "<<error.what());return false;}
}
std::wstring PhoenixPipelineClient::ModelName()const{
    if(!Available())return {};
    if(state_->recipe.at("runtime").value("spatial_only",false))return L"Processing disabled";
    const auto source=state_->recipe.at("runtime").value("weight_source",Json::object());
    const auto model=state_->recipe.value("model",source.value("model",std::string{}));
    std::wstring label=model=="original"?L"Original":model=="small-fp8"?L"Lightweight":L"Neural model";
    const auto device=phoenix_player::ExecutionDevice(state_->recipe);
    if(device!="native"){
        auto backend=device=="cpu"?std::string("CPU"):device.substr(0,device.find(':'));
        std::transform(backend.begin(),backend.end(),backend.begin(),[](unsigned char c){return char(std::toupper(c));});
        label+=L" · "+std::wstring(backend.begin(),backend.end());
    }
    return label;
}
bool PhoenixPipelineClient::Available()const{return state_&&state_->session;}
bool PhoenixPipelineClient::Initialize(ID3D12Device* device,ID3D12CommandQueue* queue,unsigned width,unsigned height,unsigned output_width,unsigned output_height){
    for(unsigned attempt=0;attempt<2;++attempt){
    try{
        const auto recipe=PlaybackRecovery().active();Check(recipe.at("schema")=="phoenix-media/1","Unsupported Phoenix media recipe");
        const auto library_text=recipe.at("native_library").get<std::string>();
        const auto library=std::filesystem::path(std::u8string(library_text.begin(),library_text.end()));
        Check(library.is_absolute()&&std::filesystem::is_regular_file(library),"Phoenix native library is missing");
        const auto digest=Sha256File(library);
        Check(digest&&*digest==recipe.at("native_library_sha256").get<std::string>(),"Phoenix native library checksum differs");
        auto prepared=std::make_unique<State>();prepared->device=device;prepared->queue=queue;
        prepared->recipe=recipe;
        prepared->width=width;prepared->height=height;prepared->output_width=output_width;prepared->output_height=output_height;
        prepared->module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(prepared->module!=nullptr,"Cannot load Phoenix native library");
        prepared->Resolve(prepared->create,"phoenix_native_create");prepared->Resolve(prepared->destroy,"phoenix_native_destroy");
        prepared->Resolve(prepared->frame,"phoenix_native_frame_d3d12");prepared->Resolve(prepared->defaults,"phoenix_native_default_controls");
        prepared->RequireExecutionContract(recipe.at("runtime"));
        prepared->Resolve(prepared->identity,"phoenix_native_set_frame_info");prepared->Resolve(prepared->status,"phoenix_native_status");prepared->Resolve(prepared->error,"phoenix_native_error");
        prepared->tuning=reinterpret_cast<decltype(prepared->tuning)>(GetProcAddress(prepared->module,"phoenix_native_set_tuning"));
        prepared->latency=reinterpret_cast<decltype(prepared->latency)>(GetProcAddress(prepared->module,"phoenix_native_set_latency"));
        prepared->reconfigure=reinterpret_cast<decltype(prepared->reconfigure)>(GetProcAddress(prepared->module,"phoenix_native_reconfigure"));
        auto config=recipe.at("runtime");config["width"]=width;config["height"]=height;config["output_width"]=output_width;config["output_height"]=output_height;
        config["pipeline"]=recipe.at("pipeline");config["engine_motion_valid"]=false;
        if(recipe.contains("tuning"))config["tuning"]=recipe.at("tuning");
        prepared->session=prepared->create(config.dump().c_str(),device,queue);Check(prepared->session!=nullptr,prepared->error());
        CheckHR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&prepared->allocator)));
        CheckHR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,prepared->allocator.Get(),nullptr,IID_PPV_ARGS(&prepared->command)));CheckHR(prepared->command->Close());
        CheckHR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&prepared->fence)));prepared->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(prepared->event!=nullptr,"Phoenix conversion event allocation failed");
        prepared->proxy_input=prepared->Image(width,height);prepared->proxy_output=prepared->Image(output_width,output_height);
        D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,3}};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,4};
        D3D12_ROOT_SIGNATURE_DESC description{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> signature,code,errors;
        CheckHR(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors));
        CheckHR(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&prepared->root)));
        const auto shader=std::string(phoenix_pipeline::hdr_bridge_shader)+ConversionShader;
        const HRESULT compiled=D3DCompile(shader.data(),shader.size(),"phoenix_media",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(compiled))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Phoenix media conversion shader failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=prepared->root.Get();pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
        CheckHR(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&prepared->conversion)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        for(auto& views:prepared->views)CheckHR(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&views)));
        state_=std::move(prepared);error_.clear();LOG("Phoenix direct native pipeline initialized: "<<Status());return true;
    }catch(const std::exception& error){
        error_=error.what();LOG("Phoenix media initialization failed: "<<error_);
        try{const auto prior=PlaybackRecovery().active();const auto restored=PlaybackRecovery().failure(error_);
            if(attempt==0&&restored&&*restored!=prior&&SUCCEEDED(device->GetDeviceRemovedReason())){LOG("Restoring last working Phoenix recipe during initialization");continue;}}
        catch(const std::exception& recovery_error){LOG("Phoenix initialization recovery failed: "<<recovery_error.what());}
        return false;
    }}return false;
}
bool PhoenixPipelineClient::Evaluate(ID3D12Resource* source,D3D12_RESOURCE_STATES source_state,ID3D12Resource* output,D3D12_RESOURCE_STATES output_state,bool reset,const FrameIdentity* frame_identity,bool hdr_source,uint64_t source_ready_ns,uint64_t source_due_ns){
    try{
        Check(Available(),"Phoenix media pipeline unavailable");
        auto& state=*state_;PhoenixNativeControls controls{};state.defaults(&controls);controls.reset=reset||hdr_source!=state.hdr_source;
        if(frame_identity){
            Check(frame_identity->pts100ns<=INT64_MAX/100&&frame_identity->pts100ns>=INT64_MIN/100,"Phoenix timestamp overflow");
            PhoenixNativeFrameInfo info{sizeof(info),1,frame_identity->sourceGeneration,frame_identity->frameNumber,frame_identity->pts100ns*100,0,0};
            const int described=state.identity(state.session,&info);Check(described==0,state.error());
        }
        state.Convert(source,source_state,state.proxy_input.Get(),D3D12_RESOURCE_STATE_COMMON,false,hdr_source);
        const bool monitored=state.recipe.at("pipeline").value("PipelineMonitoring",0)!=0;
        const auto submitted_ns=monitored?uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0;
        state.source_ready_ns=source_ready_ns;state.source_due_ns=source_due_ns;state.pending_presentation=false;
        if(monitored&&state.latency&&(source_ready_ns||source_due_ns)){
            // Decode prefetch and a deliberate pause are planned holds, not
            // processing overload. Only time after both readiness and the
            // media deadline feeds the shared quality controller.
            const auto eligible=std::max(source_ready_ns,source_due_ns);
            const double age=submitted_ns>=eligible?double(submitted_ns-eligible)*1e-6:0;
            Check(state.latency(state.session,std::min(age,60000.),std::min(state.presentation_tail_ms,60000.))==0,state.error());
        }
        const int result=state.frame(state.session,state.proxy_input.Get(),D3D12_RESOURCE_STATE_COMMON,nullptr,D3D12_RESOURCE_STATE_COMMON,state.proxy_output.Get(),D3D12_RESOURCE_STATE_COMMON,&controls);
        Check(result==0,state.error());
        if(monitored){state.sdk_finished_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());state.pending_presentation=true;state.sdk_processing_ms=double(state.sdk_finished_ns-submitted_ns)*1e-6;}
        state.Convert(state.proxy_output.Get(),D3D12_RESOURCE_STATE_COMMON,output,output_state,true,hdr_source,hdr_source?source:nullptr,source_state);
        state.hdr_source=hdr_source;
        state.completed_recipe=state.recipe;
        state.completed_frame=frame_identity?Json{{"frame_number",frame_identity->frameNumber},
            {"pts_100ns",frame_identity->pts100ns},{"source_generation",frame_identity->sourceGeneration},
            {"history_generation",frame_identity->historyGeneration},{"job_id",frame_identity->jobId},
            {"reset",std::string(HistoryResetName(frame_identity->reset))}}:Json(nullptr);
        ++evaluations_;error_.clear();
        try{PlaybackRecovery().completed();}catch(const std::exception& recovery_error){LOG("Phoenix health record could not be saved: "<<recovery_error.what());}
        return true;
    }catch(const std::exception& error){error_=error.what();LOG("Phoenix media frame failed: "<<error_);
        try{PlaybackRecovery().failure(error_);}catch(const std::exception& recovery_error){LOG("Phoenix recovery record failed: "<<recovery_error.what());}return false;}
}
void PhoenixPipelineClient::NotePresented(){
    if(!Available()||state_->recipe.at("pipeline").value("PipelineMonitoring",0)==0)return;
    auto& state=*state_;const auto now=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool reused=!state.pending_presentation;
    if(state.pending_presentation){state.presentation_tail_ms=double(now-state.sdk_finished_ns)*1e-6;state.pending_presentation=false;}
    state.playback_telemetry={{"sdk_to_present_return_ms",state.presentation_tail_ms},{"source_age_available",state.source_ready_ns!=0&&now>=state.source_ready_ns},
        {"latency_feedback_available",state.latency!=nullptr},{"scope","decoded frame readiness through Present API return; excludes prior source capture/decode and physical display scanout"}};
    if(state.source_ready_ns&&now>=state.source_ready_ns)state.playback_telemetry["source_ready_to_present_return_ms"]=double(now-state.source_ready_ns)*1e-6;
    state.playback_telemetry["presentation_reuses_completed_frame"]=reused;
    if(!reused&&state.source_due_ns)state.playback_telemetry["presentation_deadline_lateness_ms"]=now>state.source_due_ns?double(now-state.source_due_ns)*1e-6:0;
    state.playback_telemetry["planned_media_hold_excluded_from_budget"]=state.source_due_ns!=0;
    wchar_t text[160]{};
    if(state.source_ready_ns&&now>=state.source_ready_ns)swprintf_s(text,L"SDK %.2f ms \u00b7 decoded age %.1f ms",state.sdk_processing_ms,double(now-state.source_ready_ns)*1e-6);
    else swprintf_s(text,L"SDK %.2f ms",state.sdk_processing_ms);
    state.telemetry_text=text;
}
std::wstring PhoenixPipelineClient::TelemetryText()const{
    return Available()&&state_->recipe.at("pipeline").value("PipelineMonitoring",0)?state_->telemetry_text:std::wstring{};
}
bool PhoenixPipelineClient::SetTuning(const std::string& controls){
    try{
        Check(Available(),"Phoenix media pipeline unavailable");Check(state_->tuning!=nullptr,"Update the Phoenix native runtime to use live neural controls");
        auto candidate=state_->recipe.value("tuning",Json::object());candidate.update(Json::parse(controls));
        auto recipe=state_->recipe;recipe["tuning"]=candidate;PlaybackRecovery().begin(recipe);
        if(state_->tuning(state_->session,candidate.dump().c_str())!=0){
            const std::string failure=state_->error();PlaybackRecovery().failure(failure);PlaybackRecovery().begin(state_->recipe);throw std::runtime_error(failure);
        }
        state_->recipe=std::move(recipe);error_.clear();return true;
    }catch(const std::exception& error){error_=error.what();LOG("Phoenix controls refused: "<<error_);return false;}
}
bool PhoenixPipelineClient::ApplySettings(const std::string& pipeline,const std::string& tuning,const std::string& device){
    try{
        Check(Available(),"Phoenix media pipeline unavailable");
        auto candidate=phoenix_player::SelectExecution(state_->recipe,device);candidate["pipeline"]=Json::parse(pipeline);candidate["tuning"]=Json::parse(tuning);
        return ApplyRecipe(candidate.dump());
    }catch(const std::exception& error){error_=error.what();LOG("Phoenix chain settings refused: "<<error_);return false;}
}
bool PhoenixPipelineClient::ApplyRecipe(const std::string& document){
    try{
        Check(Available(),"Phoenix media pipeline unavailable");Check(state_->reconfigure!=nullptr,"Update the Phoenix native runtime to edit its processing chain");
        auto candidate=Json::parse(document);
        state_->RequireExecutionContract(candidate.at("runtime"));
        Check(candidate.at("native_library")==state_->recipe.at("native_library")&&candidate.at("native_library_sha256")==state_->recipe.at("native_library_sha256"),"Runtime identity changed; relaunch the player for the selected package");
        auto config=candidate.at("runtime");config["width"]=state_->width;config["height"]=state_->height;
        config["output_width"]=state_->output_width;config["output_height"]=state_->output_height;
        config["pipeline"]=candidate.at("pipeline");if(candidate.contains("tuning"))config["tuning"]=candidate.at("tuning");config["engine_motion_valid"]=false;
        PlaybackRecovery().begin(candidate);
        if(state_->reconfigure(&state_->session,config.dump().c_str())!=0){
            const std::string failure=state_->error();PlaybackRecovery().failure(failure);PlaybackRecovery().begin(state_->recipe);throw std::runtime_error(failure);
        }
        state_->recipe=std::move(candidate);error_.clear();return true;
    }catch(const std::exception& error){error_=error.what();LOG("Phoenix chain settings refused: "<<error_);return false;}
}
bool PhoenixPipelineClient::RestoreLaunchDefaults(){return ApplyRecipe(BaseRecipe().dump());}
bool PhoenixPipelineClient::SavePreferences(){
    try{
        Check(Available(),"Phoenix media pipeline unavailable");
        const auto document=phoenix_player::PreferenceDocument(BaseRecipe(),state_->recipe);
        if(document==state_->saved_preferences)return true;
        phoenix_player::SavePreferences(PreferencesPath(),BaseRecipe(),state_->recipe);state_->saved_preferences=document;return true;
    }catch(const std::exception& error){LOG("Phoenix preferences could not be saved: "<<error.what());return false;}
}
std::string PhoenixPipelineClient::Status()const{
    if(!Available())return error_;const size_t bytes=state_->status(state_->session,nullptr,0);if(!bytes)return "{}";
    std::string text(bytes,'\0');state_->status(state_->session,text.data(),text.size());text.resize(bytes-1);
    auto report=Json::parse(text);report["media_frame"]=state_->completed_frame;
    report["completed_media_recipe"]=state_->completed_recipe;
    report["media_recipe"]=state_->recipe;
    report["playback_telemetry"]=state_->recipe.at("pipeline").value("PipelineMonitoring",0)?state_->playback_telemetry:Json{{"monitoring",0}};
    report["media_evaluations"]=evaluations_;
    report["media_color_domain"]=state_->hdr_source?"linear BT.709 HDR / reversible gamut-magnitude proxy":"linear BT.709 SDR / sRGB proxy";
    return report.dump();
}
