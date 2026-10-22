// Exercises the actual player handoff with local clips and an offscreen window.
// Run a disposable copy of this executable: PlayerApp saves beside its module.
#include "../src/Main.cpp"
#include <iostream>
#include <source_location>

namespace {
void check(bool ok,std::source_location where=std::source_location::current()){
    if(!ok)throw std::runtime_error("Live playback regression at line "+std::to_string(where.line()));
}
}

struct PlayerAppTestAccess {
    static void decoderSwap(const std::filesystem::path& source){
        VideoDecoder measured,ordinary;
        measured.SetLatencyMonitoring(true);
        check(measured.SetOutputExtent(128,80));
        check(measured.OpenSequential(source.wstring()));
        check(ordinary.OpenSequential(source.wstring()));
        measured.Swap(ordinary);
        VideoFrame unmeasured,timed;
        check(measured.ReadNext(unmeasured));check(ordinary.ReadNext(timed));
        check(unmeasured.readyTimeNs==0&&timed.readyTimeNs!=0);
        check(measured.Width()==64&&ordinary.Width()==128);
        for(bool enabled:{false,true}){
            YouTubeCompletion prepared;prepared.result.ok=true;prepared.result.mediaUrl=source.wstring();
            prepared.measureLatency=enabled;
            PlayerApp::PrepareYouTubeMedia(prepared,{},0,0,false,DefaultNeuralCarrierQuality());
            check(prepared.mediaErrorKey.empty()&&!prepared.firstFrame.bgra.empty());
            check((prepared.firstFrame.readyTimeNs!=0)==enabled);
        }
        check(measured.OpenMetadata(source.wstring())&&ordinary.OpenMetadata(source.wstring()));
        check(measured.Width()==64&&ordinary.Width()==128);
    }
    static void run(const std::filesystem::path& source,const std::filesystem::path& rendered,unsigned scale,bool nv12){
        PlayerApp app{AppOptions{}};
        // No source audio is generated, and a mute request is retained too.
        app.m_muted=true;app.m_volume=0;app.m_neuralRequested=true;
        app.m_viewport=CreateWindowExW(0,L"STATIC",L"Phoenix test viewport",WS_POPUP|WS_VISIBLE,
            -32000,-32000,320,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        check(app.m_viewport!=nullptr);
        struct WindowOwner {HWND window;~WindowOwner(){DestroyWindow(window);}} viewport{app.m_viewport};
        check(app.m_decoder.OpenMetadata(source.wstring(),MediaSourceKind::LocalFile,{},nv12));
        const unsigned width=app.m_decoder.Width(),height=app.m_decoder.Height();
        app.m_path=source.wstring();app.m_loaded=true;app.m_playing=false;app.m_currentSec=.2;
        app.m_renderWnd=CreateWindowExW(0,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE,
            0,0,320,200,app.m_viewport,nullptr,GetModuleHandleW(nullptr),&app);
        check(app.m_renderWnd!=nullptr);
        app.m_renderer=MakeD3D12Renderer();app.m_renderer->SetDirectPhoenixProcessing(false);
        app.ConfigureRendererSource();app.m_renderer->SetDirectPhoenixProcessing(false);
        check(app.m_renderer->Initialize(app.m_renderWnd,width,height,width,height,8,5,DefaultNeuralCarrierQuality()));
        app.m_next.bgra={1,2,3,4};app.m_haveNext=true;
        const auto* previous=app.m_renderer.get();const HWND previousWindow=app.m_renderWnd;
        app.m_liveSession=true;app.m_liveRange={0,20000000};
        app.m_liveSegments=std::make_shared<NeuralSegmentIndex>();
        app.m_liveSegments->Append({rendered,1,0,0,0,20000000,60});
        const auto unchanged=[&]{
            check(!app.m_cachedPlayback&&!app.m_liveAttached&&app.m_renderer.get()==previous);
            check(app.m_renderWnd==previousWindow&&IsWindow(previousWindow));
            check(app.m_haveNext&&app.m_next.bgra==std::vector<uint8_t>({1,2,3,4}));
            check(app.m_decoder.Width()==width&&app.m_decoder.Height()==height);
            check(app.m_currentSec==.2&&!app.m_playing&&app.m_comparisonView==ComparisonView::Original);
        };
        app.m_livePhoenixRecipe="{invalid";
        check(!app.AttachLiveNeural());unchanged();
        app.m_livePhoenixRecipe=nlohmann::json{{"output_scale",scale}}.dump();
        // Force failure after pair and metadata preparation: a missing viewport
        // must not tear down the old renderer or clear its pending source frame.
        const HWND validViewport=app.m_viewport;app.m_viewport=nullptr;
        check(!app.AttachLiveNeural());app.m_viewport=validViewport;unchanged();
        check(app.AttachLiveNeural());
        check(app.m_liveAttached&&app.m_cachedPlayback&&!app.m_haveNext);
        check(app.m_comparisonView==ComparisonView::Neural&&app.m_havePresentedPair);
        check(app.m_decoder.Width()==width*scale&&app.m_decoder.Height()==height*scale);
        check(app.m_renderer->OutputW()==width*scale&&app.m_renderer->OutputH()==height*scale);
        check(app.m_lastPair&&app.m_lastPlaybackFrame.get()==&app.m_lastPair->neural);
        check(app.m_lastPair->original.timestamp100ns==app.m_lastPair->neural.timestamp100ns);
        check(!app.m_renderer->DirectPhoenixAvailable()&&!app.m_renderer->DLSSEnabled());
        std::vector<uint8_t> original,neural;uint32_t w=0,h=0;
        ComparisonSettings comparison;comparison.mode=ComparisonMode::Original;
        app.m_renderer->SetComparison(comparison);check(app.m_renderer->PresentCurrent());
        check(app.m_renderer->CaptureComposedView(original,w,h));
        comparison.mode=ComparisonMode::Neural;app.m_renderer->SetComparison(comparison);
        check(app.m_renderer->PresentCurrent());check(app.m_renderer->CaptureComposedView(neural,w,h));
        check(original.size()==neural.size()&&!original.empty());
        unsigned changed=0;for(size_t i=0;i<original.size();++i)changed+=std::abs(int(original[i])-int(neural[i]))>20;
        check(changed>original.size()/10); // Independent red/green clip oracle.
        check(app.m_renderer->DLSSEvaluations()==0); // Never process cached results twice.
        // The requested geometry must travel with a decoder swap, including a
        // subsequent reopen; its currently probed width alone cannot prove this.
        check(app.m_decoder.OpenMetadata(source.wstring(),MediaSourceKind::LocalFile,{},nv12));
        check(app.m_decoder.Width()==width*scale&&app.m_decoder.Height()==height*scale);
        app.m_liveSession=false;app.m_renderer.reset();DestroyWindow(app.m_renderWnd);app.m_renderWnd=nullptr;
        app.m_loaded=false;app.m_viewport=nullptr;
    }
};

int wmain(int argc,wchar_t** argv){
    try{
        check(argc==4);check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)));
        check(SUCCEEDED(MFStartup(MF_VERSION)));
        WNDCLASSW windowClass{};windowClass.lpfnWndProc=DefWindowProcW;
        windowClass.hInstance=GetModuleHandleW(nullptr);windowClass.lpszClassName=L"DLSSVideoRenderClassV11";
        check(RegisterClassW(&windowClass)!=0);
        PlayerAppTestAccess::decoderSwap(argv[1]);
        for(bool nv12:{false,true})for(unsigned scale:{1u,2u})
            PlayerAppTestAccess::run(argv[1],argv[scale==1?2:3],scale,nv12);
        MFShutdown();CoUninitialize();
        std::cout<<"{\"passed\":true,\"cases\":4,\"failure_paths\":8,\"comparison_readbacks\":8,\"decoder_swap\":true}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
