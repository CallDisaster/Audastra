#include "War3MapCore/Services/CodeCheckProject.h"
#include "CodeCheckProjectLua.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"
#include "War3MapCore/Script/ScriptBaseInputs.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptInjectionSources.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/ScriptQueries.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace War3Map::Services {
namespace {
using Clock = std::chrono::steady_clock;
std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string BytesText(const std::vector<std::byte>& bytes) {
    return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string Reason(const std::vector<Diagnostic>& diagnostics, std::string fallback) {
    if (diagnostics.empty()) return fallback;
    return diagnostics.front().code + "：" + diagnostics.front().message.substr(0, 2048);
}
Script::ScriptUserSourceRequest ModernUserSources(const Script::ScriptToolchainLayout& layout,
    const CodeCheckProjectSnapshot& snapshot, const std::filesystem::path& mapDirectory) {
    Script::ScriptUserSourceRequest request{&layout, snapshot.catalog.get(),
        Trigger::UiCatalog::ResolveUiRoot(snapshot.uiRoot.empty() ? snapshot.toolRoot : snapshot.uiRoot)};
    request.mapDirectory = mapDirectory;
    // 与 MapScriptBuild 相同：只按实际启用层序提供各层现存 jass 根。
    // 来源计划不会自行推断显式根；捕获和复核都必须使用同一份输入合同。
    for (const auto& layer : snapshot.catalog->Layers()) {
        const auto root = request.uiRoot / std::filesystem::path(std::u8string(layer.begin(), layer.end())) / "jass";
        std::error_code error;
        if (std::filesystem::is_directory(root, error)) {
            request.explicitInjectionRoots.push_back(root);
            request.explicitUserIncludes.push_back(root);
        }
    }
    return request;
}
void Poll(std::stop_token cancel, Clock::time_point deadline) {
    if (cancel.stop_requested() || Clock::now() >= deadline) throw std::runtime_error("code-check-stopped");
}
std::uint32_t LineCount(std::string_view text, std::stop_token cancel, Clock::time_point deadline) {
    std::uint32_t line = 1;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if ((at & 4095u) == 0) Poll(cancel, deadline);
        if (text[at] == '\r') { ++line; if (at + 1 < text.size() && text[at + 1] == '\n') ++at; }
        else if (text[at] == '\n') ++line;
    }
    return line;
}
struct RequestedLine { std::uint32_t line; std::string_view text; };
std::vector<RequestedLine> RequestedLines(std::string_view text, std::vector<std::uint32_t> wanted,
    std::stop_token cancel, Clock::time_point deadline) {
    std::sort(wanted.begin(), wanted.end()); wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    std::vector<RequestedLine> result; result.reserve(wanted.size());
    std::uint32_t line = 1; std::size_t begin = 0, next = 0;
    for (std::size_t at = 0; at <= text.size() && next < wanted.size(); ++at) {
        if ((at & 4095u) == 0) Poll(cancel, deadline);
        if (at < text.size() && text[at] != '\r' && text[at] != '\n') continue;
        while (next < wanted.size() && wanted[next] < line) ++next;
        if (next < wanted.size() && wanted[next] == line) { result.push_back({line, text.substr(begin, at - begin)}); ++next; }
        if (at < text.size() && text[at] == '\r' && at + 1 < text.size() && text[at + 1] == '\n') ++at;
        begin = at + 1; ++line;
    }
    return result;
}
// 先通过生产PP/Emitter的真实行映射定位，再比较同次冻结原行；不搜索同名文本猜字节位置。
void VerifySemanticLineCopies(CodeCheckResult& result, std::string_view preprocessed,
    std::stop_token cancel, Clock::time_point deadline) {
    auto& index = result.semanticIndex;
    std::vector<CodeSymbolPosition*> candidates;
    std::vector<std::uint32_t> inputLines;
    std::vector<std::vector<std::uint32_t>> sourceLines(index.sources.size());
    for (auto* positions : {&index.definitions, &index.references}) for (auto& position : *positions) {
        if (!position.exact && position.inputExact && position.line && position.inputLine && position.file < sourceLines.size()) {
            candidates.push_back(&position); inputLines.push_back(position.inputLine);
            sourceLines[position.file].push_back(position.line);
        }
    }
    if (candidates.empty()) return;
    const auto input = RequestedLines(preprocessed, std::move(inputLines), cancel, deadline);
    std::vector<std::vector<RequestedLine>> originals(index.sources.size());
    for (std::size_t file = 0; file < index.sources.size(); ++file) {
        Poll(cancel, deadline); if (sourceLines[file].empty()) continue;
        const CodeCheckSource* source = nullptr; bool ambiguous = false;
        for (const auto& candidate : result.externalSources) if (candidate.name == index.sources[file]) {
            if (source && source->text != candidate.text) ambiguous = true;
            source = &candidate;
        }
        if (source && !ambiguous) originals[file] = RequestedLines(source->text, std::move(sourceLines[file]), cancel, deadline);
    }
    const auto find = [](const auto& lines, std::uint32_t line) -> std::optional<std::string_view> {
        const auto found = std::lower_bound(lines.begin(), lines.end(), line, [](const auto& entry, auto value) { return entry.line < value; });
        return found != lines.end() && found->line == line ? std::optional{found->text} : std::nullopt;
    };
    for (auto* position : candidates) {
        Poll(cancel, deadline);
        const auto before = find(input, position->inputLine), original = find(originals[position->file], position->line);
        if (!before || !original || *before != *original || position->inputByteColumn == 0) continue;
        const auto offset = static_cast<std::size_t>(position->inputByteColumn - 1);
        if (offset > original->size() || position->inputByteLength > original->size() - offset) continue;
        position->byteColumn = position->inputByteColumn; position->byteLength = position->inputByteLength; position->exact = true;
    }
    std::size_t declaration = 0;
    for (std::size_t i = 0; i < index.parsed.definitions.size() && i < index.definitions.size(); ++i) {
        const auto& definition = index.parsed.definitions[i];
        if (definition.kind == Jass::SymbolKind::Parameter || definition.kind == Jass::SymbolKind::Local) continue;
        if (declaration >= result.declarations.symbols.size()) break;
        auto& summary = result.declarations.symbols[declaration++]; const auto& position = index.definitions[i];
        if (position.exact && summary.name == definition.name && position.file < index.sources.size()
            && summary.source == index.sources[position.file] && summary.line == position.line)
            summary.byteColumn = position.byteColumn;
    }
}
bool Within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    const auto relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
Jass::SourceMap AuthorMap(const Script::MapScriptResult& generated, std::stop_token cancel, Clock::time_point deadline) {
    Jass::SourceMap map;
    std::map<StableId, std::uint32_t> authorFiles;
    const auto generatedFile = map.AddFile("Map/generated.j");
    std::size_t span = 0, position = 0;
    std::uint32_t line = 1, authorLine = 1;
    StableId author = (std::numeric_limits<StableId>::max)();
    while (position <= generated.text.size()) {
        Poll(cancel, deadline);
        while (span < generated.authorCode.size()
            && position >= generated.authorCode[span].byteOffset + generated.authorCode[span].byteLength) ++span;
        if (span < generated.authorCode.size() && position >= generated.authorCode[span].byteOffset) {
            const auto id = generated.authorCode[span].triggerId;
            if (author != id) { author = id; authorLine = 1; }
            auto file = authorFiles.find(id);
            if (file == authorFiles.end()) file = authorFiles.emplace(id, map.AddFile(CodeCheckTargetSourceName(
                {id == 0 ? Trigger::CodeBlockKind::GlobalHeader : Trigger::CodeBlockKind::CustomTrigger, id}))).first;
            map.Map(line, {file->second, authorLine++});
        } else { author = (std::numeric_limits<StableId>::max)(); map.Map(line, {generatedFile, line}); }
        if (position == generated.text.size()) break;
        while (position < generated.text.size() && generated.text[position] != '\r' && generated.text[position] != '\n') {
            if ((position & 4095u) == 0) Poll(cancel, deadline);
            ++position;
        }
        if (position == generated.text.size()) break;
        if (generated.text[position++] == '\r' && position < generated.text.size() && generated.text[position] == '\n') ++position;
        ++line;
    }
    return map;
}
void MapInjected(Jass::SourceMap& author, std::string_view generated, const std::vector<std::filesystem::path>& paths,
    const Script::ScriptInjectionSources& sources, bool separatorBefore, std::stop_token cancel, Clock::time_point deadline) {
    auto line = LineCount(generated, cancel, deadline);
    for (const auto& path : paths) {
        Poll(cancel, deadline);
        const auto found = std::find_if(sources.files.begin(), sources.files.end(), [&](const auto& file) { return file.binding.path == path; });
        if (found == sources.files.end()) throw std::runtime_error("injection-source-missing");
        if (separatorBefore) ++line; // 生产Framework注入在各文件前写CRLF；现代链在各文件后写CRLF。
        author.Map(line, {author.AddFile(PathText(path)), 1});
        line += LineCount(BytesText(found->bytes), cancel, deadline) - 1;
        if (!separatorBefore) ++line;
    }
}
// 只有真正指向整图主文的段再穿过author映射；include保持自己的文件身份。
Jass::SourceMap MapPreprocessed(const Jass::PreprocessResult& preprocessed, const Jass::SourceMap& author,
    std::stop_token cancel, Clock::time_point deadline) {
    Jass::SourceMap output;
    std::map<std::string, std::uint32_t> files;
    const auto fileId = [&](const std::string& name) {
        auto found = files.find(name);
        if (found == files.end()) found = files.emplace(name, output.AddFile(name)).first;
        return found->second;
    };
    const auto lines = LineCount(preprocessed.text, cancel, deadline);
    for (std::uint32_t line = 1; line <= lines; ++line) {
        if ((line & 255u) == 0) Poll(cancel, deadline);
        const auto origin = preprocessed.sourceMap.Find(line);
        if (!origin || origin->file >= preprocessed.sourceMap.Files().size()) continue;
        const auto& source = preprocessed.sourceMap.Files()[origin->file];
        if (source == "war3map.j") {
            if (const auto mapped = author.Find(origin->line); mapped && mapped->file < author.Files().size()) {
                output.Map(line, {fileId(author.Files()[mapped->file]), mapped->line}); continue;
            }
        }
        output.Map(line, {fileId(source), origin->line});
    }
    return output;
}
// 模板求值输出行 → 求值前（预处理后）行。模板块外的文本逐字节原样复制；模板自身输出的各行
// （例如 YDNL 展开出的换行）都指向模板所在的输入行。换行规则与 LineCount 一致。
Jass::SourceMap MapTemplateOutput(std::string_view input, const Script::TemplateResult& evaluated,
    std::stop_token cancel, Clock::time_point deadline) {
    Jass::SourceMap map;
    const auto file = map.AddFile("template-input");
    std::size_t span = 0, inputAt = 0;
    std::uint32_t inputLine = 1, outputLine = 1;
    const auto inputLineAt = [&](std::size_t offset) {
        for (; inputAt < offset && inputAt < input.size(); ++inputAt) {
            if ((inputAt & 4095u) == 0) Poll(cancel, deadline);
            if (input[inputAt] == '\n' || (input[inputAt] == '\r' && (inputAt + 1 >= input.size() || input[inputAt + 1] != '\n'))) ++inputLine;
        }
        return inputLine;
    };
    const auto& output = evaluated.text;
    for (std::size_t start = 0;;) {
        const auto& spans = evaluated.spans;
        while (span + 1 < spans.size() && spans[span + 1].outputBegin <= start) ++span;
        std::size_t source = start;
        if (!spans.empty() && spans[span].outputBegin <= start)
            source = start < spans[span].outputEnd ? spans[span].sourceBegin : spans[span].sourceEnd + (start - spans[span].outputEnd);
        map.Map(outputLine, {file, inputLineAt(source)});
        auto at = start;
        while (at < output.size() && output[at] != '\r' && output[at] != '\n') {
            if ((at & 4095u) == 0) Poll(cancel, deadline);
            ++at;
        }
        if (at == output.size()) break;
        if (output[at++] == '\r' && at < output.size() && output[at] == '\n') ++at;
        start = at; ++outputLine;
    }
    return map;
}
// 整图含编辑器不执行的模板块（如物编生成）时：取同次生产预处理结果中本代码块的行段（宏已按保存链展开），
// 只对这一段做模板求值并检查正文语法，行号经同一映射回到作者原行。本段自身含不能求值的模板时返回空。
std::optional<CodeCheckResult> CheckAuthorSection(CodeCheckRequest request, std::string_view preprocessed,
    const Script::TemplateContext& templates, const std::string& reason, std::stop_token cancel, Clock::time_point deadline) {
    const auto& map = request.sourceMap;
    std::uint32_t first = 0, last = 0;
    const auto lines = LineCount(preprocessed, cancel, deadline);
    for (std::uint32_t line = 1; line <= lines; ++line) {
        if ((line & 255u) == 0) Poll(cancel, deadline);
        const auto origin = map.Find(line);
        if (origin && origin->file < map.Files().size() && map.Files()[origin->file] == request.source.name) {
            if (first == 0) first = line;
            last = line;
        }
    }
    if (first == 0) return std::nullopt;
    std::vector<std::uint32_t> wanted(last - first + 1);
    for (std::uint32_t index = 0; index < wanted.size(); ++index) wanted[index] = first + index;
    std::string section;
    for (const auto& line : RequestedLines(preprocessed, std::move(wanted), cancel, deadline)) {
        if (line.line != first) section += '\n';
        section.append(line.text);
    }
    auto evaluated = Script::EvaluateTemplates(section, templates);
    if (!evaluated.Succeeded()) return std::nullopt;
    Jass::SourceMap offset; // 本段第 k 行 → 整图预处理第 first+k-1 行
    offset.Map(1, {offset.AddFile("preprocessed"), first});
    const auto evaluatedLines = LineCount(evaluated.text, cancel, deadline);
    request.sourceMap = MapTemplateOutput(section, evaluated, cancel, deadline)
        .ComposeWith(offset, evaluatedLines).ComposeWith(map, evaluatedLines);
    request.source.text = std::move(evaluated.text);
    request.language = CodeLanguage::VJass; request.preprocessed = true;
    request.context.complete = false;
    request.context.reason = "仅检查本段语法：" + reason + "；本段已按保存链展开宏与模板，整图检查由保存链完成。";
    auto checked = CheckCode(request, cancel);
    checked.reason = request.context.reason;
    return checked;
}
void AddExternalSource(CodeCheckResult& result, std::string name, std::string text, std::size_t& bytes) {
    if (std::any_of(result.externalSources.begin(), result.externalSources.end(),
        [&](const auto& source) { return source.name == name; })) return;
    if (result.externalSources.size() >= 1024 || text.size() > 32 * 1024 * 1024 - bytes) {
        result.externalSourcesComplete = false; return;
    }
    bytes += text.size(); result.externalSources.push_back({std::move(name), std::move(text)});
}
} // namespace

