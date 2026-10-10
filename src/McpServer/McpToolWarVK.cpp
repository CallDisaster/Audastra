#include "McpToolInternal.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"

#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace War3Map::Mcp {
namespace {

constexpr std::string_view kWarVKManifestSha256Upper =
    "920872221B3836A5EFF69D3EC721915B21E0C4B5399C0F09F05B028CF46D27BF";
constexpr std::string_view kWarVKManifestSha256Lower =
    "920872221b3836a5eff69d3ec721915b21e0c4b5399c0f09f05b028cf46d27bf";
constexpr std::string_view kWarVKWrapperSha256Lower =
    "e222e2044ecd3763292c576df34926a0cc6fe954618cc0e3f2fab7bdf31c2606";
constexpr std::string_view kWarVKHandoffSha256Lower =
    "d42af5a46d0d10159de7708ba84f07316305eeb0fa1155735d6026cc22554ab8";

struct PinnedWarVKArtifact {
    std::string_view path;
    std::string_view sha256;
    std::uintmax_t bytes;
    bool listedInHandoff;
};

constexpr std::array kPinnedWarVKArtifacts{
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/manifest/warvk_v1.json",
        kWarVKManifestSha256Lower, 36779, false},
    PinnedWarVKArtifact{
        "Core/WarVK/Generated/WarVKDispatch.generated.h",
        "6ea687318cdc281231c59a9c5bf2a380c62cbef08a162d24a53ccaa72da84544",
        24205, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/warvk_v1.j",
        kWarVKWrapperSha256Lower, 17878, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/TriggerData.txt",
        "b59d9cee850f3820539361f291293e9a9231b834101892dfd84dfe7714ab9144",
        7782, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/TriggerStrings.txt",
        "3d1a1492de35147d9a9aee641d7caddcedefcf2ca183a9340d27c92966337a20",
        11252, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/mapui/action.txt",
        "65df8a86e9c0f0d430c3880d27ed71e0665cf62112d98bbf20eda1ec579b87f2",
        17046, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/mapui/call.txt",
        "03771993d7eaac58efde59acf07d08c5ad8340f7ec2e9b1ccbc7120058efdf5f",
        8259, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/mapui/define.txt",
        "c77308a9bd4884397482688d28def3ea450a9cb7a611e470a39bde1c5d75f513",
        124, true},
    PinnedWarVKArtifact{
        "Tools/WarVKJapi/generated/protocol_vectors.json",
        "57b12be605629aca750767da73b74f211ddddebcc392c2a2323317fa416f8596",
        13870, true},
};



Status RequireOrdinaryPath(const std::filesystem::path& path, bool directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Status::Failure(
            "MCP", "MCP_WARVK_ARTIFACT_MISSING",
            "找不到固定 WarVK 生成物：" + path.string());
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Status::Failure(
            "MCP", "MCP_WARVK_REPARSE_FORBIDDEN",
            "WarVK 生成物路径禁止符号链接、junction 或其他 reparse point：" +
                path.string());
    }
    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDirectory != directory) {
        return Status::Failure(
            "MCP", "MCP_WARVK_ARTIFACT_KIND_MISMATCH",
            "WarVK 生成物文件/目录类型不符合固定清单：" + path.string());
    }
    return Status::Ok();
}

