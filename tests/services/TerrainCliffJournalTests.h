#pragma once

namespace War3Map::TerrainSaveTests {
// 由Services真实捕获完整DOO owner，覆盖悬崖/已有坡道水的journal与可选摘要预算。
// 由既有TerrainWaterEditServiceTests入口调用；不构成原版编辑器或游戏验收。
void CheckTerrainCliffJournalContracts();
}
