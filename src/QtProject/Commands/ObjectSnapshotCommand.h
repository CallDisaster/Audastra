#pragma once

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include <QUndoCommand>
#include <functional>

namespace War3Map::QtProject {
// 物编表与修改面板共用原有快照撤销命令；业务逆操作仍由 Core 校验。
class ObjectSnapshotCommand final : public QUndoCommand {
public:
    ObjectSnapshotCommand(ObjectData::LniObjectDocument before, ObjectData::LniObjectDocument after,
        std::function<void(const ObjectData::LniObjectDocument&)> apply, const QString& label)
        : QUndoCommand(label), before_(std::move(before)), after_(std::move(after)), apply_(std::move(apply)) {}
    void undo() override { apply_(before_); }
    void redo() override { apply_(after_); }
private:
    ObjectData::LniObjectDocument before_, after_;
    std::function<void(const ObjectData::LniObjectDocument&)> apply_;
};
} // namespace War3Map::QtProject
