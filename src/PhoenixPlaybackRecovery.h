#pragma once
#include "pipeline/recovery.h"
#include <memory>

namespace phoenix_player {
using Json=nlohmann::json;
inline Json RuntimeSignature(const Json& recipe){
    Json result;
    for(const auto key:{"native_library","native_library_sha256","model","runtime","output_scale"})if(recipe.contains(key))result[key]=recipe[key];
    const auto pipeline=recipe.value("pipeline",Json::object());
    for(const auto& field:pipeline.items()){
        const auto& key=field.key();
        if(key.starts_with("Chain")||key.starts_with("Fusion")||key.starts_with("Motion")||key=="ProcessingChain"||key=="AdditionalScaler"||key=="MultiscaleFusion"||key=="FlowAnalysisHeight")
            result["pipeline"][key]=field.value();
    }
    const auto tuning=recipe.value("tuning",Json::object());
    const Json defaults{{"network_passes",1},{"neural_scale",100},{"central_blocks",8},{"repeats",{1,1,1,1,1,1,1,1}}};
    for(const auto key:{"network_passes","neural_scale","central_blocks","repeats"})result["tuning"][key]=tuning.contains(key)?tuning[key]:defaults[key];
    return result;
}
inline Json SafeRecipe(Json recipe){
    recipe["model"]="none";
    recipe["runtime"]["spatial_only"]=true;recipe["runtime"].erase("weight_source");recipe["runtime"].erase("tiny_model_path");
    recipe.erase("tuning");recipe["pipeline"]={{"PipelineMonitoring",0},{"StartupRecovery",true}};
    return recipe;
}
class PlaybackRecovery {
    std::unique_ptr<phoenix_pipeline::RecoveryJournal> journal_;
    Json active_;
public:
    PlaybackRecovery(const Json& requested,const std::filesystem::path& path):active_(requested){
        const bool automatic=requested.at("pipeline").value("StartupRecovery",true);
        std::error_code file_error;
        if(!automatic&&!std::filesystem::is_regular_file(path,file_error))return;
        journal_=std::make_unique<phoenix_pipeline::RecoveryJournal>(path,SafeRecipe(requested));
        const auto previous=journal_->report();
        const bool same_interrupted=previous.at("pending").is_object()&&RuntimeSignature(previous.at("pending"))==RuntimeSignature(requested);
        const bool restore_without_pending=previous.at("pending").is_null()&&previous.value("restore_required",!previous.value("reason",std::string{}).empty());
        if(automatic)if(const auto working=journal_->recover_interrupted();working&&(same_interrupted||restore_without_pending))active_=*working;
        const auto requested_settings=journal_->take_requested();
        if(requested_settings){
            if(requested_settings->value("schema",std::string{})=="phoenix-media/1")active_=*requested_settings;
            else if(requested_settings->value("Backend",3)==0)active_=SafeRecipe(active_);
        }
        if(!automatic&&!requested_settings){journal_.reset();return;}
        begin(active_);
    }
    const Json& active()const{return active_;}
    void begin(const Json& candidate){
        if(journal_){const auto& current=journal_->pending()?journal_->report().at("pending"):journal_->working();
            if(RuntimeSignature(current)!=RuntimeSignature(candidate))journal_->begin(candidate);}
        active_=candidate;
    }
    std::optional<Json> failure(const std::string& reason){
        if(!journal_)return std::nullopt;
        auto fallback=journal_->failure(reason);if(fallback)active_=*fallback;return fallback;
    }
    void completed(){if(journal_)journal_->completed_frame();}
};
}
