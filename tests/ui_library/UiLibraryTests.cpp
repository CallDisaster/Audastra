#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::UiLibrary;

void Expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// 原创合成数据根：两个 config 列出的包、一个未列出的包、一个非包目录。
std::filesystem::path CreateRoot() {
    const auto root = std::filesystem::temp_directory_path() /
        ("War3MapEditor64-UiLibrary-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    const auto ui = root / "ui";
    WriteText(ui / "config", "base\r\nlib/ext\r\n");
    WriteText(ui / "base" / "action.txt", "[DoNothing]\r\ntitle=\"Do nothing\"\r\n[SetLife]\r\ntitle=\"Set life\"\r\n");
    WriteText(ui / "lib" / "ext" / "action.txt",
        "[LibraryAction]\r\ntitle=\"Library\"\r\n[DoNothing]\r\ntitle=\"Override\"\r\n");
    WriteText(ui / "lib" / "ext" / "bin" / "loader.dll", "MZ not a real binary");
    WriteText(ui / "extra" / "call.txt", "[ExtraCall]\r\ntitle=\"Extra\"\r\nreturns=unit\r\n");
    WriteText(ui / "notes" / "readme.md", "not a package");
    return root;
}

Trigger::EcaNode Node(std::string name, std::string layer) {
    Trigger::EcaNode node;
    node.name.text = std::move(name);
    node.uiLayer = std::move(layer);
    return node;
}

void TestDiscoveryAndFingerprints(const std::filesystem::path& root) {
    auto discovered = DiscoverPackages(root);
    Expect(discovered.Succeeded(), "discovery failed");
    const auto& packages = *discovered.value;
    Expect(packages.size() == 3, "expected base, lib/ext and extra packages");
    Expect(packages[0].id == "base" && packages[0].listedInConfig && packages[0].configOrder == 0, "base order");
    Expect(packages[1].id == "lib/ext" && packages[1].listedInConfig && packages[1].configOrder == 1, "lib/ext order");
    Expect(packages[2].id == "extra" && !packages[2].listedInConfig, "unlisted package must follow listed ones");
    Expect(packages[1].fileCount == 1, "binary files must not be read or fingerprinted");

    // 包内程序变化不影响指纹；文本定义变化会改变指纹。
    const std::string before = packages[1].fingerprint;
    WriteText(root / "ui" / "lib" / "ext" / "bin" / "loader.dll", "MZ changed");
    Expect((*DiscoverPackages(root).value)[1].fingerprint == before, "binary change altered the fingerprint");
    WriteText(root / "ui" / "lib" / "ext" / "action.txt",
        "[LibraryAction]\r\ntitle=\"Library v2\"\r\n[DoNothing]\r\ntitle=\"Override\"\r\n");
    Expect((*DiscoverPackages(root).value)[1].fingerprint != before, "definition change kept the old fingerprint");
}

void TestGlobalLayerAndEffectiveLayers(const std::filesystem::path& root) {
    const auto packages = *DiscoverPackages(root).value;
    const GlobalSettings initial = ReconcileGlobal({}, packages);
    Expect(initial.packages.size() == 3 && initial.packages[0].enabled && initial.packages[1].enabled &&
        !initial.packages[2].enabled, "first use must mirror config");

    // 用户调整过顺序并关闭 lib/ext；本机已删除的包从设置中移除。
    GlobalSettings saved;
    saved.packages = {{"LIB/EXT", false, "old"}, {"gone", true, "x"}, {"base", true, "y"}};
    const GlobalSettings merged = ReconcileGlobal(saved, packages);
    Expect(merged.packages.size() == 3, "missing package was not dropped");
    Expect(merged.packages[0].id == "lib/ext" && !merged.packages[0].enabled, "user order / state not kept");
    Expect(merged.packages[1].id == "base" && merged.packages[2].id == "extra" && !merged.packages[2].enabled,
        "new unlisted package must be appended disabled");

    const auto onlyGlobal = EffectiveLayers(merged, {}, packages);
    Expect(onlyGlobal == std::vector<std::string>{"base"}, "disabled package leaked into effective layers");
    const std::vector<std::string> mapOnly{"lib/ext"};
    const auto withMapOnly = EffectiveLayers(merged, mapOnly, packages);
    Expect(withMapOnly == std::vector<std::string>{"lib/ext", "base"}, "map-only package must keep global position");
}

void TestCatalogFromLayersAndConflicts(const std::filesystem::path& root) {
    const std::string configBefore = ReadText(root / "ui" / "config");
    const std::vector<std::string> layers{"base", "lib/ext"};
    auto catalog = Trigger::UiCatalog::LoadLayers(root, layers);
    Expect(catalog.Succeeded(), "LoadLayers failed");
    Expect(ReadText(root / "ui" / "config") == configBefore, "LoadLayers modified the data-root config");
    Expect(catalog.value->Find(Trigger::UiDefinitionKind::Action, "LibraryAction") != nullptr, "layer not loaded");
    Expect(catalog.value->Find(Trigger::UiDefinitionKind::Call, "ExtraCall") == nullptr, "unselected layer loaded");
    const auto conflicts = FindConflicts(*catalog.value);
    Expect(conflicts.size() == 1 && conflicts[0].name == "DoNothing", "override conflict not reported");
    Expect(conflicts[0].layers == layers && conflicts[0].activeLayer == "lib/ext", "conflict must show load order and winner");

    const std::vector<std::string> reordered{"lib/ext", "base"};
    auto swapped = Trigger::UiCatalog::LoadLayers(root, reordered);
    Expect(swapped.Succeeded() && FindConflicts(*swapped.value)[0].activeLayer == "base", "later layer must win");
    Expect(swapped.value->Sha256() != catalog.value->Sha256(), "catalog identity must depend on layer order");
}

void TestMapUsageChecks(const std::filesystem::path& root) {
    auto packages = *DiscoverPackages(root).value;
    Trigger::TriggerDocument document;
    document.ecaNodes = {Node("LibraryAction", "lib/ext"), Node("DoNothing", "base"),
        Node("LibraryAction", "lib/ext"), Node("GoneAction", "gone")};
    const MapUsage usage = ComputeMapUsage(document, packages);
    Expect(usage.packages.size() == 3, "usage must list each referenced package once");
    Expect(usage.packages[0].id == "base" && usage.packages[1].id == "lib/ext" && usage.packages[2].id == "gone",
        "usage must follow local load order with unknown packages last");
    Expect(usage.packages[1].functionCount == 1, "function count must be distinct names");

    GlobalSettings global = ReconcileGlobal({}, packages);
    auto checks = CheckMapUsage(usage, global, packages);
    Expect(checks[0].status == PackageStatus::Same && checks[1].status == PackageStatus::Same &&
        checks[2].status == PackageStatus::Missing, "baseline check statuses");

    global.packages[1].enabled = false;
    WriteText(root / "ui" / "base" / "action.txt", "[DoNothing]\r\ntitle=\"Changed\"\r\n[SetLife]\r\ntitle=\"Set life\"\r\n");
    packages = *DiscoverPackages(root).value;
    checks = CheckMapUsage(usage, global, packages);
    Expect(checks[0].status == PackageStatus::Changed && checks[0].localFingerprint != checks[0].recordedFingerprint,
        "content change not detected");
    Expect(checks[1].status == PackageStatus::DisabledLocally, "locally disabled package not detected");
    Expect(ToString(checks[2].status) == "missing", "status names are stable");
}

} // namespace

int main() {
    std::filesystem::path root;
    try {
        root = CreateRoot();
        TestDiscoveryAndFingerprints(root);
        root = CreateRoot();
        TestGlobalLayerAndEffectiveLayers(root);
        TestCatalogFromLayersAndConflicts(root);
        TestMapUsageChecks(root);
        std::filesystem::remove_all(root);
        std::cout << "UiLibrary two-layer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (!root.empty()) std::filesystem::remove_all(root);
        std::cerr << "UiLibrary test failure: " << error.what() << '\n';
        return 1;
    }
}
