#include "TerrainResizeReplayCases.h"

#include <exception>
#include <iostream>

int main() {
    try {
        using namespace War3Map::TerrainResizeReplayTests;
        ResizeReplayRestoresTypedAuthorityAndRejectsTampering();
        ResizeReplayUsesCurrentShapeAndAggregateBudgets();
        ResizeHistoryKeepsOldShadowAcrossPhysicalSaveStation();
        ResizeReplayComposesWithCurrentWaterCliffPaletteBoundsAndOrdinaryBrushes();
        std::cout << "Terrain resize replay and saved history passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
