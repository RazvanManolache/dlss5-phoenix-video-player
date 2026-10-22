#include "WasapiRenderer.h"
#include "nlohmann/json.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
struct WasapiRendererTestAccess {
    static ComPtr<IAudioStreamVolume> Stream(WasapiRenderer& renderer){
        std::lock_guard lock(renderer.mutex_);ComPtr<IAudioStreamVolume> value;
        if(FAILED(renderer.client_->GetService(IID_PPV_ARGS(&value))))throw std::runtime_error("Stream volume unavailable");
        return value;
    }
    static ComPtr<ISimpleAudioVolume> Session(WasapiRenderer& renderer){
        std::lock_guard lock(renderer.mutex_);ComPtr<ISimpleAudioVolume> value;
        if(FAILED(renderer.client_->GetService(IID_PPV_ARGS(&value))))throw std::runtime_error("Session volume unavailable");
        return value;
    }
};
namespace {
float SessionLevel(ISimpleAudioVolume* volume){float result=0;if(FAILED(volume->GetMasterVolume(&result)))throw std::runtime_error("Cannot read session level");return result;}
std::vector<float> Levels(IAudioStreamVolume* volume){
    UINT32 channels=0;if(FAILED(volume->GetChannelCount(&channels)))throw std::runtime_error("Cannot read stream channels");
    std::vector<float> result(channels);if(FAILED(volume->GetAllVolumes(channels,result.data())))throw std::runtime_error("Cannot read stream levels");return result;
}
bool AllNear(const std::vector<float>& values,float wanted){
    if(values.empty())return false;for(float value:values)if(std::abs(value-wanted)>0.00001f)return false;return true;
}
}
int main(){
    Json report{{"passed",false},{"endpoint_started",false}};int failures=0;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com)){std::cerr<<"COM initialization failed\n";return 1;}
    try{
        // No buffers are written and neither endpoint is started. This observes
        // the real mixer state without playing sound or touching other apps.
        WasapiRenderer playing; if(!playing.Open())throw std::runtime_error("Cannot open render endpoint");
        auto session=WasapiRendererTestAccess::Session(playing);
        const float initial=SessionLevel(session.Get());
        BOOL initial_mute=FALSE;session->GetMute(&initial_mute);
        struct Restore{ISimpleAudioVolume* session;float initial;BOOL mute;~Restore(){session->SetMasterVolume(initial,nullptr);session->SetMute(mute,nullptr);}} restore{session.Get(),initial,initial_mute};
        auto stream=WasapiRendererTestAccess::Stream(playing);
        playing.SetVolume(0);
        report["active_gain_when_muted"]=Levels(stream.Get());
        report["shared_session_before_prepare"]=SessionLevel(session.Get());
        WasapiRenderer prepared;if(!prepared.Open())throw std::runtime_error("Cannot open prepared endpoint");
        auto next=WasapiRendererTestAccess::Stream(prepared);
        prepared.SetVolume(1);
        report["active_gain_after_prepare"]=Levels(stream.Get());
        report["shared_session_after_prepare"]=SessionLevel(session.Get());
        failures+=!AllNear(Levels(stream.Get()),0);
        failures+=std::abs(SessionLevel(session.Get())-initial)>0.00001f;
        playing.SetVolume(0.37f);prepared.SetVolume(0.82f);
        report["active_gain_after_prepared_volume_change"]=Levels(stream.Get());
        failures+=!AllNear(Levels(stream.Get()),0.37f);
        failures+=!AllNear(Levels(next.Get()),0.82f);
        prepared.SetVolume(0);playing.SetVolume(0.65f);
        failures+=!AllNear(Levels(next.Get()),0);
        failures+=!AllNear(Levels(stream.Get()),0.65f);
        playing.Close();
        report["prepared_gain_after_handoff"]=Levels(next.Get());
        failures+=!AllNear(Levels(next.Get()),0);
        session->SetMasterVolume(0,nullptr);
        failures+=!prepared.MigrateLegacySessionGain(0);
        report["legacy_zero_gain_after_upgrade"]=SessionLevel(session.Get());
        failures+=std::abs(SessionLevel(session.Get())-1)>0.00001f;
        failures+=!AllNear(Levels(next.Get()),0);
        session->SetMasterVolume(0.42f,nullptr);
        failures+=!prepared.MigrateLegacySessionGain(0);
        failures+=std::abs(SessionLevel(session.Get())-0.42f)>0.00001f;
        report["different_windows_gain_preserved"]=SessionLevel(session.Get());
        session->SetMute(TRUE,nullptr);session->SetMasterVolume(0,nullptr);
        failures+=!prepared.MigrateLegacySessionGain(0);
        BOOL os_muted=FALSE;session->GetMute(&os_muted);
        failures+=os_muted!=TRUE;
        report["windows_mute_preserved"]=os_muted==TRUE;
        report["failures"]=failures;report["passed"]=failures==0;
    }catch(const std::exception& error){report["error"]=error.what();failures=1;}
    CoUninitialize();std::cout<<report.dump()<<'\n';return failures?1:0;
}