struct CodeCheckProjectCache::Impl {
    std::mutex mutex;
    std::shared_ptr<const CodeCheckProjectSnapshot> snapshot;
    std::optional<MapArchiveSession> archive;
    std::optional<MapDocument> map;
    std::optional<Game::GameDataSource> game;
    std::optional<ObjectData::StandardObjectCatalog> standard;
    std::optional<Script::ScriptToolchainLayout> layout;
    std::optional<Script::ScriptBaseInputs> bases;
    std::optional<Script::ScriptFrameworkInputs> framework;
    std::optional<Script::ScriptUserSourcePlan> entry;
    std::optional<Script::ScriptInjectionSources> injection;
    Script::ObjectQuerySources objects;
    std::optional<Script::TemplateContext> templates; // 与保存链同一构造：工具链命令表 + 原版与本图技能
    std::string incomplete;
    bool captureFinished = false;

    void Capture(const std::shared_ptr<const CodeCheckProjectSnapshot>& next, std::stop_token cancel, Clock::time_point deadline) {
        snapshot = next; archive.reset(); map.reset(); game.reset(); standard.reset(); layout.reset(); bases.reset();
        framework.reset(); entry.reset(); injection.reset(); objects = {}; templates.reset();
        incomplete = "上下文捕获尚未完成。"; captureFinished = false;
        const auto fail = [&](std::string reason) { incomplete = std::move(reason); captureFinished = true; };
        if (!next || !next->triggers || !next->catalog || next->binding.empty()) { fail("工程没有完整的冻结触发器来源。"); return; }
        Poll(cancel, deadline);
        if (next->sourcePath.empty() || next->sourceSha256.empty()) { fail("当前工程没有绑定源地图 SHA；仅检查正文语法。"); return; }
        auto opened = MapArchiveSession::Open(next->sourcePath);
        if (!opened.Succeeded() || !opened.value) { fail(Reason(opened.diagnostics, "源地图无法只读捕获。")); return; }
        if (opened.value->SourceSha256() != next->sourceSha256) { fail("源地图已改变，冻结来源身份失效。"); return; }
        archive.emplace(std::move(*opened.value));
        map = next->map ? next->map : std::optional<MapDocument>{MapDocument::FromArchive(*archive)};
        auto mapStatus = map->InitializationSourceStatus();
        if (!mapStatus.Succeeded() || !map->Settings() || map->Settings()->scriptLanguage != 0
            || map->Settings()->formatVersion != 25) { fail(Reason(mapStatus.diagnostics, "当前地图初始化或语言尚不支持完整JASS上下文。")); return; }
        Poll(cancel, deadline);
        auto capability = QueryScriptRebuildCapability(next->toolRoot, next->uiRoot);
        if (capability.state != ScriptRebuildCapabilityState::Ready || !capability.layout) {
            fail(Reason(capability.diagnostics, "没有可绑定的保存检查工具链。")); return;
        }
        layout.emplace(std::move(*capability.layout));
        if (!next->gameRoot.empty()) {
            auto mounted = Game::GameDataSource::MountStrict(next->gameRoot);
            if (!mounted.Succeeded() || !mounted.value) { fail(Reason(mounted.diagnostics, "游戏基础数据无法捕获。")); return; }
            game.emplace(std::move(*mounted.value));
            auto loaded = ObjectData::StandardObjectCatalog::Load(*game);
            if (!loaded.Succeeded() || !loaded.value) { fail(Reason(loaded.diagnostics, "原版物编目录无法捕获。")); return; }
            standard.emplace(std::move(*loaded.value));
        } else if (map->Units() && !map->Units()->units.empty()) { fail("预放单位的完整初始化检查缺少本次游戏物编目录。"); return; }
        objects.standard = standard ? &*standard : nullptr;
        for (std::size_t index = 0; index < 3; ++index) {
            Poll(cancel, deadline);
            const auto type = static_cast<ObjectData::ObjectType>(index);
            std::optional<ObjectData::NativeObjectDocument> native;
            if (next->nativeObjects[index]) native = *next->nativeObjects[index];
            else {
                if (archive->HasMember(ObjectData::NativeFileName(type))) { fail("现有物编成员没有可验证的native基线。"); return; }
                auto empty = ObjectData::NativeObjectDocument::CreateEmpty(type); if (empty.Succeeded()) native = std::move(*empty.value);
            }
            if (!native) { fail("物编基线无法冻结。"); return; }
            if (next->editableObjects[index]) {
                auto applied = ObjectData::ApplyLniChangesToNative(*native, *next->editableObjects[index], next->objectSchema.get(), objects.standard);
                if (!applied.Succeeded()) { fail(Reason(applied.diagnostics, "当前物编覆盖无法按保存规则回灌。")); return; }
            }
            auto& records = index == 0 ? objects.units : index == 1 ? objects.abilities : objects.items;
            records.assign(native->Objects().begin(), native->Objects().end());
        }
        auto capturedBases = Script::CaptureScriptBaseInputs({&*layout, Script::ScriptBasePolicy::BoundLayout,
            &*archive, next->baseReplacements, next->baseRemovals, game ? &*game : nullptr, {}, next->revision});
        if (!capturedBases.Succeeded() || !capturedBases.value) { fail(Reason(capturedBases.diagnostics, "实际common/blizzard来源无法捕获。")); return; }
        bases.emplace(std::move(*capturedBases.value));
        Poll(cancel, deadline);
        if (layout->executionPolicy == Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired) {
            auto profile = Script::MakeLegacyFrameworkProfile(*layout, *next->catalog,
                Trigger::UiCatalog::ResolveUiRoot(next->uiRoot.empty() ? next->toolRoot : next->uiRoot), archive->SourcePath().parent_path());
            if (!profile.Succeeded() || !profile.value) { fail(Reason(profile.diagnostics, "框架档案无法绑定。")); return; }
            profile.value->sourceEncodings = next->sourceEncodings;
            profile.value->ordinaryIncludeAccess = next->includeAccess;
            auto captured = Script::CaptureScriptFrameworkInputs(std::move(*profile.value), *layout, *next->catalog);
            if (!captured.Succeeded() || !captured.value) { fail(Reason(captured.diagnostics, "框架包含未能完整捕获。")); return; }
            framework.emplace(std::move(*captured.value));
        } else {
            auto request = ModernUserSources(*layout, *next, archive->SourcePath().parent_path());
            auto captured = Script::CaptureScriptUserSourcePlan(request);
            if (!captured.Succeeded() || !captured.value) { fail(Reason(captured.diagnostics, "实际函数库入口无法冻结。")); return; }
            entry.emplace(std::move(*captured.value));
            auto sources = Script::CaptureScriptInjectionSources(*entry, request, !layout->allowReturnBug);
            if (!sources.Succeeded() || !sources.value) { fail(Reason(sources.diagnostics, "实际注入函数库无法冻结。")); return; }
            injection.emplace(std::move(*sources.value));
        }
        Poll(cancel, deadline);
        templates = Script::BuildTemplateContext(next->toolRoot, standard ? &*standard : nullptr, objects.abilities);
        incomplete.clear(); captureFinished = true;
    }
};

