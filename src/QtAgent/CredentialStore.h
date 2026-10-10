#pragma once

#include <QString>

namespace War3Map::Agent {

// CurrentUser DPAPI：普通设置可迁移，密钥只属于当前 Windows 用户。
// 可传入独立目录做测试，不访问用户已有凭据。
class CredentialStore final {
public:
    explicit CredentialStore(QString directory = {});

    [[nodiscard]] QString Read(const QString& reference, QString* error = nullptr) const;
    [[nodiscard]] bool Write(const QString& reference, const QString& secret, QString* error = nullptr) const;
    [[nodiscard]] bool Remove(const QString& reference, QString* error = nullptr) const;
    [[nodiscard]] bool Exists(const QString& reference) const;
    [[nodiscard]] QString Directory() const;

private:
    [[nodiscard]] QString Path(const QString& reference) const;
    QString directory_;
};

} // namespace War3Map::Agent
