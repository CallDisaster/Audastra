#include "War3MapCore/Render/ModelAnimationCapture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int wmain(int argc,wchar_t** argv) try {
    War3Map::Render::AnimationCaptureRequest request; std::filesystem::path input;
    for(int i=1;i<argc;++i) {
        const std::wstring key=argv[i];
        if(key==L"--hardware") {request.software=false;continue;}
        if(key==L"--world-preview") {request.worldPreview=true;continue;}
        if(i+1>=argc)throw std::runtime_error("参数缺少值。");
        const std::wstring value=argv[++i];
        if(key==L"--game-root")request.gameRoot=value;
        else if(key==L"--input")input=value;
        else if(key==L"--output-dir")request.outputDirectory=std::filesystem::absolute(value);
        else if(key==L"--width" || key==L"--height" || key==L"--revision") {
            std::size_t end=0; const auto number=std::stoull(value,&end);
            if(end!=value.size() || value.empty() || value.front()==L'-')throw std::runtime_error("整数参数无效。");
            if(key==L"--revision")request.documentRevision=number;
            else {
                if(number>2048)throw std::runtime_error("图像尺寸超过2048。");
                if(key==L"--width")request.width=static_cast<std::uint32_t>(number);else request.height=static_cast<std::uint32_t>(number);
            }
        } else throw std::runtime_error("未知参数。");
    }
    if(input.empty() || request.gameRoot.empty() || request.outputDirectory.empty())throw std::runtime_error(
        "用法：war3-model-animation --game-root <游戏目录> --input <UTF-8清单.tsv> --output-dir <不存在目录> [--width 1024 --height 768 --revision 0] [--hardware] [--world-preview]；默认严格，world-preview 仅为世界页局部恢复近似。");
    const auto size=std::filesystem::file_size(input); if(size>1024*1024)throw std::runtime_error("清单超过1MiB。");
    std::string text(static_cast<std::size_t>(size),'\0'); std::ifstream stream(input,std::ios::binary);
    if(!stream.read(text.data(),static_cast<std::streamsize>(text.size())) || stream.peek()!=std::char_traits<char>::eof())throw std::runtime_error("清单未完整读取或读取时变化。");
    auto plan=War3Map::Render::ParseAnimationCapturePlan(text);
    if(!plan.Succeeded()) {for(const auto& d:plan.diagnostics)std::cerr<<d.code<<": "<<d.message<<'\n';return 1;}
    request.plan=std::move(*plan.value);
    auto output=War3Map::Render::RunModelAnimationCapture(request);
    if(!output.Succeeded()) {for(const auto& d:output.diagnostics)std::cerr<<d.code<<": "<<d.message<<'\n';return 1;}
    std::cout<<output.value->summaryJson<<'\n';return 0;
} catch(const std::exception& e) {std::cerr<<"ANIMATION_COMMAND_FAILED: "<<e.what()<<'\n';return 1;}
