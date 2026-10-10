#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace War3Map::ObjectData {

enum class ReferenceSource : std::uint8_t {
    Trigger,
    Jass,
    PreplacedObject,
    Shop,
    Summon,
    ObjectData,
};

using ReferenceSourceMask = std::uint32_t;

[[nodiscard]] constexpr ReferenceSourceMask ReferenceSourceBit(
    ReferenceSource source) noexcept {
    return ReferenceSourceMask{1} << static_cast<std::uint8_t>(source);
}

[[nodiscard]] constexpr ReferenceSourceMask RequiredReferenceSources() noexcept {
    return ReferenceSourceBit(ReferenceSource::Trigger)
        | ReferenceSourceBit(ReferenceSource::Jass)
        | ReferenceSourceBit(ReferenceSource::PreplacedObject)
        | ReferenceSourceBit(ReferenceSource::Shop)
        | ReferenceSourceBit(ReferenceSource::Summon)
        | ReferenceSourceBit(ReferenceSource::ObjectData);
}

struct ReferenceEvidence {
    ReferenceSource source = ReferenceSource::ObjectData;
    std::string location;
    std::string summary;
    bool blocksDeletion = true;
};

struct ReferenceAuditReport {
    ObjectKey subject;
    std::string documentFingerprint;
    ReferenceSourceMask inspectedSources = 0;
    std::vector<ReferenceEvidence> references;
};

class DeletionAuditToken {
public:
    DeletionAuditToken(const DeletionAuditToken&) = default;
    DeletionAuditToken& operator=(const DeletionAuditToken&) = default;

private:
    friend class ReferenceAuditAuthority;
    friend class LniObjectDocument;

    DeletionAuditToken(
        ObjectKey subject,
        std::string documentFingerprint,
        std::uint64_t nonce);

    ObjectKey subject_;
    std::string documentFingerprint_;
    std::uint64_t nonce_ = 0;
};

class ReferenceAuditAuthority {
public:
    [[nodiscard]] static Result<DeletionAuditToken> IssueDeletionToken(
        const ReferenceAuditReport& report,
        bool explicitUserApproval);
};

} // namespace War3Map::ObjectData
