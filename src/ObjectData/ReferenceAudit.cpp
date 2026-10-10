#include "War3MapCore/ObjectData/ReferenceAudit.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace War3Map::ObjectData {
namespace {

std::atomic<std::uint64_t> g_nextDeletionNonce{1};

} // namespace

DeletionAuditToken::DeletionAuditToken(
    ObjectKey subject,
    std::string documentFingerprint,
    std::uint64_t nonce)
    : subject_(std::move(subject)),
      documentFingerprint_(std::move(documentFingerprint)),
      nonce_(nonce) {
}

Result<DeletionAuditToken> ReferenceAuditAuthority::IssueDeletionToken(
    const ReferenceAuditReport& report,
    bool explicitUserApproval) {
    if (!explicitUserApproval) {
        return Result<DeletionAuditToken>::Failure(
            "ObjectData",
            "object-delete-approval-required",
            "对象删除必须获得显式用户批准。");
    }
    if (report.subject.rawcode.empty() || report.documentFingerprint.empty()) {
        return Result<DeletionAuditToken>::Failure(
            "ObjectData",
            "object-delete-audit-identity-missing",
            "引用审计必须绑定对象和当前文档指纹。");
    }
    const ReferenceSourceMask required = RequiredReferenceSources();
    if ((report.inspectedSources & required) != required) {
        return Result<DeletionAuditToken>::Failure(
            "ObjectData",
            "object-delete-audit-incomplete",
            "引用审计尚未覆盖 WTG、JASS、预放置对象、商店、召唤和物编引用。");
    }
    const auto blocking = std::find_if(
        report.references.begin(),
        report.references.end(),
        [](const ReferenceEvidence& evidence) {
            return evidence.blocksDeletion;
        });
    if (blocking != report.references.end()) {
        return Result<DeletionAuditToken>::Failure(
            "ObjectData",
            "object-delete-reference-blocked",
            "对象仍存在阻断删除的引用：" + blocking->summary);
    }

    return Result<DeletionAuditToken>::Success(DeletionAuditToken{
        report.subject,
        report.documentFingerprint,
        g_nextDeletionNonce.fetch_add(1, std::memory_order_relaxed),
    });
}

} // namespace War3Map::ObjectData
