#pragma once

#include <QMainWindow>
#include <optional>

#include "zscan/schema/fit_adapter.hpp"
#include "zscan/schema/materials.hpp"
#include "zscan/schema/types.hpp"
#include "zscan/schema/workup_adapter.hpp"

class QListWidget;
class QPlainTextEdit;
class QLabel;
class QFormLayout;
class QTableWidget;
class QComboBox;
class QCheckBox;
class QPushButton;
class QQuickWidget;
namespace zscan::app {

class CurvePlot;

class SessionWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SessionWindow(QWidget* parent = nullptr);

private slots:
    void openSession();
    void editSession();
    void saveSession();
    void saveSessionAs();
    void editSnapshots();
    void runFit();

private:
    void showReport(const QString& root, bool valid, QStringList errors, QStringList warnings);
    void showCurveDetails(int row);

    QListWidget* curves_ { };
    QPlainTextEdit* diagnostics_ { };
    QLabel* status_ { };
    QPlainTextEdit* details_ { };
    QTableWidget* workup_table_ { };
    CurvePlot* curve_plot_ { };
    QTableWidget* workup_summary_table_ { };
    QTableWidget* fit_table_ { };
    QTableWidget* fit_setup_table_ { };
    QTableWidget* fit_contributions_ { };
    CurvePlot* fit_plot_ { };
    QLabel* fit_detail_diagnostics_ { };
    std::optional<zfit::FitResult> fit_result_;
    std::optional<zfit::FitRequest> fit_request_;
    QPushButton* fit_run_button_ { };
    QLabel* fit_status_ { };
    std::vector<QCheckBox*> fit_parameter_checks_;
    QComboBox* workup_curve_combo_ { };
    QLabel* workup_summary_ { };
    QStringList curve_details_;
    std::optional<zscan::schema::SessionManifest> manifest_;
    std::optional<zscan::schema::InstrumentSnapshot> instrument_;
    std::optional<zscan::schema::SampleSnapshot> sample_;
    std::optional<zscan::schema::MaterialCatalog> materials_;
    std::optional<zscan::schema::PreparedWorkup> workup_;
    QString session_root_;
    bool dirty_ { false };
    void showWorkupCurve(int index);
};

} // namespace zscan::app
