#pragma once
#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/Services/MapTestWorkspace.h"
#include "War3MapCore/Services/LuaTestMailboxSession.h"

namespace War3Map::HLua {
struct TestRequest {
    BuildRequest build;
    std::filesystem::path helpers, ydweRoot;
    Services::MapTestLuaEngine engine = Services::MapTestLuaEngine::Owned;
    bool cloaked = true;
    std::filesystem::path externalDependenciesDirectory;
};
class TestSession final {
public:
    ~TestSession();
    [[nodiscard]] const BuildReceipt& Build() const noexcept { return build_; }
    [[nodiscard]] const Script::MapTestLaunchReceipt& Launch() const noexcept { return launch_; }
    [[nodiscard]] Result<Services::LuaTestReloadSnapshot> QueryLua() const;
    [[nodiscard]] Status Stop();
    [[nodiscard]] Status Cleanup();
private:
    BuildReceipt build_;
    Script::MapTestLaunchReceipt launch_;
    std::shared_ptr<Services::MapTestWorkspaceLease> workspace_;
    friend Result<std::shared_ptr<TestSession>> StartProjectTest(const TestRequest&);
};
[[nodiscard]] Result<std::shared_ptr<TestSession>> StartProjectTest(const TestRequest& request);
[[nodiscard]] Status ValidateTestEngineSources(const std::filesystem::path& map, std::string_view expectedSha256,
    Services::MapTestLuaEngine engine);
} // namespace War3Map::HLua
