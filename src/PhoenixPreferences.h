#pragma once
#include "NeuralCache.h"
#include "AtomicFile.h"
#include "PhoenixExecution.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>

namespace phoenix_player {
using Json=nlohmann::json;
inline std::string PreferenceIdentity(const Json& base){
    const auto digest=Sha256Bytes(base.dump());if(!digest)throw std::runtime_error("Cannot identify the Phoenix launch recipe");return *digest;
}
inline void PreferencePath(const std::filesystem::path& path){
    if(!path.is_absolute())throw std::invalid_argument("Phoenix preferences require an absolute path");
    for(auto current=path;!current.empty();){const auto attributes=GetFileAttributesW(current.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::invalid_argument("Phoenix preference paths cannot traverse links");
        const auto parent=current.parent_path();if(parent==current)break;current=parent;
    }
}
inline Json LoadPreferences(const std::filesystem::path& path,const Json& base){
    PreferencePath(path);if(!std::filesystem::is_regular_file(path))return base;
    if(std::filesystem::file_size(path)>65536)throw std::invalid_argument("Phoenix preferences are oversized");
    std::ifstream file(path);const auto saved=Json::parse(file);
    if(saved.at("schema")!="phoenix-media-preferences/1"||saved.at("base_sha256")!=PreferenceIdentity(base)||
        !saved.at("pipeline").is_object()||!saved.at("tuning").is_object())throw std::invalid_argument("Phoenix preferences do not match this launch recipe");
    auto result=base;result["pipeline"]=saved.at("pipeline");result["tuning"]=saved.at("tuning");
    if(saved.contains("execution_device"))result=SelectExecution(std::move(result),saved.at("execution_device").get<std::string>());
    if(saved.value("processing_disabled",false)){result["runtime"]["spatial_only"]=true;result["model"]="none";}
    return result;
}
inline Json PreferenceDocument(const Json& base,const Json& applied){
    return {{"schema","phoenix-media-preferences/1"},{"base_sha256",PreferenceIdentity(base)},
        {"pipeline",applied.at("pipeline")},{"tuning",applied.value("tuning",Json::object())},
        {"execution_device",ExecutionDevice(applied)},
        {"processing_disabled",applied.at("runtime").value("spatial_only",false)}};
}
inline void SavePreferences(const std::filesystem::path& path,const Json& base,const Json& applied){
    PreferencePath(path);const auto bytes=PreferenceDocument(base,applied).dump(2);
    if(bytes.size()>65536)throw std::invalid_argument("Phoenix preferences exceed the size limit");
    std::filesystem::create_directories(path.parent_path());
    if(!atomic_file::Replace(path,bytes))throw std::runtime_error("Cannot save Phoenix processing preferences");
}
}