Result<VerifiedWarVKModule> LoadPinnedWarVKModule(
    const std::filesystem::path& repositoryRoot,
    std::string_view expectedManifestSha256) {
    if (!repositoryRoot.is_absolute() ||
        expectedManifestSha256 != kWarVKManifestSha256Upper) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_MANIFEST_NOT_PINNED",
            "generated_repository_root 必须是绝对路径，且 manifest SHA-256 必须精确匹配编辑器固定代际。");
    }
    const Status rootStatus = RequireOrdinaryPath(repositoryRoot, true);
    if (!rootStatus.Succeeded()) {
        Result<VerifiedWarVKModule> result;
        result.diagnostics = rootStatus.diagnostics;
        return result;
    }

    for (const PinnedWarVKArtifact& artifact : kPinnedWarVKArtifacts) {
        const std::filesystem::path path = repositoryRoot /
            std::filesystem::path(std::string(artifact.path));
        const Status ordinary = RequireOrdinaryPath(path, false);
        if (!ordinary.Succeeded()) {
            Result<VerifiedWarVKModule> result;
            result.diagnostics = ordinary.diagnostics;
            return result;
        }
        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
        const auto digest = Sha256File(path);
        if (sizeError || size != artifact.bytes || !digest.Succeeded() ||
            *digest.value != artifact.sha256) {
            return Result<VerifiedWarVKModule>::Failure(
                "MCP", "MCP_WARVK_ARTIFACT_HASH_MISMATCH",
                "WarVK 固定生成物长度或 SHA-256 不匹配：" + path.string());
        }
    }

    const std::filesystem::path generatedDirectory =
        repositoryRoot / "Tools/WarVKJapi/generated";
    const Status generatedStatus = RequireOrdinaryPath(generatedDirectory, true);
    if (!generatedStatus.Succeeded()) {
        Result<VerifiedWarVKModule> result;
        result.diagnostics = generatedStatus.diagnostics;
        return result;
    }
    const Status mapUiStatus = RequireOrdinaryPath(generatedDirectory / "mapui", true);
    if (!mapUiStatus.Succeeded()) {
        Result<VerifiedWarVKModule> result;
        result.diagnostics = mapUiStatus.diagnostics;
        return result;
    }
    const std::set<std::string> expectedGeneratedFiles{
        "TriggerData.txt", "TriggerStrings.txt", "protocol_vectors.json",
        "warvk_v1.j", "warvk_v1_handoff_manifest.json",
        "mapui/action.txt", "mapui/call.txt", "mapui/define.txt"};
    std::set<std::string> observedGeneratedFiles;
    std::error_code walkError;
    for (std::filesystem::recursive_directory_iterator iterator(
             generatedDirectory,
             std::filesystem::directory_options::none,
             walkError), end;
         !walkError && iterator != end; iterator.increment(walkError)) {
        const Status ordinary = RequireOrdinaryPath(
            iterator->path(), iterator->is_directory());
        if (!ordinary.Succeeded()) {
            Result<VerifiedWarVKModule> result;
            result.diagnostics = ordinary.diagnostics;
            return result;
        }
        const std::string relative = iterator->path()
            .lexically_relative(generatedDirectory).generic_string();
        if (iterator->is_directory()) {
            if (relative != "mapui") {
                return Result<VerifiedWarVKModule>::Failure(
                    "MCP", "MCP_WARVK_GENERATED_EXTRA",
                    "WarVK generated 目录包含固定清单之外的目录：" + relative);
            }
        } else if (iterator->is_regular_file()) {
            observedGeneratedFiles.insert(relative);
        } else {
            return Result<VerifiedWarVKModule>::Failure(
                "MCP", "MCP_WARVK_ARTIFACT_KIND_MISMATCH",
                "WarVK generated 目录包含非普通文件：" + relative);
        }
    }
    if (walkError || observedGeneratedFiles != expectedGeneratedFiles) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_GENERATED_SET_MISMATCH",
            "WarVK generated 目录必须与固定文件集合完全一致，禁止缺失或额外产物。");
    }

    const std::filesystem::path handoffPath =
        generatedDirectory / "warvk_v1_handoff_manifest.json";
    const auto handoffDigest = Sha256File(handoffPath);
    if (!handoffDigest.Succeeded() ||
        *handoffDigest.value != kWarVKHandoffSha256Lower) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_HANDOFF_HASH_MISMATCH",
            "WarVK handoff manifest 不是编辑器固定审核版本。");
    }
    QFile handoffFile(QString::fromStdWString(handoffPath.wstring()));
    if (!handoffFile.open(QIODevice::ReadOnly)) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_HANDOFF_OPEN_FAILED",
            "无法只读打开 WarVK handoff manifest。");
    }
    QJsonParseError parseError{};
    const QJsonDocument handoffDocument = QJsonDocument::fromJson(
        handoffFile.readAll(), &parseError);
    const QJsonObject handoff = handoffDocument.object();
    if (parseError.error != QJsonParseError::NoError ||
        !handoffDocument.isObject() || handoff.value("schemaVersion").toInt() != 1 ||
        handoff.value("artifact").toString() != "WarVK JAPI v1 static handoff" ||
        handoff.value("sourceManifestSha256").toString().toStdString() !=
            kWarVKManifestSha256Upper ||
        handoff.value("protocolVersion").toInt() != 1 ||
        handoff.value("commandCount").toInt() != 55 ||
        handoff.value("runtimeEffectClaimed").toBool(true) ||
        !handoff.value("localVisualOnly").toBool(false)) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_HANDOFF_SCHEMA_MISMATCH",
            "WarVK handoff manifest 的固定 schema 或安全声明不匹配。");
    }
    std::set<std::string> listedPaths;
    for (const QJsonValue& value : handoff.value("files").toArray()) {
        const QJsonObject entry = value.toObject();
        const std::string path = entry.value("path").toString().toStdString();
        const auto expected = std::find_if(
            kPinnedWarVKArtifacts.begin(), kPinnedWarVKArtifacts.end(),
            [&path](const PinnedWarVKArtifact& artifact) {
                return artifact.listedInHandoff && artifact.path == path;
            });
        if (expected == kPinnedWarVKArtifacts.end() ||
            Text::LowerCType(entry.value("sha256").toString().toStdString()) !=
                expected->sha256 ||
            entry.value("bytes").toDouble(-1) !=
                static_cast<double>(expected->bytes) ||
            !listedPaths.insert(path).second) {
            return Result<VerifiedWarVKModule>::Failure(
                "MCP", "MCP_WARVK_HANDOFF_FILE_MISMATCH",
                "WarVK handoff 文件清单、长度或 SHA-256 不匹配。");
        }
    }
    const std::size_t expectedListed = static_cast<std::size_t>(std::count_if(
        kPinnedWarVKArtifacts.begin(), kPinnedWarVKArtifacts.end(),
        [](const PinnedWarVKArtifact& artifact) {
            return artifact.listedInHandoff;
        }));
    if (listedPaths.size() != expectedListed) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_HANDOFF_FILE_SET_MISMATCH",
            "WarVK handoff 没有精确列出全部固定生成物。");
    }

    const std::filesystem::path wrapperPath = generatedDirectory / "warvk_v1.j";
    QFile wrapperFile(QString::fromStdWString(wrapperPath.wstring()));
    if (!wrapperFile.open(QIODevice::ReadOnly)) {
        return Result<VerifiedWarVKModule>::Failure(
            "MCP", "MCP_WARVK_WRAPPER_OPEN_FAILED",
            "无法只读打开固定 WarVK JASS wrapper。");
    }
    const QByteArray wrapperBytes = wrapperFile.readAll();
    VerifiedWarVKModule module;
    module.sourceManifestSha256 = std::string(kWarVKManifestSha256Lower);
    module.wrapperSha256 = std::string(kWarVKWrapperSha256Lower);
    module.wrapperText.assign(wrapperBytes.constData(),
        static_cast<std::size_t>(wrapperBytes.size()));
    return Result<VerifiedWarVKModule>::Success(std::move(module));
}

} // namespace

