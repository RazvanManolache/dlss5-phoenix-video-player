#pragma once
#include "nlohmann/json.hpp"
#include <string>
#include <stdexcept>

namespace phoenix_player {
inline std::string ExecutionDevice(const nlohmann::json& recipe){
    const auto runtime=recipe.value("runtime",nlohmann::json::object());
    return runtime.contains("portable_runtime")?runtime.at("portable_runtime").at("device").get<std::string>():"native";
}
inline nlohmann::json SelectExecution(nlohmann::json recipe,const std::string& device){
    if(device.empty()||device==ExecutionDevice(recipe))return recipe;
    if(device=="native"){recipe["runtime"].erase("portable_runtime");return recipe;}
    if(!recipe.contains("portable_package"))throw std::invalid_argument("Install the complete portable player package first.");
    bool available=device=="cpu";
    for(const auto& entry:recipe.value("execution_devices",nlohmann::json::array()))
        available=available||entry.value("id",std::string{})==device;
    if(!available)throw std::invalid_argument("The selected processing device is not in this launch's native device inventory.");
    const auto separator=device.find(':');const auto backend=device=="cpu"?"cpu":device.substr(0,separator);
    if(backend!="cpu"&&backend!="cuda"&&backend!="vulkan"&&backend!="hip"&&backend!="sycl")
        throw std::invalid_argument("Unsupported portable processing device.");
    recipe["runtime"]["portable_runtime"]=recipe.at("portable_package");
    recipe["runtime"]["portable_runtime"]["backend"]=backend;
    recipe["runtime"]["portable_runtime"]["device"]=device;
    return recipe;
}
}
