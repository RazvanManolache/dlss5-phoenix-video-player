#pragma once
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <vector>
#include <string>
#include <array>
#include <string_view>
#include <type_traits>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include "pipeline/recipe.h"
#include "PhoenixExecution.h"
#include "Utf8Text.h"

// The same native schema used by the addon and Everything. UI edits are kept
// local until Apply; the SDK constructs a replacement before retiring the
// working session, so refused settings leave the visible session intact.
namespace phoenix_player {
class PipelineDialog {
    using Json=nlohmann::json;
    phoenix_pipeline::Options options_;
    phoenix_pipeline::Tuning tuning_;
    std::function<std::string(const std::string&,const std::string&,const std::string&)> apply_;
    Json recipe_;std::string device_;
    HWND window_=nullptr,tabs_=nullptr,status_=nullptr;
    HFONT font_=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    std::array<std::vector<HWND>,3> pages_;
    std::vector<std::function<void()>> readers_;
    std::array<HWND,4> stages_{};
    bool tiny_available_=false;
    HWND add(int page,const wchar_t* kind,const std::wstring& text,DWORD style,int x,int y,int w=355,int h=24,int id=0){
        HWND result=CreateWindowExW(std::wstring_view(kind)==L"EDIT"?WS_EX_CLIENTEDGE:0,kind,text.c_str(),WS_CHILD|WS_VISIBLE|style,
            x,y,w,h,window_,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        if(!result)throw std::runtime_error("Cannot create Phoenix setting control");
        SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);if(page>=0)pages_[size_t(page)].push_back(result);return result;
    }
    void check(int page,const wchar_t* text,bool& value,int x,int y){
        auto control=add(page,L"BUTTON",text,WS_TABSTOP|BS_AUTOCHECKBOX,x,y);SendMessageW(control,BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);
        readers_.push_back([control,&value]{value=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;});
    }
    template<class Value>void number(int page,const wchar_t* text,Value& value,int x,int y){
        add(page,L"STATIC",text,SS_LEFT,x,y,253);
        auto control=add(page,L"EDIT",std::to_wstring(value),WS_TABSTOP|ES_AUTOHSCROLL,x+255,y-2,90);
        readers_.push_back([control,&value]{wchar_t text[64]{};GetWindowTextW(control,text,64);wchar_t* end=nullptr;const auto parsed=wcstod(text,&end);
            if(end==text||*end||!std::isfinite(parsed))throw std::invalid_argument("Enter a valid number in each field.");
            if constexpr(std::is_integral_v<Value>){if(parsed!=std::floor(parsed)||parsed<0||parsed>10000)throw std::invalid_argument("Enter a whole number within the supported range.");}
            value=Value(parsed);});
    }
    void choice(int page,const wchar_t* label,int& value,std::initializer_list<const wchar_t*> names,int x,int y){
        add(page,L"STATIC",label,SS_LEFT,x,y,355);auto control=add(page,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,x,y+23,355,230);
        for(auto name:names)SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));SendMessageW(control,CB_SETCURSEL,value,0);
        readers_.push_back([control,&value]{value=int(SendMessageW(control,CB_GETCURSEL,0,0));});
    }
    void build(){
        tabs_=add(-1,WC_TABCONTROLW,L"",WS_TABSTOP,12,10,786,30,1);
        for(const auto text:{L"Motion and monitoring",L"Processing order",L"Quality and budgets"}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(text);TabCtrl_InsertItem(tabs_,TabCtrl_GetItemCount(tabs_),&item);}
        check(0,L"Estimate motion when needed",options_.motion_enabled,24,60);
        choice(0,L"Motion source",options_.motion_source,{L"Automatic: prefer valid engine vectors",L"Engine vectors",L"NVIDIA Optical Flow",L"Zero motion"},24,98);
        number(0,L"Motion analysis height (64–2160)",options_.flow_height,24,166);
        number(0,L"Consistency tolerance (0.1–20 px)",options_.flow_consistency,24,204);
        number(0,L"Cost threshold (1–1000 per mille)",options_.flow_cost,24,242);
        check(0,L"Detect scene changes and reset history",options_.scene_detection,24,286);
        choice(0,L"Monitoring",options_.monitoring,{L"Off",L"Basic CPU/queue completion counters",L"Detailed sampled GPU timings"},24,328);
        number(0,L"GPU sample interval (1–600)",options_.sample_interval,24,396);
        choice(0,L"Motion diagnostic image",options_.motion_view,{L"Processed image",L"Motion direction and magnitude",L"Motion confidence"},24,440);
        add(0,L"STATIC",L"Motion analysis has its own resolution. No geometric depth is invented. Pipeline adaptation requires monitoring. Neural FPS/time budgets work independently.",SS_LEFT,414,64,365,140);
        add(0,L"STATIC",L"Processing device (new temporal session)",SS_LEFT,414,220);
        auto execution=add(0,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,414,245,355,230);
        auto choices=std::make_shared<std::vector<std::string>>();
        auto append=[&](const std::string& id,const std::string& label){
            if(std::find(choices->begin(),choices->end(),id)!=choices->end())return;
            const auto row=SendMessageW(execution,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(utf8_text::ToWide(label).c_str()));
            choices->push_back(id);if(id==device_)SendMessageW(execution,CB_SETCURSEL,row,0);
        };
        append("native","Native RTX · rendering GPU");
        if(recipe_.contains("portable_package")){
            append("cpu","CPU C++ (experimental; slow at large sizes)");
            for(const auto& choice:recipe_.value("execution_devices",Json::array()))append(choice.at("id"),choice.at("label"));
        }
        append(device_,device_);
        readers_.push_back([this,execution,choices]{const auto row=SendMessageW(execution,CB_GETCURSEL,0,0);
            if(row<0||size_t(row)>=choices->size())throw std::invalid_argument("Choose a processing device.");device_=choices->at(size_t(row));});
        check(1,L"Enable processing chain",options_.chain_enabled,24,60);
        const wchar_t* names[]{L"Neural rendering",L"Scaling",L"Color treatment",L"Multiscale fusion"};
        for(int row=0;row<4;++row){
            const auto stage=options_.order[size_t(row)];stages_[size_t(row)]=add(1,L"BUTTON",names[stage],WS_TABSTOP|BS_AUTOCHECKBOX,24,106+row*42,245,25);
            SendMessageW(stages_[size_t(row)],BM_SETCHECK,options_.enabled[size_t(stage)]?BST_CHECKED:BST_UNCHECKED,0);
            add(1,L"BUTTON",L"Move up",WS_TABSTOP|BS_PUSHBUTTON,278,106+row*42,80,25,100+row);
        }
        add(1,L"STATIC",L"Scaling provider",SS_LEFT,24,292);
        auto scaler=add(1,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,24,315,355,200);
        for(int provider:{0,1,3}){
            if(provider==3&&!tiny_available_)continue;
            const wchar_t* name=provider==0?L"Existing spatial scaler":provider==1?L"NVIDIA Image Scaling":L"Tiny native 2x";
            const auto row=SendMessageW(scaler,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));SendMessageW(scaler,CB_SETITEMDATA,row,provider);
            if(provider==options_.scaler)SendMessageW(scaler,CB_SETCURSEL,row,0);
        }
        readers_.push_back([this,scaler]{const auto row=SendMessageW(scaler,CB_GETCURSEL,0,0);options_.scaler=int(SendMessageW(scaler,CB_GETITEMDATA,row,0));});
        number(1,L"NIS sharpness (0–1)",options_.sharpness,24,360);
        number(1,L"Brightness (-2–2)",options_.color_brightness,24,404);number(1,L"Contrast (0–4)",options_.color_contrast,24,442);
        number(1,L"Saturation (0–2)",options_.color_saturation,24,480);number(1,L"Gamma (0.1–4)",options_.color_gamma,24,518);
        check(1,L"Experimental multiscale fusion",options_.fusion,414,106);
        number(1,L"Fusion layers (2–3)",options_.fusion_layers,414,150);
        number(1,L"Second layer scale (0.1–1)",options_.fusion_second_scale,414,188);
        number(1,L"Third layer scale (0.1–1)",options_.fusion_third_scale,414,226);
        add(1,L"STATIC",L"Each whole-network pass and fusion branch owns separate history. Branch predictions are combined at full size. Central block repeats stay inside each evaluation. Tiny needs exactly 2x output; choose that launch recipe in Everything.",SS_LEFT,414,304,365,160);
        number(2,L"Whole-network passes (1–10)",tuning_.passes,24,60);number(2,L"Neural processing scale (10–100%)",tuning_.neural_scale,24,98);
        number(2,L"Central blocks (1–8)",tuning_.blocks,24,136);number(2,L"Minimum central blocks",tuning_.minimum_blocks,24,174);
        for(unsigned i=0;i<8;++i){const auto label=L"Block "+std::to_wstring(i+1)+L" repeats (0–4)";number(2,label.c_str(),tuning_.repeats[i],24,228+int(i)*36);}
        choice(2,L"Adaptive priority",options_.priority,{L"Keep quality fixed",L"Prefer responsiveness",L"Prefer quality"},414,60);
        number(2,L"Target source FPS (0 = disabled)",tuning_.target_fps,414,130);number(2,L"Max processing ms (0 = disabled)",tuning_.max_time_ms,414,168);
        number(2,L"Processing age budget (ms)",options_.latency_ms,414,206);
        check(2,L"Allow motion resolution adjustment",options_.adjust_flow,414,250);check(2,L"Allow neural resolution adjustment",options_.adjust_scale,414,286);
        check(2,L"Allow central block adjustment",options_.adjust_blocks,414,322);check(2,L"Allow network pass adjustment",options_.adjust_passes,414,358);
        number(2,L"Minimum motion analysis height",options_.minimum_flow_height,414,402);number(2,L"Minimum neural scale (%)",options_.minimum_scale,414,440);
        number(2,L"Minimum network passes",options_.minimum_passes,414,478);
        add(2,L"STATIC",L"Offline exports keep the selected quality and process all frames. The live governor adjusts one permitted variable at a time, then lets it settle.",SS_LEFT,414,526,365,65);
        status_=add(-1,L"STATIC",L"Settings apply to this playback and its export recipe. Apply resets temporal history.",SS_LEFT,24,620,650,48);
        add(-1,L"BUTTON",L"Apply",WS_TABSTOP|BS_DEFPUSHBUTTON,600,680,90,30,IDOK);add(-1,L"BUTTON",L"Close",WS_TABSTOP|BS_PUSHBUTTON,700,680,90,30,IDCANCEL);
        show_page();
    }
    void show_page(){const int selected=TabCtrl_GetCurSel(tabs_);for(unsigned page=0;page<3;++page)for(auto control:pages_[page])ShowWindow(control,int(page)==selected?SW_SHOW:SW_HIDE);}
    void move(unsigned row){if(!row)return;
        for(unsigned i=0;i<4;++i)options_.enabled[size_t(options_.order[i])]=SendMessageW(stages_[i],BM_GETCHECK,0,0)==BST_CHECKED;
        std::swap(options_.order[row],options_.order[row-1]);const wchar_t* names[]{L"Neural rendering",L"Scaling",L"Color treatment",L"Multiscale fusion"};
        for(unsigned i=0;i<4;++i){const auto stage=unsigned(options_.order[i]);SetWindowTextW(stages_[i],names[stage]);SendMessageW(stages_[i],BM_SETCHECK,options_.enabled[stage]?BST_CHECKED:BST_UNCHECKED,0);}
    }
    void apply(){try{
        for(const auto& read:readers_)read();for(unsigned i=0;i<4;++i)options_.enabled[size_t(options_.order[i])]=SendMessageW(stages_[i],BM_GETCHECK,0,0)==BST_CHECKED;
        if(!options_.valid())throw std::invalid_argument("A motion, chain or budget value is outside its displayed range.");
        const auto tuning=phoenix_pipeline::Tuning::read(tuning_.json());
        if(options_.scaler==2)throw std::invalid_argument("VSR is unavailable in this package.");
        if(options_.scaler==3&&!tiny_available_)throw std::invalid_argument("Launch an installed Tiny 2x recipe through Everything first.");
        const auto error=apply_(phoenix_pipeline::options_json(options_).dump(),tuning.json().dump(),device_);
        if(!error.empty())throw std::runtime_error(error);
        SetWindowTextW(status_,L"Applied. Every temporal stage starts fresh; exports use this recipe.");
    }catch(const std::exception& error){const std::string text=error.what();const std::wstring message(text.begin(),text.end());SetWindowTextW(status_,message.c_str());}}
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<PipelineDialog*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<PipelineDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self){if(message==WM_COMMAND){const auto id=LOWORD(w);if(id==IDOK){self->apply();return 0;}if(id==IDCANCEL){DestroyWindow(window);return 0;}if(id>=100&&id<104){self->move(id-100);return 0;}}
            if(message==WM_NOTIFY&&reinterpret_cast<NMHDR*>(l)->hwndFrom==self->tabs_&&reinterpret_cast<NMHDR*>(l)->code==TCN_SELCHANGE){self->show_page();return 0;}}
        return DefWindowProcW(window,message,w,l);
    }
public:
    PipelineDialog(const std::string& status,std::function<std::string(const std::string&,const std::string&,const std::string&)> apply):apply_(std::move(apply)){
        const auto recipe=Json::parse(status).at("media_recipe");options_=phoenix_pipeline::options_from_json(recipe.at("pipeline"));tuning_=phoenix_pipeline::Tuning::read(recipe.value("tuning",Json::object()));
        tiny_available_=recipe.at("runtime").contains("tiny_model_path");
        recipe_=recipe;device_=ExecutionDevice(recipe);
    }
    void show(HWND owner){
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=procedure;cls.lpszClassName=L"PhoenixSharedPipelineEditor";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&cls);
        HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,L"Phoenix processing",WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,830,765,owner,nullptr,cls.hInstance,this);
        if(!window)throw std::runtime_error("Cannot open Phoenix processing settings");
        try{build();}catch(...){DestroyWindow(window);throw;}
        EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);MSG message{};
        while(IsWindow(window)){const auto result=GetMessageW(&message,nullptr,0,0);if(result<=0){if(result==0)PostQuitMessage(int(message.wParam));break;}
            if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(IsWindow(window))DestroyWindow(window);EnableWindow(owner,TRUE);SetForegroundWindow(owner);
    }
};
}
