#include "material_editor.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>

namespace zscan::app {

MaterialEditor::MaterialEditor(schema::MaterialKind kind, QWidget* parent)
    : QDialog(parent)
    , kind_(kind)
{
    setWindowTitle(kind == schema::MaterialKind::Solvent ? "Add solvent type" : "Add entrance window type");
    auto* form = new QFormLayout(this);
    id_ = new QLineEdit(this);
    name_ = new QLineEdit(this);
    index_ = new QLineEdit(this);
    uncertainty_ = new QLineEdit(this);
    provenance_ = new QLineEdit(this);
    form->addRow("ID", id_);
    form->addRow("Name", name_);
    form->addRow("Refractive index", index_);
    form->addRow("Relative uncertainty", uncertainty_);
    form->addRow("Provenance ID", provenance_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        bool index_ok = false;
        bool uncertainty_ok = false;
        const auto index = index_->text().toDouble(&index_ok);
        const auto uncertainty = uncertainty_->text().toDouble(&uncertainty_ok);
        if (!id_->text().trimmed().isEmpty() && !name_->text().trimmed().isEmpty()
            && index_ok && index > 0 && uncertainty_ok && uncertainty > 0
            && !provenance_->text().trimmed().isEmpty()) {
            accepted_ = true;
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

std::optional<schema::MaterialPreset> MaterialEditor::result() const
{
    if (!accepted_) return std::nullopt;
    bool index_ok = false;
    bool uncertainty_ok = false;
    const auto index = index_->text().toDouble(&index_ok);
    const auto uncertainty = uncertainty_->text().toDouble(&uncertainty_ok);
    if (!index_ok || !uncertainty_ok) return std::nullopt;
    return schema::MaterialPreset { id_->text().trimmed().toStdString(),
        name_->text().trimmed().toStdString(), kind_, index, uncertainty, "user",
        provenance_->text().trimmed().toStdString() };
}

} // namespace zscan::app
