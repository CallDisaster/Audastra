#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"

namespace War3Map::Services {

inline constexpr char kMinimapMarkerProvenance[] = "audastra\\minimap-markers.bin";

enum class MinimapMarkerOrigin { Legacy, Generated, Author };
enum class MinimapMarkerOperation { Add, Remove, Move };

struct MinimapMarkerEdit {
    MinimapMarkerOperation operation;
    // Zero-based row in the exact revision returned by Read.
    std::size_t row = 0;
    MapData::MinimapMarkerRecord value;
};

struct MinimapMarkerView {
    std::string revision;
    bool legacy = false;
    std::vector<MapData::MinimapMarkerRecord> records;
    std::vector<MinimapMarkerOrigin> origins;
};

struct MinimapMarkerProjection {
    std::vector<std::byte> markers;
    std::vector<std::byte> provenance;
    std::size_t count = 0;
};

// Native MMP remains a plain v0 member. The private sidecar binds its complete
// SHA, the generated prefix, and explicit suppressions of generated rows.
// Legacy rows have no trustworthy owner; they are never classified by value.
class MinimapMarkerEdits {
public:
    [[nodiscard]] static Result<MinimapMarkerEdits> Read(const MapArchiveSession& archive,
        std::span<const std::byte> pending = {});
    [[nodiscard]] Result<MinimapMarkerView> View() const;
    [[nodiscard]] Result<std::vector<std::byte>> Apply(std::string_view revision,
        std::span<const MinimapMarkerEdit> edits) const;
    [[nodiscard]] Result<MinimapMarkerProjection> Project(std::span<const std::byte> generated) const;
    [[nodiscard]] bool UsesGenerator() const noexcept { return managed_; }
    [[nodiscard]] bool ShouldWrite() const noexcept { return managed_ || pending_; }
    [[nodiscard]] bool HasPending() const noexcept { return pending_; }
    [[nodiscard]] bool HasStoredProvenance() const noexcept { return stored_; }

private:
    bool managed_ = false, pending_ = false, stored_ = false;
    std::string sourceIdentity_, archiveIdentity_;
    std::vector<MapData::MinimapMarkerRecord> visibleGenerated_, author_, suppressed_;
};

} // namespace War3Map::Services
