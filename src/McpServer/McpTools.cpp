#include "McpTools.h"
#include "McpToolInternal.h"
#include "McpObjectTools.h"
#include "McpObjectResourceTools.h"
#include "McpVariableTools.h"
#include "McpMapDataTools.h"
#include "McpPlacementTools.h"
#include "McpAuxiliaryTools.h"
#include "McpProductTools.h"
#include "McpCodeCheckTools.h"
#include "McpTerrainEditTools.h"
#include "McpLuaTestTools.h"
#include "McpLuaFileTools.h"
#include "McpWorldViewportTools.h"

namespace War3Map::Mcp {
void RegisterModelAnimationTool(McpServer& server);
void RegisterCampaignTools(McpServer& server);

void RegisterAllTools(McpServer& server) {
    RegisterProductTools(server);
    RegisterCodeCheckTool(server);
    RegisterLuaTestTools(server);
    RegisterLuaFileTools(server);
    RegisterWorldViewportTools(server);
    RegisterReadOnlyTools(server);
    RegisterArchiveIdentityTools(server);
    RegisterArchiveMemberTools(server);
    RegisterImportTools(server);
    RegisterObjectResourceTools(server);
    RegisterProjectSearchTools(server);
    RegisterTerrainTools(server);
    RegisterTerrainEditTools(server);
    RegisterModelAnimationTool(server);
    RegisterCampaignTools(server);
    RegisterMapEditSaveTool(server);
    RegisterMapCreateTool(server);
    RegisterRevisionTools(server);
    RegisterWarVKTool(server);
    RegisterEditTools(server);
    RegisterTriggerGraphTools(server);
    RegisterObjectTools(server);
    RegisterVariableTools(server);
    RegisterMapDataTools(server);
    RegisterPlacementTools(server);
    RegisterAuxiliaryTools(server);
}

} // namespace War3Map::Mcp
