#include "Fixtures.h"

#include <iostream>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace FeedbackFixtures;

int main() {
    try {
        const auto context = Context();
        const auto input = std::string(R"(game=C:\Games\SyntheticWarcraft\war3.exe
ydwe=D:\工具\SyntheticYDWE\bin\loader.dll
user=C:\Users\Synthetic User\私人 资料\notes.txt
other=E:\未登记\用户隐私\details.log
unicodefolder=C:\u1234\private-unicode-folder\details.log
punctuation=C:\Users\O'Connor,private;name\private-punctuation-file.log
network=\\private-server\私人 共享\目录\file.log
posix=/home/private-person/internal/data.log
map="原创测试 地图.w3x"
fullmap=E:\Map Store\原创测试 地图.w3x
apiKey="short-secret"
credentialRef={"provider":"private-vault-name","id":"internal-ref-identifier"}
Authorization: Bearer original-bearer-fixture
link=https://user:private-password@example.invalid/private-token/endpoint?access_token=private-query#private-fragment
tokenlike sk-original-test-abcdefghijklmno
opaque abcdefghijklmnopqrstuvwxyz0123456789ABCDEF
known=original-secret-value-123
mapContent={"units":"private-map-unit-content"}
triggerContent="private-trigger-source-text"
conversation="private-original-dialogue"
)");
        const auto cleaned = Take(CleanPrivateText(input, context), "clean mixed embedded original fixture");
        NoPrivate(cleaned.text, {"SyntheticWarcraft", "SyntheticYDWE", "Synthetic User", "用户隐私", "private-unicode-folder", "private-punctuation-file", "O'Connor", "private-server", "private-person", "原创测试 地图.w3x",
            "short-secret", "private-vault-name", "internal-ref-identifier", "original-bearer-fixture", "private-password", "private-query", "private-fragment",
            "private-token", "sk-original-test", "abcdefghijklmnopqrstuvwxyz0123456789ABCDEF", "original-secret-value-123", "private-map-unit-content",
            "private-trigger-source-text", "private-original-dialogue"});
        Require(cleaned.text.find("<游戏目录>") != std::string::npos, "classify known game root");
        Require(cleaned.text.find("<YDWE目录>") != std::string::npos, "classify Chinese ydwe root");
        Require(cleaned.text.find("<用户目录>") != std::string::npos, "classify user root with spaces");
        Require(cleaned.text.find("<网络路径>") != std::string::npos, "clean unknown UNC root");
        Require(cleaned.text.find("<凭据>") != std::string::npos, "clean structured credential reference");
        const auto mapHash = Take(MapNameHash8("原创测试 地图.w3x"), "map name hash");
        Require(cleaned.text.find("<地图:" + mapHash + ">") != std::string::npos, "map filename replaced by eight hex digits");
        const auto adjacentFields = Take(CleanPrivateText(R"(at C:\Users\Synthetic User\private.txt apiKey="adjacent-camel-key"
at C:\Users\Synthetic User\private.txt api\u005fkey="adjacent-encoded-key"
at C:\Users\Synthetic User\private.txt credentialRef="adjacent-short-reference"
at C:\Users\Synthetic User\private.txt Authorization: Bearer adjacent-short-bearer
)", context), "path boundary retains sensitive field labels for cleaning");
        NoPrivate(adjacentFields.text, {"Synthetic User", "private.txt", "adjacent-camel-key", "adjacent-encoded-key", "adjacent-short-reference", "adjacent-short-bearer"});
        Require(Take(CleanPrivateText(adjacentFields.text, context), "adjacent field cleaning idempotence").text == adjacentFields.text,
            "sensitive field boundary is idempotent");
        const auto twice = Take(CleanPrivateText(cleaned.text, context), "clean sanitized value again");
        Require(twice.text == cleaned.text, "privacy transformation is idempotent");

        const auto encoded = Take(CleanPrivateText(R"(path=C%3A%5CUsers%5CEncoded%20Person%5Cprivate.txt
api\u005fkey="escaped-short-key"
url=https%3A%2F%2Fuser%3Apassword%40example.invalid%2Fv1%3Ftoken%3Dquery-token%23fragment-token
encodedspace=https://example.invalid/path?token=first%20private%20second,third;fourth#private-fragment
afterurl=https://example.invalid/endpoint credentialRef="after-url-private-ref"
credentialRef="vault://internal-reference"
--api-key command-short-key
Bearer standalone-short-bearer
credentialRef standalone-short-reference
base64token AbCdEf0123456789AbCdEf/XyZ0123456789XyZ012345
data:text/plain;base64,cHJpdmF0ZQ==
)", context), "decode encoded sensitive spellings");
        NoPrivate(encoded.text, {"Encoded", "Person", "escaped-short-key", "password", "query-token", "fragment-token", "internal-reference", "command-short-key", "cHJpdmF0ZQ", "second", "third", "fourth", "after-url-private-ref", "standalone-short-bearer", "standalone-short-reference", "AbCdEf0123456789AbCdEf"});
        Require(!CleanPrivateText(std::string("\xc0\x80", 2), context).Succeeded(), "invalid UTF8 rejected");
        Require(!CleanPrivateText("%252525252541", context).Succeeded(), "excess encoded depth rejected");
        Require(!CleanPrivateText(std::string(33, 'x'), {}, PrivacyLimits{32, 128, 4096}).Succeeded(), "bounded raw text rejected before partial persistence");
        const std::string commitHash = "0123456789abcdef0123456789abcdef01234567";
        const std::string shaHash = "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
        const auto hashes = Take(CleanPrivateText("commit: " + commitHash + ", sha256: " + shaHash, {}), "preserve diagnostic hashes");
        Require(hashes.text == "commit: " + commitHash + ", sha256: " + shaHash && hashes.replacements == 0,
            "40 and 64 digit mixed-case hexadecimal identities are not credentials");
        Require(Take(CleanPrivateText(hashes.text, {}), "hash cleaning idempotence").text == hashes.text,
            "diagnostic hash exemption is idempotent");
        const auto diagnosticForms = "commit: " + commitHash + ". SHA256=" + shaHash + " sha1=" + commitHash + " commit=" + commitHash;
        Require(Take(CleanPrivateText(diagnosticForms, {}), "diagnostic hash boundaries").text == diagnosticForms,
            "sentence punctuation and explicit diagnostic assignments preserve exact hashes");
        Require(Take(CleanPrivateText("sha256%3D" + shaHash, {}), "encoded diagnostic hash").text == "sha256=" + shaHash,
            "encoded diagnostic assignment is normalized before hash classification");
        Require(Take(CleanPrivateText("opaque=" + shaHash, {}), "unknown long assignment").text == "<密钥>",
            "unknown assignments are not added to the hash exemption");
        Require(Take(CleanPrivateText(commitHash + "." + std::string(32, 'g'), {}), "compound private token").text == "<密钥>",
            "compound credentials are not split into an exempt hash");
        for (const auto length : {39U, 41U, 63U, 65U})
            Require(Take(CleanPrivateText(std::string(length, 'a'), {}), "other long hex tokens").text == "<密钥>",
                "only the exact hash lengths are exempt");
        Require(Take(CleanPrivateText(std::string(39, 'a') + "g", {}), "nonhex 40 digit token").text == "<密钥>",
            "a nonhex token of hash length remains private");
        const auto hexCredentials = Take(CleanPrivateText("apiKey=" + commitHash + "\nAuthorization: Bearer " + shaHash, {}), "hex credentials");
        Require(hexCredentials.text.find(commitHash) == std::string::npos && hexCredentials.text.find(shaHash) == std::string::npos,
            "a hash-shaped value in a credential field is still removed");
        Require(Take(CleanPrivateText("apiKey%3D" + commitHash, {}), "encoded hash-shaped credential").text.find(commitHash) == std::string::npos,
            "encoded credential fields still win over the hash exemption");
        auto hashSecretContext = context;
        hashSecretContext.secretValues.push_back(shaHash);
        Require(Take(CleanPrivateText("known: " + shaHash, hashSecretContext), "known hash-shaped secret").text.find(shaHash) == std::string::npos,
            "known credential values have priority over diagnostic hashes");
        auto invalidContext = context;
        invalidContext.secretValues.push_back("a");
        Require(!ValidatePrivacyContext(invalidContext).Succeeded(), "unbounded ambiguous context rejected");

        const auto a = Take(ProblemFingerprint("a", "bc", "1.2.3"), "problem fingerprint one");
        const auto b = Take(ProblemFingerprint("ab", "c", "1.2.3"), "problem fingerprint two");
        const auto changedVersion = Take(ProblemFingerprint("a", "bc", "1.2.4"), "problem fingerprint version");
        Require(a.size() == 64 && a != b && a != changedVersion, "fingerprint binds lengths and version");

        auto draft = UserDraft();
        draft.includeLogs = false;
        draft.recentLogs = "excluded private recent logs";
        draft.includeConversation = false;
        draft.conversationSnippet = "excluded private conversation transcript";
        draft.includeScreenshot = false;
        draft.screenshot.png = Png(true);
        const auto defaultReport = Take(CleanDraft(draft, context), "clean user draft default exclusions");
        Require(!defaultReport.logs && !defaultReport.conversationSnippet && !defaultReport.screenshot, "logs conversation screenshot default excluded");
        Require(ValidateCleanReport(defaultReport, context).Succeeded(), "clean report validates before persistence");
        auto unclean = defaultReport;
        unclean.userDescription = R"(private file C:\Users\Synthetic User\secret.txt)";
        Require(!ValidateCleanReport(unclean, context).Succeeded(), "forged unclean report rejected");
        auto agent = AgentDraft();
        agent.agentIssue = AgentIssue::UserMapError;
        Require(!CleanDraft(agent, context).Succeeded(), "agent cannot report user map errors");
        for (const auto gap : {AgentIssue::ToolUsabilityGap, AgentIssue::CompatibilityGap}) {
            agent.agentIssue = gap;
            const auto report = CleanDraft(agent, context);
            Require(report.Succeeded() && report.value->agentIssue == gap, "agent may report tool usability and compatibility gaps");
            auto withoutCode = agent;
            withoutCode.errorCode.clear();
            Require(!CleanDraft(withoutCode, context).Succeeded(), "gap reports still need an error code");
        }
        Require(AgentIssueName(AgentIssue::ToolUsabilityGap) == "tool-usability-gap"
            && AgentIssueName(AgentIssue::CompatibilityGap) == "compatibility-gap", "gap names are stable");
        agent.agentIssue = AgentIssue::InternalToolError;
        agent.includeConversation = true;
        agent.conversationSnippet = "no automatic conversation";
        Require(!CleanDraft(agent, context).Succeeded(), "agent cannot add original conversation automatically");
        agent.includeConversation = false;
        agent.includeScreenshot = true;
        agent.screenshot.png = Png();
        Require(!CleanDraft(agent, context).Succeeded(), "agent cannot add screenshot automatically");

        const auto png = Png(true);
        const auto prepared = Take(PrepareScreenshot(png), "prepare user screenshot");
        Require(prepared.png.size() < png.size(), "strip private ancillary PNG block");
        NoPrivate(std::string_view(reinterpret_cast<const char*>(prepared.png.data()), prepared.png.size()), {"original-secret-value-123", "Synthetic User"});
        Require(Take(PrepareScreenshot(prepared.png), "screenshot canonical second pass") == prepared, "prepared PNG is stable");
        draft.includeScreenshot = true;
        draft.screenshot.png = png;
        draft.screenshot.userReviewedSha256 = Take(Sha256(png), "raw screenshot hash");
        Require(!CleanDraft(draft, context).Succeeded(), "original PNG hash does not confirm processed attachment");
        draft.screenshot.userReviewedSha256 = prepared.sha256;
        const auto screenshotReport = Take(CleanDraft(draft, context), "explicit reviewed final screenshot");
        Require(screenshotReport.screenshot == prepared, "queue receives only prepared screenshot");
        auto badPng = prepared.png;
        badPng.back() ^= std::byte{1};
        Require(!PrepareScreenshot(badPng).Succeeded(), "PNG invalid CRC rejected");
        badPng = prepared.png;
        badPng.push_back(std::byte{'x'});
        Require(!PrepareScreenshot(badPng).Succeeded(), "PNG polyglot tail rejected");
        Require(!PrepareScreenshot(prepared.png, 8).Succeeded(), "PNG attachment size bounded");
        std::cout << "feedback privacy checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "feedback privacy failure: " << error.what() << '\n';
        return 1;
    }
}