CodeCheckProjectCache::CodeCheckProjectCache() : impl_(std::make_unique<Impl>()) {}
CodeCheckProjectCache::~CodeCheckProjectCache() = default;
std::string CodeCheckTargetSourceName(Trigger::CodeBlockTarget target, CodeLanguage language) {
    const std::string suffix = language == CodeLanguage::Lua53 ? ".lua" : ".j";
    return target.kind == Trigger::CodeBlockKind::GlobalHeader ? "WCT/header" + suffix
        : "WCT/trigger-" + std::to_string(target.triggerId) + suffix;
}

CodeCheckResult CheckProjectCode(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckProjectCache& cache, Trigger::CodeBlockTarget target, std::string draft,
    CodeLanguage language, std::uint64_t generation, std::stop_token cancel) {
    CodeCheckRequest request;
    request.language = language; request.source = {CodeCheckTargetSourceName(target, language), std::move(draft)};
    request.generation = generation; request.context.binding = snapshot ? snapshot->binding : "unbound";
    request.debug = snapshot && snapshot->debug;
    request.jass.cancel = cancel; request.lua.cancel = cancel;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    request.jass.deadline = deadline; request.lua.deadline = deadline;
    const auto fallback = [&](std::string reason) {
        request.context.complete = false; request.context.reason = "仅检查正文语法：" + std::move(reason);
        auto checked = CheckCode(request, cancel);
        if (language != CodeLanguage::Lua53) checked.reason = request.context.reason;
        return checked;
    };
    try {
        if (language == CodeLanguage::Lua53) {
            try { Detail::CaptureLuaCheckContext(snapshot, target, request, cancel); }
            catch (...) {
                request.context.luaSemantic = {};
                request.context.luaSemantic.runtime.reason = "本机运行库来源未能完成捕获，未形成语义资格。";
            }
            return CheckCode(request, cancel);
        }
        if (snapshot && snapshot->triggers && target.kind == Trigger::CodeBlockKind::CustomTrigger) {
            const auto selected = std::find_if(snapshot->triggers->triggers.begin(), snapshot->triggers->triggers.end(),
                [&](const auto& trigger) { return trigger.id == target.triggerId; });
            if (selected != snapshot->triggers->triggers.end()
                && selected->type == 0 && selected->customTextFlag != 0 && selected->enabled == 0)
                return fallback("该代码块已禁用，不属于本次生产整图；仍解析当前草稿，未授予整图语义通过资格。");
        }
        std::lock_guard lock(cache.impl_->mutex);
        auto& owned = *cache.impl_;
        if (!owned.captureFinished || owned.snapshot != snapshot || !snapshot
            || owned.snapshot->binding != snapshot->binding) owned.Capture(snapshot, cancel, deadline);
        if (!owned.incomplete.empty()) {
            // 框架/整图来源不完整时仍可展示已独立复核的实际基础库签名；不授予语义资格。
            if (owned.layout && owned.bases
                && Script::VerifyScriptToolchainLayout(*owned.layout).Succeeded()
                && Script::VerifyScriptBaseInputs(*owned.bases, *owned.layout, Script::ScriptBasePolicy::BoundLayout).Succeeded()) {
                request.context.declarations = {{"Save/common.j", BytesText(owned.bases->common.bytes)},
                    {"Save/blizzard.j", BytesText(owned.bases->blizzard.bytes)}};
                request.jass.returnBug = owned.bases->allowReturnBug;
            }
            return fallback(owned.incomplete);
        }
        Poll(cancel, deadline);
        auto currentLayout = Script::VerifyScriptToolchainLayout(*owned.layout);
        auto currentBases = Script::VerifyScriptBaseInputs(*owned.bases, *owned.layout, Script::ScriptBasePolicy::BoundLayout);
        if (!currentLayout.Succeeded() || !currentBases.Succeeded()) {
            owned.captureFinished = false;
            return fallback("基础库或工具链实际来源已改变，下次检查将重新捕获。");
        }
        request.context.declarations = {{"Save/common.j", BytesText(owned.bases->common.bytes)},
            {"Save/blizzard.j", BytesText(owned.bases->blizzard.bytes)}};
        request.jass.returnBug = owned.bases->allowReturnBug;
        if (owned.framework) {
            const auto current = Script::VerifyScriptFrameworkInputs(*owned.framework, *owned.layout);
            if (!current.Succeeded()) { owned.captureFinished = false; return fallback(Reason(current.diagnostics, "框架来源已改变。")); }
        } else {
            auto sources = ModernUserSources(*owned.layout, *snapshot, owned.archive->SourcePath().parent_path());
            const auto current = Script::VerifyScriptInjectionSources(*owned.injection, *owned.entry, sources, !owned.layout->allowReturnBug);
            if (!current.Succeeded()) { owned.captureFinished = false; return fallback(Reason(current.diagnostics, "函数库注入来源已改变。")); }
        }
        auto document = *snapshot->triggers;
        auto plan = Trigger::TriggerCodeEditor::PrepareText(document, *snapshot->catalog, target, request.source.text);
        if (!plan.Succeeded() || !plan.value) return fallback(Reason(plan.diagnostics, "草稿尚不能进入私有作者候选。"));
        auto applied = Trigger::TriggerCodeEditor::Apply(document, *snapshot->catalog, *plan.value);
        if (!applied.Succeeded()) return fallback(Reason(applied.diagnostics, "私有作者候选来源已过期。"));
        Script::MapScriptInputs inputs;
        inputs.triggers = &document; inputs.catalog = snapshot->catalog.get(); inputs.queries = Script::BuildObjectQueries(owned.objects);
        Script::AttachSoundQueries(inputs.queries, owned.game ? &*owned.game : nullptr, &*owned.archive);
        inputs.settings = &*owned.map->Settings(); inputs.units = owned.map->Units() ? &*owned.map->Units() : nullptr;
        inputs.doodads = owned.map->Doodads() ? &*owned.map->Doodads() : nullptr;
        inputs.regions = owned.map->Regions() ? &*owned.map->Regions() : nullptr;
        inputs.cameras = owned.map->Cameras() ? &*owned.map->Cameras() : nullptr;
        inputs.sounds = owned.map->Sounds() ? &*owned.map->Sounds() : nullptr;
        inputs.strings = owned.map->Strings() ? &*owned.map->Strings() : nullptr;
        inputs.terrain = snapshot->terrain; inputs.debug = snapshot->debug;
        std::error_code error;
        inputs.nullSafeHandleSet = std::any_of(owned.layout->systemIncludes.begin(), owned.layout->systemIncludes.end(),
            [&](const auto& directory) { return std::filesystem::exists(directory / "YDTrigger/SaveLoadSystem/HTSystem.j", error); });
        auto generated = Script::GenerateMapScript(inputs);
        if (!generated.Succeeded()) return fallback(Reason(generated.diagnostics, "生产整图生成尚不能完成。"));
        if (std::none_of(generated.authorCode.begin(), generated.authorCode.end(),
            [&](const auto& span) { return span.triggerId == target.triggerId; }))
            return fallback("生产整图未发射当前代码块，不能用其他脚本的检查结果代表此草稿。");
        auto author = AuthorMap(generated, cancel, deadline);
        std::string injected;
        std::vector<std::filesystem::path> paths;
        Jass::PreprocessResult preprocessed;
        if (owned.framework) {
            Jass::SourceLexicalView lexical;
            auto injection = Script::InjectScriptFramework(generated.text, *owned.framework, paths, &lexical);
            if (!injection.Succeeded() || !injection.value) return fallback(Reason(injection.diagnostics, "框架注入尚不能完成。"));
            MapInjected(author, generated.text, paths, owned.framework->injection, true, cancel, deadline);
            Script::ScriptPreprocessSourceRequest preprocess;
            preprocess.check = request.jass; preprocess.check.maxSources = 1024;
            preprocess.mainText = std::move(*injection.value); preprocess.mainDirectory = owned.framework->profile.mainSourceDirectory;
            preprocess.frameworkDirectivePolicy = Script::FrameworkDirectivePolicy(*owned.framework);
            preprocess.includeCandidatePolicy = Script::ScriptIncludeCandidatePolicy::BoundRootsSearch;
            preprocess.mainLexicalView = std::move(lexical);
            preprocess.defines = {{"WARCRAFT_VERSION", std::to_string(owned.layout->versionMacro)}, {"YDWE_VERSION_STRING", "\"1.32.13\""}, {"SCRIPT_INJECTION", "1"}};
            if (snapshot->debug) preprocess.defines.emplace_back("DEBUG", "1");
            auto captured = Script::CaptureScriptPreprocessSources(owned.framework->entryPlan,
                Script::FrameworkSourceRequest(*owned.framework), preprocess);
            if (!captured.Succeeded() || !captured.value) return fallback(Reason(captured.diagnostics, "本次草稿的框架预处理来源无法冻结。"));
            auto valid = Script::ValidateFrameworkIncludes(*owned.framework, *captured.value);
            if (!valid.Succeeded()) return fallback(Reason(valid.diagnostics, "框架包含编码尚不能绑定。"));
            preprocessed = Script::ReplayScriptPreprocessSources(*captured.value);
            if (preprocessed.text != captured.value->preprocessed.text) return fallback("owned预处理回放与同次草稿捕获不一致。");
            for (const auto& source : captured.value->includes)
                if (source.binding.kind == Script::ScriptToolchainBindingKind::RegularFile)
                    request.context.includes.push_back({PathText(source.binding.path), BytesText(source.bytes)});
        } else {
            auto injection = Script::InjectOwnedCode(generated.text, *owned.injection, request.jass, &paths);
            if (!injection.Succeeded() || !injection.value) return fallback(Reason(injection.diagnostics, "函数库注入尚不能完成。"));
            injected = std::move(*injection.value);
            MapInjected(author, generated.text, paths, *owned.injection, false, cancel, deadline);
            Jass::PreprocessOptions options;
            options.check = request.jass; options.check.maxSources = 1024;
            options.systemIncludes = owned.layout->systemIncludes; options.userIncludes = owned.entry->orderedUserIncludes;
            options.defines = {{"WARCRAFT_VERSION", std::to_string(owned.layout->versionMacro)}, {"YDWE_VERSION_STRING", "\"1.32.13\""}, {"SCRIPT_INJECTION", "1"}};
            if (snapshot->debug) options.defines.emplace_back("DEBUG", "1");
            for (const auto& path : owned.layout->forceIncludes) options.forceIncludes.push_back(PathText(path));
            options.maxOutputBytes = 16 * 1024 * 1024;
            std::map<std::filesystem::path, Jass::PreprocessIncludeContent> includes;
            std::size_t bytes = 0;
            options.includeCandidatePolicy = [&](const Jass::IncludeCandidateRequest& candidate) {
                const auto allowed = Within(candidate.candidate, owned.archive->SourcePath().parent_path())
                    || std::any_of(options.systemIncludes.begin(), options.systemIncludes.end(), [&](const auto& root) { return Within(candidate.candidate, root); })
                    || std::any_of(options.userIncludes.begin(), options.userIncludes.end(), [&](const auto& root) { return Within(candidate.candidate, root); });
                return Result<Jass::IncludeCandidateEligibility>::Success(allowed ? Jass::IncludeCandidateEligibility::Allowed
                    : Jass::IncludeCandidateEligibility::SkippedOutOfScope);
            };
            options.includeProvider = [&](const std::filesystem::path& path) -> Result<Jass::PreprocessIncludeContent> {
                Poll(cancel, deadline);
                const auto name = path.lexically_normal();
                if (const auto found = includes.find(name); found != includes.end()) return Result<Jass::PreprocessIncludeContent>::Success(found->second);
                if (includes.size() >= 512) return Result<Jass::PreprocessIncludeContent>::Failure("Code.Check", "CODE_CHECK_INCLUDE_BUDGET", "包含候选超过512项。");
                auto source = Script::CaptureReadOnlyScriptSource(name);
                if (!source.Succeeded() || !source.value) return {{}, std::move(source.diagnostics)};
                if (source.value->bytes.size() > 32 * 1024 * 1024 - bytes) return Result<Jass::PreprocessIncludeContent>::Failure("Code.Check", "CODE_CHECK_INCLUDE_BUDGET", "包含文本超过32MiB。");
                bytes += source.value->bytes.size();
                Jass::PreprocessIncludeContent content{source.value->binding.kind == Script::ScriptToolchainBindingKind::RegularFile, BytesText(source.value->bytes)};
                includes.emplace(name, content); return Result<Jass::PreprocessIncludeContent>::Success(std::move(content));
            };
            preprocessed = Jass::Preprocess(injected, "war3map.j", owned.archive->SourcePath().parent_path(), options);
            for (const auto& [path, source] : includes)
                if (source.exists) request.context.includes.push_back({PathText(path), source.text});
        }
        Poll(cancel, deadline);
        if (!preprocessed.Succeeded()) return fallback(Reason(preprocessed.diagnostics, "预处理未能完成。"));
        request.sourceMap = MapPreprocessed(preprocessed, author, cancel, deadline);
        if (preprocessed.text.find("<?") != std::string::npos) {
            // 与保存链相同的内置模板求值（如 YDNL 展开为换行）；需要整段执行的块（物编生成等）不在编辑器里执行。
            auto evaluated = Script::EvaluateTemplates(preprocessed.text, *owned.templates);
            if (!evaluated.Succeeded()) {
                std::vector<Diagnostic> errors;
                std::copy_if(evaluated.diagnostics.begin(), evaluated.diagnostics.end(), std::back_inserter(errors),
                    [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; });
                const auto reason = Reason(errors, "模板求值未能完成");
                if (auto section = CheckAuthorSection(request, preprocessed.text, *owned.templates, reason, cancel, deadline))
                    return std::move(*section);
                return fallback(reason + "；编辑器不执行该模板，保存链仍完整执行并检查。");
            }
            Poll(cancel, deadline);
            const auto lines = LineCount(evaluated.text, cancel, deadline);
            request.sourceMap = MapTemplateOutput(preprocessed.text, evaluated, cancel, deadline).ComposeWith(request.sourceMap, lines);
            preprocessed.text = std::move(evaluated.text);
        }
        request.source.text = std::move(preprocessed.text);
        request.context.declarations = {{"Save/common.j", BytesText(owned.bases->common.bytes)}, {"Save/blizzard.j", BytesText(owned.bases->blizzard.bytes)}};
        request.context.complete = true; request.jass.returnBug = owned.bases->allowReturnBug;
        request.language = CodeLanguage::VJass; // 整图依赖也可能含库；统一展开后仍交给同一JASS检查器。
        request.preprocessed = true;
        auto result = CheckCode(request, cancel);
        std::size_t externalBytes = 0;
        for (const auto& source : result.externalSources) externalBytes += source.text.size();
        if (externalBytes > 32 * 1024 * 1024) {
            result.externalSources.clear(); externalBytes = 0; result.externalSourcesComplete = false;
        }
        // GUI/初始化声明映射到这一份实际生成正文；导航使用同次冻结内容。
        AddExternalSource(result, "Map/generated.j", generated.text, externalBytes);
        const auto& injectionSources = owned.framework ? owned.framework->injection : *owned.injection;
        for (const auto& path : paths) {
            Poll(cancel, deadline);
            const auto found = std::find_if(injectionSources.files.begin(), injectionSources.files.end(),
                [&](const auto& source) { return source.binding.path == path; });
            if (found != injectionSources.files.end()) AddExternalSource(result, PathText(path), BytesText(found->bytes), externalBytes);
        }
        for (const auto& span : generated.authorCode) {
            Poll(cancel, deadline);
            const auto* trigger = span.triggerId == 0 ? nullptr
                : Trigger::TriggerDocumentEditor::FindTrigger(document, span.triggerId);
            const auto* block = span.triggerId == 0 ? &document.wct.customCode : trigger ? &trigger->customText : nullptr;
            if (block) AddExternalSource(result, CodeCheckTargetSourceName({span.triggerId == 0
                ? Trigger::CodeBlockKind::GlobalHeader : Trigger::CodeBlockKind::CustomTrigger, span.triggerId}),
                Trigger::TriggerCodeEditor::NormalizedText(*block), externalBytes);
        }
        VerifySemanticLineCopies(result, request.source.text, cancel, deadline);
        if (!result.externalSourcesComplete) result.reason += " 只读导航来源清单达到显示预算，尚未完整列出。";
        return result;
    } catch (const std::exception&) {
        CodeCheckResult failed;
        failed.generation = generation; failed.contextBinding = request.context.binding; failed.cancelled = cancel.stop_requested();
        failed.reason = failed.cancelled ? "代码检查已取消。" : "代码检查未能完整执行或超过时间预算。";
        failed.diagnostics.push_back({DiagnosticSeverity::Error, {}, failed.cancelled ? "CODE_CHECK_CANCELLED" : "CODE_CHECK_CONTEXT_INCOMPLETE", failed.reason});
        return failed;
    }
}
} // namespace War3Map::Services
