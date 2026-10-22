#pragma once
#include <windows.h>
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"
#include <functional>
#include "PhoenixMediaCache.h"

struct PhoenixMediaSegment {
    std::filesystem::path path;
    uint64_t index=0,firstFrame=0,frames=0;
    int64_t firstTimestamp100ns=0,end100ns=0;
};

struct PhoenixMediaJob {
    std::filesystem::path source, cacheRoot, output, helpers;
    // An immutable recipe snapshot, not a file another player may rewrite.
    std::string recipe;
    uint32_t outputWidth=0,outputHeight=0,segmentFrames=120;
    NeuralRenderRange range;
    EncoderQuality quality=EncoderQuality::Standard;
    // Player cache requests authenticate a job-owned assembled carrier. They
    // never publish into the legacy Feature18 cache or overwrite user exports.
    bool cachedPlayback=false,lookupOnly=false;
    HANDLE pauseEvent=nullptr;
    uint32_t firstSegmentFrames=0;
    std::function<void(const PhoenixMediaSegment&)> segmentReady;
};
struct PhoenixMediaJobResult {
    bool ok=false,cancelled=false;
    std::wstring detail;
    uint64_t frames=0,replayedFrames=0,reusedFrames=0;
    std::filesystem::path receipt;
    std::filesystem::path payload;
    bool cacheHit=false,preparedOnly=false;
    uint32_t width=0,height=0;
    std::shared_ptr<phoenix_media_cache::Lease> playbackLease;
};
// Video-only native Phoenix carrier. The existing final mux attaches source
// audio, subtitles and chapters. Each completed segment is authenticated before
// reuse. Resume rebuilds temporal state by replaying every preceding source frame.
PhoenixMediaJobResult RunPhoenixMediaJob(const PhoenixMediaJob&,std::stop_token,
    const std::function<void(uint64_t,uint64_t)>& progress={});
