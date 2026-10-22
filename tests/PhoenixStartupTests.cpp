#include "PhoenixRecipe.h"
#include "UiLayout.h"
#include "PhoenixPlaybackRecovery.h"
#include "PhoenixPreferences.h"
#include "PhoenixMediaCache.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <source_location>

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Pass an empty temporary test directory");
        const std::filesystem::path root=argv[1],app=root/L"app",local=root/L"local";
        std::filesystem::create_directories(app);
        const auto central=local/L"DLSS5-Everything"/L"Media"/L"recipe.json";
        const auto portable=app/L"phoenix-media.json";
        auto check=[](bool ok,std::source_location where=std::source_location::current()){if(!ok)throw std::runtime_error("Phoenix startup/status regression at line "+std::to_string(where.line()));};
        check(SelectPhoenixRecipe({},app,local).empty());
        std::filesystem::create_directories(central.parent_path());
        std::ofstream(central)<<"{}";
        check(SelectPhoenixRecipe({},app,local)==central);
        std::ofstream(portable)<<"{}";
        check(SelectPhoenixRecipe({},app,local)==portable);
        const auto missing=root/L"missing-explicit.json";
        check(SelectPhoenixRecipe(missing,app,local)==missing);
        check(SelectPhoenixRecipe({},app,{})==portable);
        PlayerStatusSnapshot status{};status.mediaLoaded=true;
        status.runtimeConfiguration=PlayerRuntimeConfiguration::PhoenixDirect;
        status.sourceWidth=status.outputWidth=2560;status.sourceHeight=status.outputHeight=1440;
        status.phoenixModel=L"Lightweight";status.upscalingStatus=L"Scaling off";
        const auto processed=BuildPlayerStatusText(status);
        check(processed.starts_with(L"Phoenix \u00b7 Lightweight \u00b7 Processed view"));
        check(processed.find(L"unavailable")==std::wstring::npos&&processed.find(L"DLAA")==std::wstring::npos);
        status.phoenixModel=L"Original";status.phoenixOriginalView=true;
        check(BuildPlayerStatusText(status).starts_with(L"Phoenix \u00b7 Original \u00b7 Original view"));
        status.runtimeConfiguration=PlayerRuntimeConfiguration::NeuralAddonUnavailable;
        check(BuildPlayerStatusText(status).starts_with(L"Neural addon unavailable"));
        using Json=nlohmann::json;using phoenix_player::PlaybackRecovery;
        const Json first{{"schema","phoenix-media/1"},{"model","original"},{"runtime",{{"weight_source","fixture"}}},{"pipeline",Json::object()}};
        auto small=first;small["model"]="small-fp8";
        const auto journal=root/L"playback-recovery.json";
        {PlaybackRecovery recovery(first,journal);check(recovery.active()==first);for(int i=0;i<30;++i)recovery.completed();recovery.begin(small);}
        {PlaybackRecovery recovery(small,journal);check(recovery.active()==first);}
        {PlaybackRecovery recovery(small,journal);check(recovery.active()==small);check(recovery.failure("Injected provider failure")==first);}
        {PlaybackRecovery recovery(first,journal);auto display=first;display["tuning"]={{"intensity",.3},{"network_passes",1}};recovery.begin(display);
            std::ifstream input(journal);Json saved;input>>saved;check(saved.at("pending").is_null());}
        {std::ifstream input(journal);Json saved;input>>saved;input.close();saved["requested"]={{"Backend",0},{"NeuralUplift",false}};
            {std::ofstream output(journal);output<<saved.dump();}
            PlaybackRecovery recovery(first,journal);check(recovery.active().at("runtime").at("spatial_only")==true);}
        const auto preferences=std::filesystem::absolute(root/L"native-preferences.json");
        auto tuned=first;tuned["tuning"]={{"intensity",.4},{"network_passes",2}};tuned["pipeline"]["ChainBrightness"]=.1;
        auto portableRecipe=first;portableRecipe["portable_package"]={{"runtime_root","fixture"},{"runtime_sha256","pinned"}};
        portableRecipe["execution_devices"]=Json::array({{{"id","vulkan:fixture"},{"label","Vulkan fixture"}}});
        const auto cpu=phoenix_player::SelectExecution(portableRecipe,"cpu");
        check(cpu.at("runtime").at("portable_runtime").at("backend")=="cpu");
        check(phoenix_player::SelectExecution(cpu,"native").at("runtime")==first.at("runtime"));
        const auto vk=phoenix_player::SelectExecution(portableRecipe,"vulkan:fixture");
        check(vk.at("runtime").at("portable_runtime").at("runtime_sha256")=="pinned");
        bool refused_device=false;try{phoenix_player::SelectExecution(portableRecipe,"cuda:unavailable");}catch(const std::invalid_argument&){refused_device=true;}
        check(refused_device);
        const auto device_preferences=std::filesystem::absolute(root/L"device-preferences.json");
        phoenix_player::SavePreferences(device_preferences,portableRecipe,cpu);
        check(phoenix_player::LoadPreferences(device_preferences,portableRecipe).at("runtime")==cpu.at("runtime"));
        phoenix_player::SavePreferences(preferences,first,tuned);const auto loaded=phoenix_player::LoadPreferences(preferences,first);
        check(loaded.at("runtime")==first.at("runtime")&&loaded.at("tuning").at("network_passes")==2);
        bool mismatch=false;try{phoenix_player::LoadPreferences(preferences,small);}catch(const std::invalid_argument&){mismatch=true;}check(mismatch);
        const auto cache=root/L"native-cache",entry=cache/std::wstring(64,L'a');std::filesystem::create_directories(entry);
        {std::ofstream file(entry/L"manifest.json");file<<Json{{"schema","phoenix-media-job/1"},{"segments",Json::array()}}.dump();}
        {std::ofstream file(entry/L"job.lock");file<<"";}{std::ofstream file(entry/L"render.mkv");file<<"fixture";}
        check(phoenix_media_cache::Maintain(cache,0,false).removed==0);
        auto reader=phoenix_media_cache::Lease::Acquire(entry),second_reader=phoenix_media_cache::Lease::Acquire(entry);
        check(phoenix_media_cache::Maintain(cache,0,true).deferred==1&&std::filesystem::exists(entry/L"render.mkv"));
        reader.reset();check(phoenix_media_cache::Maintain(cache,0,true).removed==0);second_reader.reset();
        phoenix_media_cache::Handle active_job;active_job.value=CreateFileW((entry/L"job.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(active_job.value!=INVALID_HANDLE_VALUE&&phoenix_media_cache::Maintain(cache,UINT64_MAX,false).removed==0);active_job.close();
        {std::ofstream file(entry/L"user-file.txt");file<<"preserve";}
        check(phoenix_media_cache::Maintain(cache,0,true).removed==0&&std::filesystem::exists(entry/L"user-file.txt"));
        std::filesystem::remove(entry/L"user-file.txt");check(phoenix_media_cache::Maintain(cache,UINT64_MAX,false).removed==1&&!std::filesystem::exists(entry));
        std::cout<<"Phoenix startup, preferences and active-cache protection passed.\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
