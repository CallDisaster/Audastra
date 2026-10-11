#include "WorldViewportToolService.h"
#include "WorldViewport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>

namespace War3Map::Modules {
namespace {
QJsonObject Failure(const std::vector<Diagnostic>& diagnostics) {
    QJsonArray values;
    for (const auto& d : diagnostics) values.append(QJsonObject{{"severity", "error"},
        {"subsystem", QString::fromStdString(d.subsystem)}, {"code", QString::fromStdString(d.code)},
        {"message", QString::fromStdString(d.message)}});
    return {{"ok", false}, {"diagnostics", values}};
}
QJsonObject Failure(const char* code, const char* message) {
    return {{"ok", false}, {"diagnostics", QJsonArray{QJsonObject{{"severity", "error"},
        {"subsystem", "World"}, {"code", code}, {"message", QString::fromUtf8(message)}}}}};
}
QJsonObject Camera(const War3RenderCameraV2& camera) {
    return {{"target_x", camera.targetX}, {"target_y", camera.targetY}, {"target_z", camera.targetZ},
        {"distance", camera.distance}, {"yaw_radians", camera.yawRadians}, {"pitch_radians", camera.pitchRadians}};
}
} // namespace
WorldViewportToolService::WorldViewportToolService(ViewportProvider provider) : provider_(std::move(provider)) {}
void WorldViewportToolService::Collect() {
    if (auto result = encoder_.Poll()) result_ = std::move(*result);
}
QJsonObject WorldViewportToolService::Invoke(const QString& name, const QJsonObject& args) {
    auto* viewport = provider_ ? provider_() : nullptr;
    if (!viewport) return Failure("WORLD_VIEWPORT_UNAVAILABLE", "世界视口尚未打开或窗口已关闭。");
    if (QThread::currentThread() != viewport->thread())
        return Failure("WORLD_VIEWPORT_THREAD", "视口工具必须在视口所属线程执行。");
    const auto revision = viewport->DocumentRevision();
    QJsonObject binding{{"ok", true}, {"project_identity", args.value("project_identity")},
        {"revision", args.value("expected_revision")}, {"viewport_revision", QString::number(revision)}};
    if (name == "world_viewport_camera_read") {
        const auto camera = viewport->ReadCamera(revision);
        if (!camera.Succeeded()) return Failure(camera.diagnostics);
        binding.insert("camera", Camera(*camera.value)); binding.insert("camera_state", "requested");
        return binding;
    }
    if (name == "world_viewport_camera_set" || name == "world_viewport_capture") {
        bool parsed = false;
        const auto expected = args.value("expected_viewport_revision").toString().toULongLong(&parsed);
        if (!parsed || expected != revision)
            return Failure("WORLD_VIEWPORT_REVISION", "视口修订已变化，命令未执行；重新读取视口相机。");
    }
    if (name == "world_viewport_camera_set") {
        const auto fields = args.value("camera").toObject();
        War3RenderCameraV2 camera{sizeof(camera), static_cast<float>(fields.value("target_x").toDouble()),
            static_cast<float>(fields.value("target_y").toDouble()), static_cast<float>(fields.value("target_z").toDouble()),
            static_cast<float>(fields.value("distance").toDouble()), static_cast<float>(fields.value("yaw_radians").toDouble()),
            static_cast<float>(fields.value("pitch_radians").toDouble())};
        const auto dryRun = args.value("dry_run").toBool();
        const auto status = dryRun ? viewport->CheckCameraChange(camera, revision) : viewport->SetCamera(camera, revision);
        if (!status.Succeeded()) return Failure(status.diagnostics);
        binding.insert("dry_run", dryRun); binding.insert("camera_state", "requested");
        if (!dryRun) {
            const auto current = viewport->ReadCamera(revision);
            if (!current.Succeeded()) return Failure(current.diagnostics);
            camera = *current.value;
        }
        binding.insert("camera", Camera(camera)); return binding;
    }
    Collect();
    if (name == "world_viewport_capture") {
        if (encoder_.Busy()) return Failure("WORLD_CAPTURE_BUSY", "上一张完成帧仍在后台编码。");
        const auto bytes = args.contains("max_rgba_bytes") ? static_cast<std::size_t>(args.value("max_rgba_bytes").toDouble())
            : Render::kDefaultFrameCaptureBytes;
        auto frame = viewport->CaptureCompletedFrame(revision, bytes);
        if (!frame.Succeeded()) return Failure(frame.diagnostics);
        auto input = MakeWorldScreenshotEncodeInput(std::move(*frame.value));
        // 完成帧来源修订保留原字段；本次命令的编辑身份明确单列，不冒充渲染来源。
        input.metadata.insert("request", QJsonObject{{"project_identity", args.value("project_identity")},
            {"expected_edit_revision", args.value("expected_revision")}});
        if (!encoder_.Submit(std::move(input))) return Failure("WORLD_CAPTURE_BUSY", "后台编码槽已被占用。");
        request_ = CaptureRequest{QUuid::createUuid().toString(QUuid::WithoutBraces),
            args.value("project_identity").toString(), args.value("expected_revision").toString(), viewport, revision};
        result_.reset(); binding.insert("capture_id", request_->id); binding.insert("status", "pending");
        return binding;
    }
    if (name != "world_viewport_capture_result") return Failure("WORLD_VIEWPORT_TOOL_UNKNOWN", "未知视口命令。");
    if (!request_ || args.value("capture_id").toString() != request_->id)
        return Failure("WORLD_CAPTURE_ID", "截图请求不存在或已被后续请求替代。");
    if (request_->viewport != viewport || request_->viewportRevision != revision
        || request_->projectIdentity != args.value("project_identity").toString()
        || request_->editRevision != args.value("expected_revision").toString())
        return Failure("WORLD_CAPTURE_CONTEXT_CHANGED", "工程、编辑修订或视口已变化，旧截图未交付。");
    binding.insert("capture_id", request_->id);
    if (!result_) { binding.insert("status", "pending"); return binding; }
    if (!result_->Succeeded()) {
        auto failed = Failure("WORLD_CAPTURE_ENCODE_FAILED", "完成帧PNG编码失败。");
        failed.insert("encoding_diagnostic", result_->diagnostic); return failed;
    }
    if (result_->png.size() > 4 * 1024 * 1024 || QJsonDocument(result_->metadata).toJson(QJsonDocument::Compact).size() > 64 * 1024)
        return Failure("WORLD_CAPTURE_RESPONSE_BUDGET", "完成帧PNG或元数据超出交付预算；请缩小视口后重试。");
    binding.insert("status", "ready"); binding.insert("metadata", result_->metadata);
    binding.insert("png_bytes", QString::number(result_->png.size()));
    if (args.value("include_png").toBool()) binding.insert("image", QJsonObject{{"mime_type", "image/png"},
        {"encoding", "base64"}, {"data", QString::fromLatin1(result_->png.toBase64())}});
    return binding;
}
} // namespace War3Map::Modules
