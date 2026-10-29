#pragma once

#include "zscan/schema/materials.hpp"

#include <QDialog>
#include <optional>

class QLineEdit;

namespace zscan::app {

class MaterialEditor final : public QDialog {
    Q_OBJECT
public:
    explicit MaterialEditor(schema::MaterialKind kind, QWidget* parent = nullptr);
    [[nodiscard]] std::optional<schema::MaterialPreset> result() const;

private:
    schema::MaterialKind kind_;
    QLineEdit* id_ { };
    QLineEdit* name_ { };
    QLineEdit* index_ { };
    QLineEdit* uncertainty_ { };
    QLineEdit* provenance_ { };
    bool accepted_ { false };
};

} // namespace zscan::app
