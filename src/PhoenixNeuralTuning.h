#pragma once
#include "NeuralSettings.h"
#include "nlohmann/json.hpp"

// Only appearance controls are shared with the legacy UI. Independent Phoenix
// stage histories are mandatory; RenoDX's chained-history switch has no native
// equivalent and must not silently disable temporal processing.
inline nlohmann::json PhoenixNeuralTuning(const NeuralSettings& s){
    return {{"intensity",s.intensity},{"local_tone",s.localTone},{"local_structure",s.localStructure},
        {"skin_structure",s.skinStructure},{"color_strength",s.colorStrength},{"style",s.style},
        {"network_passes",s.passes},{"auto_mask",s.autoMask}};
}