void RegisterWarVKTool(McpServer& server) {
    {
        ToolDefinition tool;
        tool.name = "install_generated_warvk_module";
        tool.permission = ToolPermission::Edit;
        tool.description =
            "校验固定 WarVK v1 manifest/handoff/全部生成物后，新建本地视觉 AutoTest custom trigger；不接受任意 JASS";
        tool.inputSchema = BasicSchema({
            {"expected_manifest_sha256", QJsonObject{
                {"type", "string"},
                {"pattern", "^[0-9A-F]{64}$"},
            }},
        }, {
            "transaction_id", "expected_revision", "category_id",
            "generated_repository_root", "expected_manifest_sha256"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            auto categoryId = ParseInt32(parameters, "category_id");
            if (!envelope.Succeeded()) return Failure(envelope);
            if (!categoryId.Succeeded()) return Failure(categoryId);
            const QJsonValue rootValue =
                parameters.value("generated_repository_root");
            const QJsonValue manifestValue =
                parameters.value("expected_manifest_sha256");
            if (!rootValue.isString() || rootValue.toString().isEmpty() ||
                !manifestValue.isString()) {
                return Failure(
                    "MCP_WARVK_PATH_OR_MANIFEST_REQUIRED",
                    "必须提供非空生成仓库绝对路径与固定 manifest SHA-256。");
            }
            const std::filesystem::path repositoryRoot(
                rootValue.toString().toStdWString());
            auto module = LoadPinnedWarVKModule(
                repositoryRoot,
                manifestValue.toString().toStdString());
            if (!module.Succeeded()) return Failure(module);
            auto installed = workspace->InstallGeneratedWarVKModule(
                envelope.value->first,
                envelope.value->second,
                *categoryId.value,
                std::move(*module.value));
            if (!installed.Succeeded()) return Failure(installed);
            return QJsonObject{
                {"ok", true},
                {"trigger_id", QString::number(*installed.value)},
                {"trigger_name", "WarVK_AutoTest"},
                {"custom_text", true},
                {"run_on_init", true},
                {"local_visual_only", true},
                {"manifest_sha256", QString::fromLatin1(
                    kWarVKManifestSha256Upper.data(),
                    static_cast<qsizetype>(kWarVKManifestSha256Upper.size()))},
            };
        };
        server.RegisterTool(std::move(tool));
    }

}

} // namespace War3Map::Mcp
