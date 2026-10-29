#pragma once
#include <string>
#include <string_view>
namespace War3Map::Trigger {
struct StorageNatives {
    std::string load, save, have, remove;
    bool known = true;
};
// 参数检查和发射器共用同一存取表；未知类型保留宏形式，不能猜测支持。
[[nodiscard]] StorageNatives StorageFor(std::string_view type, bool nullSafeHandleSet = false);
}
