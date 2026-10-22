#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include "FrameIdentity.h"

// Direct Phoenix SDK adapter. It has no RenoDX/ReShade or Python dependency.
// Its synchronous boundary is deliberately explicit; the renderer flushes its
// producer before Evaluate and resumes recording after the completed result.
class PhoenixPipelineClient {
    struct State;
    std::unique_ptr<State> state_;
    std::string error_;
    uint64_t evaluations_=0;
    bool ApplyRecipe(const std::string& recipe);
public:
    PhoenixPipelineClient();
    ~PhoenixPipelineClient();
    static bool Requested();
    static bool MonitoringRequested() noexcept;
    static std::string ActiveRecipe();
    static bool ResolveOutputExtent(unsigned source_width,unsigned source_height,unsigned& output_width,unsigned& output_height);
    bool Initialize(ID3D12Device*,ID3D12CommandQueue*,unsigned width,unsigned height,unsigned output_width,unsigned output_height);
    bool Evaluate(ID3D12Resource* linear_source,D3D12_RESOURCE_STATES source_state,
        ID3D12Resource* linear_output,D3D12_RESOURCE_STATES output_state,bool reset,
        const FrameIdentity* identity,bool hdr_source,uint64_t source_ready_ns=0,uint64_t source_due_ns=0);
    void NotePresented();
    std::wstring TelemetryText() const;
    bool Available() const;
    bool SetTuning(const std::string& controls);
    bool ApplySettings(const std::string& pipeline,const std::string& tuning,const std::string& device={});
    bool SavePreferences();
    bool RestoreLaunchDefaults();
    std::wstring ModelName() const;
    uint64_t EvaluationCount() const { return evaluations_; }
    const std::string& Error() const { return error_; }
    std::string Status() const;
};
