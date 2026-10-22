#pragma once
#include <filesystem>

// Explicit launches win, followed by a portable setup beside the player and
// Everything's last saved media setup. A broken explicit choice must fail;
// silently substituting another model would make comparisons misleading.
inline std::filesystem::path SelectPhoenixRecipe(
    const std::filesystem::path& explicit_path,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& local_app_data)
{
    if (!explicit_path.empty()) return explicit_path;
    const auto portable = executable_directory / L"phoenix-media.json";
    if (std::filesystem::exists(portable)) return portable;
    if (!local_app_data.empty()) {
        const auto shared = local_app_data / L"DLSS5-Everything" / L"Media" / L"recipe.json";
        if (std::filesystem::exists(shared)) return shared;
    }
    return {};
}
