#include "War3MapCore/HLua/Test.h"
#include "War3MapCore/ApplicationPaths.h"
#include "War3MapCore/Services/LuaObjectSnapshot.h"
#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "War3MapCore/IO/FileBytes.h"

namespace War3Map::HLua {
TestSession::~TestSession() {
    if (launch_.lease && launch_.lease->IsActive()) (void)launch_.lease->Stop();
    if (launch_.lease && !launch_.lease->IsActive()) (void)Cleanup();
}
Result<Services::LuaTestReloadSnapshot> TestSession::QueryLua() const {
    if (!launch_.luaMailbox) return Result<Services::LuaTestReloadSnapshot>::Failure("HLua", "HLUA_TEST_MAILBOX_UNAVAILABLE", "所选环境没有自带 Lua 测试邮箱。");
    return Services::QueryLuaTestReload(*launch_.luaMailbox);
}
Status TestSession::Stop() { return launch_.lease ? launch_.lease->Stop() : Status::Failure("HLua", "HLUA_TEST_NOT_STARTED", "没有本次测试进程。"); }
Status TestSession::Cleanup() {
    if (!launch_.lease || launch_.lease->IsActive()) return Status::Failure("HLua", "HLUA_TEST_ACTIVE", "测试进程尚未退出，保留工作区。");
    auto cleaned = launch_.lease->Cleanup();
    if (!cleaned.Succeeded()) return cleaned;
    return workspace_ ? workspace_->Cleanup(true) : Status::Ok();
}
Status ValidateTestEngineSources(const std::filesystem::path& map, std::string_view expectedSha256, Services::MapTestLuaEngine engine) {
    // 在选择运行环境之前统一核对，旧的扩展接口回退路径也不能绕过此门。
    {
        auto archive = MapArchiveSession::Open(map);
        if (!archive.Succeeded()) return {std::move(archive.diagnostics)};
        if (archive.value->SourceSha256()!=expectedSha256) return Status::Failure("HLua","HLUA_TEST_SOURCE_CHANGED","测试源地图身份已改变。");
        auto files = Services::CaptureMapLuaFiles(*archive.value);
        if (!files.Succeeded()) return {std::move(files.diagnostics)};
        for (const auto& file : *files.value) {
            const std::string_view text(reinterpret_cast<const char*>(file.bytes->data()), file.bytes->size());
            const auto uses = Lua::ScanExternalCapabilities(text);
            if (!uses.empty()) return Status::Failure("HLua", "HLUA_TEST_EXTERNAL_CAPABILITY",
                file.path + ':' + std::to_string(uses.front().line) + "：发现外部能力访问 " + uses.front().name + "，未启动宽松引擎。");
        }
        Status result;
        result.diagnostics.push_back({DiagnosticSeverity::Info,"HLua","HLUA_TEST_SOURCE_AUDIT",
            "已扫描 " + std::to_string(files.value->size()) + " 份 Lua；io、os.execute、package.loadlib 字面访问为 0；地图 SHA-256="
            + std::string(expectedSha256) + "；请求环境=" + (engine==Services::MapTestLuaEngine::Ydwe ? "ydwe" : "owned")
            + "。动态代码不作为完整性证明。",{}});
        return result;
    }
}
Result<std::shared_ptr<TestSession>> StartProjectTest(const TestRequest& request) {
    using Output = Result<std::shared_ptr<TestSession>>;
    if (request.build.mode != BuildTarget::Test || request.build.idsOnly || request.helpers.empty())
        return Output::Failure("HLua", "HLUA_TEST_ARGUMENT", "测试需要 test 目标与本机已安装的测试辅助程序。");
    auto built = BuildProject(request.build);
    if (!built.Succeeded()) return {std::nullopt, std::move(built.diagnostics)};
    const auto permitted=ValidateTestEngineSources(built.value->output,built.value->outputSha256,request.engine);
    if (!permitted.Succeeded()) return {std::nullopt,permitted.diagnostics};
    const auto root = std::filesystem::absolute(request.build.workspace) / "test-session";
    if (!std::filesystem::create_directory(root)) return Output::Failure("HLua", "HLUA_TEST_WORKSPACE", "无法创建独占测试目录。");
    auto frozen = Services::FreezeMapTestWorkspace(root);
    if (!frozen.Succeeded()) return {std::nullopt, std::move(frozen.diagnostics)};
    Services::MapTestRequest run;
    run.inputMap = std::filesystem::absolute(built.value->output); run.sourceMap = run.inputMap;
    run.expectedInputSha256 = built.value->outputSha256;
    run.gameRoot = std::filesystem::absolute(request.build.gameRoot); run.workspace = root / "prepared";
    run.handwrittenLuaEngine = request.engine; run.runtimeYdweRoot = request.ydweRoot;
    run.externalDependenciesDirectory = Services::ResolveCliExternalDependenciesDirectory(request.externalDependenciesDirectory);
    run.runtimeBridge = std::filesystem::absolute(request.helpers) / "audastra-warcraft-launcher.exe";
    run.runtimeBootstrap = std::filesystem::absolute(request.helpers) / "audastra-warcraft-bootstrap.dll";
    auto prepared = Services::PrepareLuaAwareMapTest(run);
    if (!prepared.Succeeded()) return {std::nullopt, std::move(prepared.diagnostics)};
    auto refresh = (*frozen.value)->Refresh();
    if (!refresh.Succeeded()) return {std::nullopt, std::move(refresh.diagnostics)};
    prepared.value->cloakedWindow = request.cloaked; prepared.value->hiddenDesktop = false; prepared.value->backgroundPlay = request.cloaked;
    auto launched = Script::LaunchMapTest(*prepared.value);
    if (!launched.Succeeded()) return {std::nullopt, std::move(launched.diagnostics)};
    auto session = std::make_shared<TestSession>(); session->build_ = std::move(*built.value);
    session->launch_ = std::move(*launched.value); session->workspace_ = std::move(*frozen.value);
    auto result = Output::Success(std::move(session));
    result.diagnostics = std::move(prepared.diagnostics);
    result.diagnostics.insert(result.diagnostics.end(), launched.diagnostics.begin(), launched.diagnostics.end());
    result.diagnostics.insert(result.diagnostics.end(),permitted.diagnostics.begin(),permitted.diagnostics.end());
    return result;
}
} // namespace War3Map::HLua
