#include "War3MapCore/Render/ModelGameLimits.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Services/ObjectResourceQuery.h"
#include "support/GeosetLimitMdxFixture.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// 几何体超出游戏上限（16384 顶点 / 49152 索引，严格大于才算）：判定、提示文字、场景按模型只报一次、物编资源查询。
namespace {

using namespace War3Map;
using namespace War3Map::Render;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string Key(std::string_view path) {
    std::string key(path);
    for (auto& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

MdxModel Parse(const std::vector<std::byte>& bytes) {
    auto model = ParseMdx(bytes);
    Require(model.Succeeded() && model.value, "合成 MDX 解析失败");
    return std::move(*model.value);
}

void TestBoundary() {
    Require(!GeosetOverGameLimit(16384, 49152), "恰好等于上限不算超限");
    Require(GeosetOverGameLimit(16385, 3), "顶点 16385 超限");
    Require(GeosetOverGameLimit(3, 49153), "索引 49153 超限");
    Require(!GeosetOverGameLimit(0, 0), "空几何体不算超限");
}

void TestFindingsAndMessage() {
    const auto atLimit = Parse(GeosetLimitFixture::BuildMdx({{16384, 49152}}));
    Require(atLimit.geosets.size() == 1 && atLimit.geosets[0].positions.size() == 16384
        && atLimit.geosets[0].indices.size() == 49152, "夹具顶点/索引数");
    Require(FindGeosetsOverGameLimit(atLimit).empty() && !GeosetGameLimitWarning(atLimit, "Doodads\\Edge.mdx"), "恰好等于上限不提醒");

    const auto model = Parse(GeosetLimitFixture::BuildMdx({{16384, 49152}, {16385, 3}, {3, 49155}, {3, 3}}));
    const auto findings = FindGeosetsOverGameLimit(model);
    Require(findings.size() == 2 && findings[0].geoset == 1 && findings[0].vertices == 16385 && findings[0].indices == 3
        && findings[1].geoset == 2 && findings[1].vertices == 3 && findings[1].indices == 49155, "只列出超限的几何体");
    const auto warning = GeosetGameLimitWarning(model, "Doodads\\Big.mdx");
    Require(warning && warning->code == kGeosetOverGameLimitCode && warning->severity == DiagnosticSeverity::Warning, "提醒代码与级别");
    Require(warning->origin && warning->origin->source == std::filesystem::path(u8"Doodads\\Big.mdx"), "提醒带模型路径");
    const auto& text = warning->message;
    for (const auto* part : {"模型 Doodads\\Big.mdx：", "第 2 个几何体 16385 顶点、3 索引", "第 3 个几何体 3 顶点、49155 索引",
             "16384 顶点 / 49152 索引", "整块不画", "旧版 WarVK 里，视野内有它们时同帧阴影可能整帧放弃", "16000 顶点、48000 索引"})
        Require(text.find(part) != std::string::npos, std::string("提示缺少：") + part);
    Require(text.find("第 1 个") == std::string::npos && text.find("第 4 个") == std::string::npos, "未超限的几何体不列");
}

void TestBytesFileAndPath() {
    const std::vector<std::byte> garbage{std::byte{'M'}, std::byte{'D'}, std::byte{'L'}, std::byte{'X'}};
    Require(!GeosetGameLimitWarning(garbage, "Bad.mdx"), "解析失败不在这里报告");
    const auto bytes = GeosetLimitFixture::BuildMdx({{16385, 3}});
    Require(GeosetGameLimitWarning(bytes, "Big.mdx").has_value(), "字节判定");
    const auto file = std::filesystem::temp_directory_path() / "audastra-geoset-limit-synthetic.mdx";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto fromFile = GeosetGameLimitWarningForFile(file, "Big.mdx");
    const auto tooLarge = GeosetGameLimitWarningForFile(file, "Big.mdx", 16);
    const auto missing = GeosetGameLimitWarningForFile(file.parent_path() / "audastra-geoset-limit-missing.mdx", "Missing.mdx");
    std::filesystem::remove(file);
    Require(fromFile && fromFile->message.find("模型 Big.mdx：") == 0, "文件判定用给定的模型名");
    Require(!tooLarge && !missing, "读不到或超过读取上限时不判定");
    Require(IsMdxPath("a.mdx") && IsMdxPath("War3mapImported\\A.MDX") && !IsMdxPath("a.mdl") && !IsMdxPath("a.blp")
        && !IsMdxPath("mdx"), "扩展名判断");
}

void TestSceneWarnsOncePerModel() {
    std::map<std::string, std::vector<std::byte>> files{
        {Key("Doodads\\Big.mdx"), GeosetLimitFixture::BuildMdx({{16385, 3}})},
        {Key("Doodads\\Small.mdx"), GeosetLimitFixture::BuildMdx({{4, 6}})}};
    const ModelFileReader reader = [&](std::string_view path) {
        const auto found = files.find(Key(path));
        if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "缺少文件");
        return Result<std::vector<std::byte>>::Success(found->second);
    };
    std::vector<ModelInstanceRequest> requests(4);
    requests[0].modelPath = "Doodads\\Big.mdx"; requests[1].modelPath = "doodads/BIG.mdx";
    requests[2].modelPath = "Doodads\\Small.mdx"; requests[3].modelPath = "Doodads\\Big.mdx";
    for (std::size_t i = 0; i < requests.size(); ++i) requests[i].position = {static_cast<float>(i) * 300.0f, 0, 0};
    const auto scene = BuildWorldModelScene(requests, reader);
    Require(scene.Succeeded() && scene.value, "场景构建失败");
    Require(scene.value->instances.size() == 4, "超限模型在编辑器里照常显示");
    const auto count = std::count_if(scene.value->warnings.begin(), scene.value->warnings.end(),
        [](const Diagnostic& d) { return d.code == kGeosetOverGameLimitCode; });
    Require(count == 1, "同一模型（路径大小写/斜杠不同）只报一次");
}

void TestObjectResourceInfo() {
    const auto bytes = GeosetLimitFixture::BuildMdx({{16385, 3}, {4, 6}});
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;
    source.id = "synthetic"; source.layer = Resources::ResourceLayer::MapImport;
    source.priority = Resources::ResourcePriority::MapArchive;
    source.probe = [](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success(Key(path) == Key("War3mapImported\\Big.mdx")
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
    };
    source.read = [&](std::string_view) { return Result<std::vector<std::byte>>::Success(bytes); };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "合成来源");
    const auto info = Services::InspectObjectResource(resolver, "War3mapImported\\Big.mdx", Services::ObjectResourceKind::Model);
    Require(info.Succeeded() && info.value, "物编资源查询失败");
    const auto& geosets = info.value->geosets;
    Require(geosets.size() == 2 && geosets[0].vertices == 16385 && geosets[0].indices == 3 && geosets[0].overGameLimit
        && geosets[1].vertices == 4 && geosets[1].indices == 6 && !geosets[1].overGameLimit, "逐几何体顶点/索引数");
    Require(std::any_of(info.value->diagnostics.begin(), info.value->diagnostics.end(),
        [](const Diagnostic& d) { return d.code == kGeosetOverGameLimitCode; }), "查询结果带超限提醒");
}

} // namespace

int main() {
    try {
        TestBoundary();
        TestFindingsAndMessage();
        TestBytesFileAndPath();
        TestSceneWarnsOncePerModel();
        TestObjectResourceInfo();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "model game limits ok\n";
    return 0;
}
