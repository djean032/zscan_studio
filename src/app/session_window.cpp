#include "session_window.hpp"
#include "acquisition_wizard.hpp"
#include "curve_plot.hpp"

#include "zscan/schema/storage.hpp"
#include "zscan/schema/toml_io.hpp"
#include "zscan/schema/validation_report.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>

namespace zscan::app {
namespace {
    QString display_value(double value)
    {
        const double magnitude = std::abs(value);
        if (magnitude != 0.0 && (magnitude < 0.01 || magnitude >= 1e6))
            return QString::number(value, 'e', 2);
        return QString::number(std::trunc(value * 100.0) / 100.0, 'f', 2);
    }

    struct SessionLoad {
        zscan::schema::ValidationReport report;
        std::optional<zscan::schema::SessionManifest> manifest;
        std::optional<zscan::schema::InstrumentSnapshot> instrument;
        std::optional<zscan::schema::SampleSnapshot> sample;
        std::optional<zscan::schema::PreparedWorkup> workup;
    };
}

SessionWindow::SessionWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ZScan Studio"));
    resize(1100, 700);
    auto* open = menuBar()->addAction(QStringLiteral("Open session…"));
    connect(open, &QAction::triggered, this, &SessionWindow::openSession);
    auto* edit = menuBar()->addAction(QStringLiteral("Edit session…"));
    connect(edit, &QAction::triggered, this, &SessionWindow::editSession);
    auto* save = menuBar()->addAction(QStringLiteral("Save revision"));
    connect(save, &QAction::triggered, this, &SessionWindow::saveSession);
    auto* save_as = menuBar()->addAction(QStringLiteral("Save as new session…"));
    connect(save_as, &QAction::triggered, this, &SessionWindow::saveSessionAs);

    auto* tabs = new QTabWidget(this);
    auto* session_page = new QWidget(tabs);
    auto* session_layout = new QVBoxLayout(session_page);
    auto* splitter = new QSplitter(session_page);
    curves_ = new QListWidget(splitter);
    auto* right = new QSplitter(Qt::Vertical, splitter);
    details_ = new QPlainTextEdit(right);
    details_->setReadOnly(true);
    diagnostics_ = new QPlainTextEdit(right);
    diagnostics_->setReadOnly(true);
    splitter->setStretchFactor(1, 1);
    connect(curves_, &QListWidget::currentRowChanged, this, &SessionWindow::showCurveDetails);
    session_layout->addWidget(splitter);
    tabs->addTab(session_page, QStringLiteral("Session"));

    auto* acquisition_page = new AcquisitionWizard(tabs);
    tabs->addTab(acquisition_page, QStringLiteral("Acquire"));

    auto* workup_page = new QWidget(tabs);
    auto* workup_layout = new QVBoxLayout(workup_page);
    workup_curve_combo_ = new QComboBox(workup_page);
    workup_summary_ = new QLabel(workup_page);
    workup_summary_table_ = new QTableWidget(workup_page);
    workup_summary_table_->setColumnCount(5);
    workup_summary_table_->setHorizontalHeaderLabels(
        { "Curve", "Energy mean ± SEM (J)", "Solvent", "Cuvette pair", "Entrance window n ± σ" });
    workup_layout->addWidget(workup_summary_table_);
    auto* point_heading = new QHBoxLayout;
    point_heading->addWidget(new QLabel(QStringLiteral("Selected curve data"), workup_page));
    point_heading->addStretch();
    point_heading->addWidget(new QLabel(QStringLiteral("Curve:"), workup_page));
    point_heading->addWidget(workup_curve_combo_);
    workup_layout->addLayout(point_heading);
    workup_table_ = new QTableWidget(workup_page);
    workup_table_->setColumnCount(4);
    workup_table_->setHorizontalHeaderLabels({ "z (m)", "Transmission", "SE", "Dark sensitivity" });
    workup_layout->addWidget(workup_table_);
    curve_plot_ = new CurvePlot(workup_page);
    workup_layout->addWidget(curve_plot_);
    connect(workup_curve_combo_, &QComboBox::currentIndexChanged, this,
        &SessionWindow::showWorkupCurve);
    tabs->addTab(workup_page, QStringLiteral("Workup"));
    auto* fit_page = new QWidget(tabs);
    auto* fit_layout = new QVBoxLayout(fit_page);
    fit_setup_table_ = new QTableWidget(fit_page);
    fit_setup_table_->setColumnCount(6);
    fit_setup_table_->setHorizontalHeaderLabels(
        { "Parameter", "Initial/fixed value", "Fit?", "Prior σ", "Fitted value", "± total SE" });
    const char* names[]
        = { "sigma_g", "sigma_s", "sigma_t", "tau10", "tau21", "tau13", "tau30", "tau43" };
    const double defaults[] = { 4.9e-18, 1.6e-17, 1.5e-17, 1e-12, 1e-12, 1e-12, 1.29e-7, 1e-12 };
    fit_setup_table_->setRowCount(8);
    fit_setup_table_->verticalHeader()->setDefaultSectionSize(22);
    fit_setup_table_->setMaximumHeight(230);
    for (int row = 0; row < 8; ++row) {
        fit_setup_table_->setItem(row, 0, new QTableWidgetItem(QString::fromLatin1(names[row])));
        fit_setup_table_->setItem(row, 1, new QTableWidgetItem(display_value(defaults[row])));
        auto* check = new QCheckBox(fit_setup_table_);
        check->setChecked(row == 0);
        fit_setup_table_->setCellWidget(row, 2, check);
        fit_setup_table_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("0")));
        fit_setup_table_->setItem(row, 4, new QTableWidgetItem(QStringLiteral("—")));
        fit_setup_table_->setItem(row, 5, new QTableWidgetItem(QStringLiteral("—")));
    }
    fit_setup_table_->resizeColumnsToContents();
    fit_layout->addWidget(fit_setup_table_);
    fit_run_button_ = new QPushButton(QStringLiteral("Run fit"), fit_page);
    fit_layout->addWidget(fit_run_button_);
    fit_status_ = new QLabel(QStringLiteral("Preview fit: reduced solver settings"), fit_page);
    fit_layout->addWidget(fit_status_);
    auto* fit_bottom = new QTabWidget(fit_page);
    auto* details_page = new QWidget(fit_bottom);
    auto* details_layout = new QVBoxLayout(details_page);
    fit_detail_diagnostics_ = new QLabel(details_page);
    fit_detail_diagnostics_->setWordWrap(true);
    details_layout->addWidget(fit_detail_diagnostics_);
    details_layout->addWidget(
        new QLabel(QStringLiteral("Calibration variance contributions (parameter variance units²)"),
            details_page));
    fit_contributions_ = new QTableWidget(details_page);
    fit_contributions_->setColumnCount(9);
    fit_contributions_->setHorizontalHeaderLabels({ "Parameter", "Energy SEM variance",
        "Meter variance", "κ variance", "Waist variance", "M² variance", "Concentration variance",
        "Path-length variance", "Fixed-prior variance" });
    fit_contributions_->resizeColumnsToContents();
    details_layout->addWidget(fit_contributions_);
    fit_bottom->addTab(details_page, QStringLiteral("Fit details"));
    auto* plot_page = new QWidget(fit_bottom);
    auto* plot_layout = new QVBoxLayout(plot_page);
    fit_plot_ = new CurvePlot(plot_page);
    plot_layout->addWidget(fit_plot_);
    fit_bottom->addTab(plot_page, QStringLiteral("Plot"));
    fit_layout->addWidget(fit_bottom);
    connect(fit_run_button_, &QPushButton::clicked, this, &SessionWindow::runFit);
    tabs->addTab(fit_page, QStringLiteral("Fit"));
    tabs->setTabEnabled(3, false);
    tabs->setTabEnabled(2, false);
    setCentralWidget(tabs);
    status_ = new QLabel(QStringLiteral("No session loaded"), this);
    statusBar()->addPermanentWidget(status_);
}

void SessionWindow::openSession()
{
    const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("Open session"), { },
        QStringLiteral("Session manifest (session.toml)"));
    if (file.isEmpty())
        return;
    const auto root = QFileInfo(file).absolutePath();
    status_->setText(QStringLiteral("Loading…"));
    diagnostics_->clear();
    details_->clear();
    curves_->clear();
    auto* watcher = new QFutureWatcher<SessionLoad>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
        const auto loaded = watcher->result();
        const auto& report = loaded.report;
        QStringList errors;
        for (const auto& error : report.errors)
            errors << QString::fromStdString(error.location + ": " + error.message);
        QStringList warnings;
        for (const auto& warning : report.warnings)
            warnings << QString::fromStdString(warning.location + ": " + warning.message);
        showReport(root, report.is_valid(), errors, warnings);
        session_root_ = root;
        manifest_ = loaded.manifest;
        instrument_ = loaded.instrument;
        workup_ = loaded.workup;
        sample_ = loaded.sample;
        auto catalog = zscan::schema::load_material_catalog(
            QDir::currentPath().toStdString() + "/config/materials.toml");
        if (catalog)
            materials_ = std::move(*catalog);
        bool draft_material = false;
        if (materials_ && sample_ && instrument_) {
            for (const auto& material : materials_->materials) {
                const bool solvent_match = material.kind == zscan::schema::MaterialKind::Solvent
                    && material.material_id == sample_->solvent_id;
                const bool window_match
                    = material.kind == zscan::schema::MaterialKind::EntranceWindow
                    && std::abs(material.refractive_index
                           - instrument_->entrance_window_refractive_index.value)
                        < 1e-12;
                if ((solvent_match || window_match) && material.status == "draft")
                    draft_material = true;
            }
        }
        if (draft_material) {
            workup_.reset();
            diagnostics_->appendPlainText(QStringLiteral(
                "ERROR: selected material presets are draft; production Workup/Fit is disabled"));
            status_->setText(QStringLiteral("Draft calibration: Workup disabled"));
        }
        dirty_ = false;
        workup_table_->setRowCount(0);
        curve_plot_->setData({ }, { }, { });
        workup_summary_table_->setRowCount(0);
        workup_curve_combo_->clear();
        workup_summary_->clear();
        auto* tabs = qobject_cast<QTabWidget*>(centralWidget());
        if (workup_) {
            tabs->setTabEnabled(2, true);
            workup_summary_table_->setRowCount(static_cast<int>(workup_->curves.size()));
            for (std::size_t i = 0; i < workup_->curves.size(); ++i)
                workup_curve_combo_->addItem(QStringLiteral("Curve %1 · %2 J")
                                                 .arg(i + 1)
                                                 .arg(workup_->energies[i].mean_j, 0, 'g', 6),
                    static_cast<int>(i));
            for (int row = 0; row < workup_summary_table_->rowCount(); ++row) {
                const auto& energy = workup_->energies[static_cast<std::size_t>(row)];
                workup_summary_table_->setItem(
                    row, 0, new QTableWidgetItem(QString::number(row + 1)));
                workup_summary_table_->setItem(row, 1,
                    new QTableWidgetItem(QStringLiteral("%1 ± %2")
                            .arg(energy.mean_j, 0, 'g', 8)
                            .arg(energy.standard_error_j, 0, 'g', 8)));
                workup_summary_table_->setItem(row, 2,
                    new QTableWidgetItem(sample_
                            ? QStringLiteral("%1: n=%2 ± %3")
                                  .arg(QString::fromStdString(sample_->solvent_id))
                                  .arg(sample_->solvent_refractive_index.value, 0, 'g', 8)
                                  .arg(sample_->solvent_refractive_index.standard_uncertainty, 0,
                                      'g', 8)
                            : QStringLiteral("unknown")));
                const auto pair_id = row < static_cast<int>(manifest_->curves.size())
                    ? manifest_->curves[static_cast<std::size_t>(row)].cuvette_pair_calibration_id
                    : std::string { };
                workup_summary_table_->setItem(
                    row, 3, new QTableWidgetItem(QString::fromStdString(pair_id)));
                workup_summary_table_->setItem(row, 4,
                    new QTableWidgetItem(instrument_
                            ? QStringLiteral("n=%1 ± %2")
                                  .arg(instrument_->entrance_window_refractive_index.value, 0, 'g',
                                      8)
                                  .arg(instrument_->entrance_window_refractive_index
                                           .standard_uncertainty,
                                      0, 'g', 8)
                            : QStringLiteral("unknown")));
            }
            workup_summary_table_->resizeColumnsToContents();
            if (!workup_->curves.empty())
                showWorkupCurve(0);
        } else {
            tabs->setTabEnabled(2, false);
        }
        tabs->setTabEnabled(3, workup_.has_value());
        curve_details_.clear();
        if (loaded.manifest) {
            for (const auto& curve : loaded.manifest->curves) {
                curve_details_
                    << QStringLiteral("Curve %1\nSample acquisition: %2\nBlank acquisition: "
                                      "%3\nEnergy burst: %4\nCuvette pair: %5")
                           .arg(QString::fromStdString(curve.curve_id.to_string()))
                           .arg(QString::fromStdString(curve.sample_acquisition_id.to_string()))
                           .arg(QString::fromStdString(curve.blank_acquisition_id.to_string()))
                           .arg(QString::fromStdString(curve.energy_burst_id.to_string()))
                           .arg(QString::fromStdString(curve.cuvette_pair_calibration_id));
                QString energy_label = QStringLiteral("Energy");
                for (const auto& burst : loaded.manifest->energy_bursts)
                    if (burst.burst_id == curve.energy_burst_id)
                        energy_label = QStringLiteral("Energy %1 J").arg(burst.reading_count);
                curves_->addItem(QStringLiteral("Curve %1 · %2 · %3")
                        .arg(curves_->count() + 1)
                        .arg(energy_label)
                        .arg(QString::fromStdString(curve.cuvette_pair_calibration_id)));
            }
        }
        if (!curve_details_.isEmpty())
            curves_->setCurrentRow(0);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        SessionLoad result;
        result.report = zscan::schema::validate_session(root.toStdString());
        // The validator remains the production gate; curve details are loaded
        // through the same Qt-free TOML service when the manifest is readable.
        std::ifstream input(root.toStdString() + "/session.toml", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto parsed = zscan::schema::toml_io::parse_session(text);
        if (parsed)
            result.manifest = std::move(*parsed);
        if (result.manifest) {
            std::ifstream snapshot(
                root.toStdString() + "/" + result.manifest->instrument_snapshot.path.to_string(),
                std::ios::binary);
            std::string snapshot_text(
                (std::istreambuf_iterator<char>(snapshot)), std::istreambuf_iterator<char>());
            auto instrument = zscan::schema::toml_io::parse_instrument(snapshot_text);
            if (instrument)
                result.instrument = std::move(*instrument);
            std::ifstream sample_file(
                root.toStdString() + "/" + result.manifest->sample_snapshot.path.to_string(),
                std::ios::binary);
            std::string sample_text(
                (std::istreambuf_iterator<char>(sample_file)), std::istreambuf_iterator<char>());
            auto sample = zscan::schema::toml_io::parse_sample(sample_text);
            if (sample)
                result.sample = std::move(*sample);
        }
        if (result.report.is_valid()) {
            auto validated = zscan::schema::load_production_session(root.toStdString());
            if (validated) {
                auto prepared = zscan::schema::prepare_workup(*validated, { 0.05, 0.05 });
                if (prepared)
                    result.workup = std::move(*prepared);
            }
        }
        return result;
    }));
}

void SessionWindow::runFit()
{
    if (!workup_)
        return;
    auto request = zscan::schema::make_fit_request(*workup_);
    if (!request) {
        QMessageBox::critical(this, QStringLiteral("Fit request failed"),
            QString::fromStdString(request.error().message));
        return;
    }
    double* initial[]
        = { &request->initial_parameters.sigma_g, &request->initial_parameters.sigma_s,
              &request->initial_parameters.sigma_t, &request->initial_parameters.tau10,
              &request->initial_parameters.tau21, &request->initial_parameters.tau13,
              &request->initial_parameters.tau30, &request->initial_parameters.tau43 };
    // Keep the first interactive fit responsive. Final production fits will
    // expose and persist the full solver settings in the Fit tab.
    request->solver_options.nz = 16;
    request->solver_options.waveform_knots = 129;
    request->max_evaluations = 30;
    request->fitted_parameters.clear();
    for (int i = 0; i < 8; ++i) {
        bool ok = false;
        const double value = fit_setup_table_->item(i, 1)->text().toDouble(&ok);
        if (!ok || !std::isfinite(value) || value <= 0) {
            QMessageBox::warning(this, QStringLiteral("Invalid fit value"),
                QStringLiteral("Values must be positive finite numbers."));
            return;
        }
        *initial[i] = value;
        auto* check = qobject_cast<QCheckBox*>(fit_setup_table_->cellWidget(i, 2));
        if (check && check->isChecked())
            request->fitted_parameters.push_back(static_cast<zfit::FitParameter>(i));
        bool prior_ok = false;
        const double prior = fit_setup_table_->item(i, 3)->text().toDouble(&prior_ok);
        if (!prior_ok || !std::isfinite(prior) || prior < 0) {
            QMessageBox::warning(this, QStringLiteral("Invalid prior"),
                QStringLiteral("Prior σ must be nonnegative."));
            return;
        }
        if (!check || !check->isChecked())
            request->calibration.fixed_parameters.push_back(
                { static_cast<zfit::FitParameter>(i), prior });
    }
    if (request->fitted_parameters.empty()) {
        QMessageBox::warning(this, QStringLiteral("No parameters selected"),
            QStringLiteral("Select at least one fitted parameter."));
        return;
    }
    fit_request_ = *request;
    fit_run_button_->setEnabled(false);
    fit_status_->setText(QStringLiteral("Fitting preview…"));
    auto* watcher = new QFutureWatcher<zfit::FitResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto result = watcher->result();
        fit_result_ = result;
        QString diagnostics = QStringLiteral("Status: %1 · Converged: %2\nResidual norm: %3 · "
                                             "Initial norm: %4 · Max residual: %5\nEvaluations: %6")
                                  .arg(result.status)
                                  .arg(result.converged ? "yes" : "no")
                                  .arg(display_value(result.residual_norm))
                                  .arg(display_value(result.initial_residual_norm))
                                  .arg(display_value(result.max_abs_residual))
                                  .arg(result.function_evaluations);
        if (result.chi_squared)
            diagnostics += QStringLiteral(" · χ²: %1").arg(display_value(*result.chi_squared));
        if (result.uncertainty)
            diagnostics += QStringLiteral(
                "\nJacobian rank: %1 · Conditional and total SEs are shown above.")
                               .arg(result.uncertainty->jacobian_rank);
        fit_detail_diagnostics_->setText(diagnostics);
        const char* names[]
            = { "sigma_g", "sigma_s", "sigma_t", "tau10", "tau21", "tau13", "tau30", "tau43" };
        const double values[] = { result.parameters.sigma_g, result.parameters.sigma_s,
            result.parameters.sigma_t, result.parameters.tau10, result.parameters.tau21,
            result.parameters.tau13, result.parameters.tau30, result.parameters.tau43 };
        const int result_count
            = static_cast<int>(result.uncertainty ? result.uncertainty->parameter_order.size() : 0);
        fit_contributions_->setRowCount(result_count);
        for (int i = 0; i < result_count; ++i) {
            const auto parameter = result.uncertainty->parameter_order[static_cast<std::size_t>(i)];
            const auto index = static_cast<std::size_t>(parameter);
            fit_setup_table_->setItem(
                static_cast<int>(index), 4, new QTableWidgetItem(display_value(values[index])));
            const auto& uncertainty = *result.uncertainty;
            fit_setup_table_->setItem(static_cast<int>(index), 5,
                new QTableWidgetItem(uncertainty.total_standard_errors.empty()
                        ? QStringLiteral("—")
                        : display_value(
                              uncertainty.total_standard_errors[static_cast<std::size_t>(i)])));
            fit_contributions_->setItem(
                i, 0, new QTableWidgetItem(QString::fromLatin1(names[index])));
            const auto& groups
                = uncertainty.calibration_variance_contributions[static_cast<std::size_t>(i)];
            for (int group = 0; group < 8; ++group)
                fit_contributions_->setItem(i, group + 1,
                    new QTableWidgetItem(display_value(groups[static_cast<std::size_t>(group)])));
        }
        fit_setup_table_->resizeColumnsToContents();
        fit_contributions_->resizeColumnsToContents();
        fit_run_button_->setEnabled(true);
        fit_status_->setText(QStringLiteral("Fit complete"));
        if (manifest_) {
            auto run_id = zscan::schema::binary::random_uuid_v4();
            if (run_id) {
                const auto directory = std::filesystem::path(session_root_.toStdString())
                    / "derived" / run_id->to_string();
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                const auto array_header = zscan::schema::binary::Header {
                    zscan::schema::binary::FileKind::DerivedArrays, *run_id, manifest_->session_id,
                    *zscan::schema::UtcTimestamp::parse("2026-10-25T15:00:00Z")
                };
                if (fit_request_ && workup_) {
                    auto prepared = zscan::schema::make_prepared_arrays(
                        *workup_, *fit_request_, array_header);
                    auto fitted = zscan::schema::make_fit_result_arrays(result, array_header);
                    if (prepared && fitted) {
                        auto prepared_path = zscan::schema::SessionPath::parse(
                            "derived/" + run_id->to_string() + "/prepared_arrays.bin");
                        auto fitted_path = zscan::schema::SessionPath::parse(
                            "derived/" + run_id->to_string() + "/fit_arrays.bin");
                        if (prepared_path && fitted_path) {
                            auto prepared_seal
                                = zscan::schema::storage::write_derived_arrays_atomic(
                                    session_root_.toStdString(), *prepared_path, *prepared);
                            auto fitted_seal = zscan::schema::storage::write_derived_arrays_atomic(
                                session_root_.toStdString(), *fitted_path, *fitted);
                            if (prepared_seal && fitted_seal) {
                                zscan::schema::FitRequestDocument request_document { *run_id, { },
                                    { }, { }, { }, { }, { },
                                    { *prepared_path, prepared_seal->byte_size,
                                        prepared_seal->sha256 },
                                    { *prepared_path, prepared_seal->byte_size,
                                        prepared_seal->sha256 } };
                                request_document.initial_parameters.values
                                    = { fit_request_->initial_parameters.sigma_g,
                                          fit_request_->initial_parameters.sigma_s,
                                          fit_request_->initial_parameters.sigma_t,
                                          fit_request_->initial_parameters.tau10,
                                          fit_request_->initial_parameters.tau21,
                                          fit_request_->initial_parameters.tau13,
                                          fit_request_->initial_parameters.tau30,
                                          fit_request_->initial_parameters.tau43 };
                                const char* names[] = { "sigma_g", "sigma_s", "sigma_t", "tau10",
                                    "tau21", "tau13", "tau30", "tau43" };
                                for (auto parameter : fit_request_->fitted_parameters)
                                    request_document.fitted_parameters.push_back(
                                        names[static_cast<std::size_t>(parameter)]);
                                auto request_text
                                    = zscan::schema::toml_io::format_fit_request(request_document);
                                if (request_text)
                                    std::ofstream(directory / "fit_request.toml") << *request_text;
                            }
                        }
                    }
                }
                zscan::schema::FitResultDocument document;
                document.run_id = *run_id;
                document.converged = result.converged;
                document.status = result.status;
                document.function_evaluations = result.function_evaluations;
                document.residual_norm = result.residual_norm;
                document.initial_residual_norm = result.initial_residual_norm;
                document.max_abs_residual = result.max_abs_residual;
                document.chi_squared = result.chi_squared;
                if (result.uncertainty) {
                    document.jacobian_rank = result.uncertainty->jacobian_rank;
                    const char* names[] = { "sigma_g", "sigma_s", "sigma_t", "tau10", "tau21",
                        "tau13", "tau30", "tau43" };
                    const double values[] = { result.parameters.sigma_g, result.parameters.sigma_s,
                        result.parameters.sigma_t, result.parameters.tau10, result.parameters.tau21,
                        result.parameters.tau13, result.parameters.tau30, result.parameters.tau43 };
                    for (std::size_t i = 0; i < result.uncertainty->parameter_order.size(); ++i) {
                        const auto index
                            = static_cast<std::size_t>(result.uncertainty->parameter_order[i]);
                        zscan::schema::ParameterResult parameter;
                        parameter.parameter = names[index];
                        parameter.value = values[index];
                        parameter.conditional_standard_error
                            = result.uncertainty->standard_errors[i];
                        if (!result.uncertainty->total_standard_errors.empty())
                            parameter.total_standard_error
                                = result.uncertainty->total_standard_errors[i];
                        parameter.calibration_variance_contributions
                            = result.uncertainty->calibration_variance_contributions[i];
                        document.parameters.push_back(std::move(parameter));
                    }
                }
                const auto text = zscan::schema::toml_io::format_fit_result(document);
                if (text) {
                    std::ofstream output(directory / "fit_result.toml", std::ios::binary);
                    output << *text;
                }
                const auto identity =
                    [&](const std::string& relative) -> std::optional<zscan::schema::FileIdentity> {
                    const auto path = std::filesystem::path(session_root_.toStdString()) / relative;
                    std::ifstream input(path, std::ios::binary);
                    if (!input)
                        return { };
                    const std::vector<std::uint8_t> bytes(
                        (std::istreambuf_iterator<char>(input)), { });
                    auto digest = zscan::schema::binary::sha256(bytes);
                    if (!digest)
                        return { };
                    return zscan::schema::FileIdentity {
                        *zscan::schema::SessionPath::parse(relative), bytes.size(), *digest
                    };
                };
                const auto fit_request_file
                    = identity("derived/" + run_id->to_string() + "/fit_request.toml");
                const auto fit_result_file
                    = identity("derived/" + run_id->to_string() + "/fit_result.toml");
                const auto prepared_file
                    = identity("derived/" + run_id->to_string() + "/prepared_arrays.bin");
                if (fit_request_file && fit_result_file && prepared_file && workup_) {
                    zscan::schema::DerivedRunDocument run_document { *run_id, manifest_->session_id,
                        *zscan::schema::UtcTimestamp::parse("2026-10-25T15:00:00Z"),
                        zscan::schema::DerivedRunState::Complete, "qt-preview",
                        "d3612e964ad8b8e8274aa322d32ffbac2b28b706", false,
                        { { "fit_request", "", *fit_request_file },
                            { "fit_result", "", *fit_result_file },
                            { "prepared_arrays", "", *prepared_file } },
                        { }, 0.05, 0.05, *fit_request_file, *fit_result_file, { } };
                    for (const auto& dark : workup_->darks)
                        if (auto id = zscan::schema::Uuid::parse(dark.record_id); id)
                            run_document.selected_dark_ids.push_back(*id);
                    if (auto run_text = zscan::schema::toml_io::format_run(run_document))
                        std::ofstream(directory / "run.toml", std::ios::binary) << *run_text;
                }
                status_->setText(QStringLiteral("Fit complete · run %1")
                        .arg(QString::fromStdString(run_id->to_string())));
            }
        }
        if (workup_) {
            std::vector<CurvePlot::Series> series;
            for (std::size_t i = 0; i < result.curves.size() && i < workup_->curves.size(); ++i)
                series.push_back({ QStringLiteral("Curve %1").arg(i + 1),
                    workup_->curves[i].scan_positions_m, workup_->curves[i].transmission,
                    workup_->curves[i].transmission_standard_errors,
                    result.curves[i].predicted_transmission });
            fit_plot_->setSeries(std::move(series));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(
        QtConcurrent::run([request = std::move(*request)] { return zfit::fit(request); }));
}

void SessionWindow::showWorkupCurve(int index)
{
    if (!workup_ || index < 0 || index >= static_cast<int>(workup_->curves.size()))
        return;
    const auto& curve = workup_->curves[static_cast<std::size_t>(index)];
    const auto& energy = workup_->energies[static_cast<std::size_t>(index)];
    const auto& dark = workup_->darks[static_cast<std::size_t>(index)];
    workup_summary_->setText(QStringLiteral(
        "Energy %1 ± %2 J · Dark %3 · drift checked: %4 · Entrance-chain transmission %5 ± %6")
            .arg(energy.mean_j, 0, 'g', 8)
            .arg(energy.standard_error_j, 0, 'g', 8)
            .arg(QString::fromStdString(dark.record_id))
            .arg(curve.dark_drift_checked ? "yes" : "no")
            .arg(workup_->calibration.entrance_chain_transmission, 0, 'g', 8)
            .arg(workup_->calibration.entrance_chain_standard_error, 0, 'g', 8));
    workup_table_->setRowCount(static_cast<int>(curve.scan_positions_m.size()));
    for (int row = 0; row < workup_table_->rowCount(); ++row) {
        workup_table_->setItem(
            row, 0, new QTableWidgetItem(QString::number(curve.scan_positions_m[row], 'g', 12)));
        workup_table_->setItem(
            row, 1, new QTableWidgetItem(QString::number(curve.transmission[row], 'g', 12)));
        workup_table_->setItem(row, 2,
            new QTableWidgetItem(
                QString::number(curve.transmission_standard_errors[row], 'g', 12)));
        workup_table_->setItem(row, 3,
            new QTableWidgetItem(QStringLiteral("%1, %2")
                    .arg(curve.dark_sensitivities[row][0])
                    .arg(curve.dark_sensitivities[row][1])));
    }
    workup_table_->resizeColumnsToContents();
    curve_plot_->setData(
        curve.scan_positions_m, curve.transmission, curve.transmission_standard_errors);
}

void SessionWindow::editSession()
{
    if (!manifest_) {
        QMessageBox::information(this, QStringLiteral("No session"),
            QStringLiteral("Open a session before editing it."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit session curves"));
    dialog.resize(900, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* material_box = new QGroupBox(QStringLiteral("Snapshot materials"), &dialog);
    auto* material_layout = new QFormLayout(material_box);
    auto* solvent_material = new QComboBox(material_box);
    auto* window_material = new QComboBox(material_box);
    if (materials_)
        for (const auto& material : materials_->materials) {
            const auto label = QString::fromStdString(material.name
                + " · n=" + std::to_string(material.refractive_index) + " · " + material.status);
            (material.kind == zscan::schema::MaterialKind::Solvent ? solvent_material
                                                                   : window_material)
                ->addItem(label, QString::fromStdString(material.material_id));
        }
    material_layout->addRow(QStringLiteral("Solvent"), solvent_material);
    material_layout->addRow(QStringLiteral("Entrance window"), window_material);
    layout->addWidget(material_box);
    auto* table = new QTableWidget(static_cast<int>(manifest_->curves.size()), 6, &dialog);
    table->setHorizontalHeaderLabels(
        { "Order", "Curve ID", "Sample scan", "Blank scan", "Energy burst", "Cuvette pair" });
    QStringList sample_scans;
    QStringList blank_scans;
    const auto scan_label = [](const auto& scan) {
        return QStringLiteral("%1 · %2 UTC · %3 · …%4")
            .arg(scan.kind == zscan::schema::ScanKind::Sample ? QStringLiteral("Sample scan")
                                                              : QStringLiteral("Blank scan"))
            .arg(QString::fromStdString(scan.acquired_at.to_string()))
            .arg(QString::fromStdString(scan.cuvette_id))
            .arg(QString::fromStdString(scan.acquisition_id.to_string().substr(28)));
    };
    for (const auto& scan : manifest_->scans) {
        if (scan.state != zscan::schema::AcquisitionState::Complete)
            continue;
        (scan.kind == zscan::schema::ScanKind::Sample ? sample_scans : blank_scans)
            << scan_label(scan);
    }
    QStringList bursts;
    for (const auto& burst : manifest_->energy_bursts)
        bursts << QStringLiteral("Energy burst · %1 UTC · %2 readings · …%3")
                      .arg(QString::fromStdString(burst.entered_at.to_string()))
                      .arg(burst.reading_count)
                      .arg(QString::fromStdString(burst.burst_id.to_string().substr(28)));
    // Pair IDs are read from the session snapshot through the manifest's
    // already validated references; the editor keeps the existing value when
    // no catalog-backed selector is available.
    QStringList sample_ids, blank_ids, burst_ids;
    for (const auto& scan : manifest_->scans)
        if (scan.state == zscan::schema::AcquisitionState::Complete)
            (scan.kind == zscan::schema::ScanKind::Sample ? sample_ids : blank_ids)
                << QString::fromStdString(scan.acquisition_id.to_string());
    for (const auto& burst : manifest_->energy_bursts)
        burst_ids << QString::fromStdString(burst.burst_id.to_string());
    const auto set_combo = [&](int row, int column, const QStringList& values,
                               const QStringList& ids, const QString& current) {
        auto* combo = new QComboBox(table);
        for (int i = 0; i < values.size(); ++i)
            combo->addItem(values[i], ids.value(i));
        const int index = combo->findData(current);
        if (index >= 0)
            combo->setCurrentIndex(index);
        else
            combo->addItem(current), combo->setCurrentIndex(combo->count() - 1);
        table->setCellWidget(row, column, combo);
    };
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto& curve = manifest_->curves[static_cast<std::size_t>(row)];
        table->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
        table->setItem(
            row, 1, new QTableWidgetItem(QString::fromStdString(curve.curve_id.to_string())));
        table->item(row, 0)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        table->item(row, 1)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        set_combo(row, 2, sample_scans, sample_ids,
            QString::fromStdString(curve.sample_acquisition_id.to_string()));
        set_combo(row, 3, blank_scans, blank_ids,
            QString::fromStdString(curve.blank_acquisition_id.to_string()));
        set_combo(
            row, 4, bursts, burst_ids, QString::fromStdString(curve.energy_burst_id.to_string()));
        auto* pair = new QComboBox(table);
        if (instrument_)
            for (const auto& value : instrument_->cuvette_pairs)
                pair->addItem(QString::fromStdString(value.calibration_id + " · "
                                  + value.sample_cuvette_id + " / " + value.blank_cuvette_id),
                    QString::fromStdString(value.calibration_id));
        const int pair_index
            = pair->findData(QString::fromStdString(curve.cuvette_pair_calibration_id));
        if (pair_index >= 0)
            pair->setCurrentIndex(pair_index);
        else
            pair->addItem(QString::fromStdString(curve.cuvette_pair_calibration_id),
                QString::fromStdString(curve.cuvette_pair_calibration_id));
        table->setCellWidget(row, 5, pair);
    }
    table->resizeColumnsToContents();
    layout->addWidget(table);
    auto* controls = new QHBoxLayout;
    auto* up = new QPushButton(QStringLiteral("Move up"), &dialog);
    auto* down = new QPushButton(QStringLiteral("Move down"), &dialog);
    auto* remove = new QPushButton(QStringLiteral("Remove curve"), &dialog);
    controls->addWidget(up);
    controls->addWidget(down);
    controls->addWidget(remove);
    controls->addStretch();
    layout->addLayout(controls);
    connect(up, &QPushButton::clicked, &dialog, [table] {
        const int row = table->currentRow();
        if (row <= 0)
            return;
        table->insertRow(row - 1);
        for (int c = 0; c < 2; ++c)
            table->setItem(row - 1, c, table->takeItem(row + 1, c));
        for (int c = 2; c < table->columnCount(); ++c)
            table->setCellWidget(row - 1, c, table->cellWidget(row + 1, c));
        table->removeRow(row + 1);
        table->setCurrentCell(row - 1, 0);
    });
    connect(down, &QPushButton::clicked, &dialog, [table] {
        const int row = table->currentRow();
        if (row < 0 || row + 1 >= table->rowCount())
            return;
        table->insertRow(row + 2);
        for (int c = 0; c < 2; ++c)
            table->setItem(row + 2, c, table->takeItem(row, c));
        for (int c = 2; c < table->columnCount(); ++c)
            table->setCellWidget(row + 2, c, table->cellWidget(row, c));
        table->removeRow(row);
        table->setCurrentCell(row + 1, 0);
    });
    connect(remove, &QPushButton::clicked, &dialog, [table] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    auto* buttons
        = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return;
    std::vector<zscan::schema::CurveMapping> revised;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto id = table->item(row, 1)->text().toStdString();
        auto found = std::find_if(manifest_->curves.begin(), manifest_->curves.end(),
            [&](const auto& curve) { return curve.curve_id.to_string() == id; });
        if (found != manifest_->curves.end()) {
            auto curve = *found;
            auto* sample = qobject_cast<QComboBox*>(table->cellWidget(row, 2));
            auto* blank = qobject_cast<QComboBox*>(table->cellWidget(row, 3));
            auto* energy = qobject_cast<QComboBox*>(table->cellWidget(row, 4));
            auto* pair = qobject_cast<QComboBox*>(table->cellWidget(row, 5));
            if (!sample || !blank || !energy || !pair)
                continue;
            curve.sample_acquisition_id
                = *zscan::schema::Uuid::parse(sample->currentData().toString().toStdString());
            curve.blank_acquisition_id
                = *zscan::schema::Uuid::parse(blank->currentData().toString().toStdString());
            curve.energy_burst_id
                = *zscan::schema::Uuid::parse(energy->currentData().toString().toStdString());
            curve.cuvette_pair_calibration_id = pair->currentData().toString().toStdString();
            revised.push_back(std::move(curve));
        }
    }
    if (materials_ && sample_ && instrument_) {
        const auto find_material
            = [&](const QString& value) -> const zscan::schema::MaterialPreset* {
            for (const auto& material : materials_->materials)
                if (QString::fromStdString(material.material_id) == value)
                    return &material;
            return nullptr;
        };
        const auto* solvent = find_material(solvent_material->currentData().toString());
        const auto* window = find_material(window_material->currentData().toString());
        if (solvent && window) {
            auto sample = *sample_;
            sample.solvent_id = solvent->material_id;
            sample.solvent_refractive_index.value = solvent->refractive_index;
            sample.solvent_refractive_index.standard_uncertainty
                = solvent->refractive_index * solvent->relative_standard_uncertainty;
            sample.solvent_refractive_index.provenance_id = solvent->provenance_id;
            auto instrument = *instrument_;
            instrument.entrance_window_refractive_index.value = window->refractive_index;
            instrument.entrance_window_refractive_index.standard_uncertainty
                = window->refractive_index * window->relative_standard_uncertainty;
            instrument.entrance_window_refractive_index.provenance_id = window->provenance_id;
            const auto sample_text = zscan::schema::toml_io::format_sample(sample);
            const auto instrument_text = zscan::schema::toml_io::format_instrument(instrument);
            if (sample_text && instrument_text) {
                const auto revision = std::to_string(manifest_->manifest_revision + 1);
                const auto write_snapshot = [&](const QString& name, const std::string& text) {
                    const auto path
                        = std::filesystem::path(session_root_.toStdString()) / name.toStdString();
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    output.write(text.data(), static_cast<std::streamsize>(text.size()));
                    output.close();
                    const auto digest = zscan::schema::binary::sha256(
                        std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
                    return zscan::schema::FileIdentity { *zscan::schema::SessionPath::parse(
                                                             name.toStdString()),
                        static_cast<std::uint64_t>(text.size()), *digest };
                };
                manifest_->sample_snapshot = write_snapshot(
                    QStringLiteral("sample_") + QString::fromStdString(revision) + ".toml",
                    *sample_text);
                manifest_->instrument_snapshot = write_snapshot(
                    QStringLiteral("instrument_") + QString::fromStdString(revision) + ".toml",
                    *instrument_text);
                sample_ = std::move(sample);
                instrument_ = std::move(instrument);
            }
        }
    }
    manifest_->curves = std::move(revised);
    ++manifest_->manifest_revision;
    dirty_ = true;
    status_->setText(QStringLiteral("Unsaved session changes"));
}

void SessionWindow::saveSession()
{
    if (!manifest_ || !dirty_)
        return;
    if (auto validation = zscan::schema::toml_io::format_session(*manifest_); !validation) {
        QMessageBox::critical(this, QStringLiteral("Save failed"),
            QString::fromStdString(validation.error().message));
        return;
    }
    const auto current = zscan::schema::storage::read_session_manifest(session_root_.toStdString());
    if (!current || current->manifest_revision + 1 != manifest_->manifest_revision) {
        QMessageBox::warning(this, QStringLiteral("Session changed on disk"),
            QStringLiteral("Reload the session before saving these edits."));
        return;
    }
    const auto result = zscan::schema::storage::write_session_manifest_atomic(
        session_root_.toStdString(), *manifest_);
    if (!result)
        QMessageBox::critical(
            this, QStringLiteral("Save failed"), QString::fromStdString(result.error().message));
    else {
        dirty_ = false;
        status_->setText(QStringLiteral("Session revision saved"));
    }
}

void SessionWindow::saveSessionAs()
{
    if (!manifest_)
        return;
    const auto parent
        = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose destination folder"));
    if (parent.isEmpty())
        return;
    const auto new_id = zscan::schema::binary::random_uuid_v4();
    if (!new_id)
        return;
    const auto destination
        = std::filesystem::path(parent.toStdString()) / ("session_" + new_id->to_string());
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    const auto fail_save_as = [&] {
        std::error_code cleanup_error;
        std::filesystem::remove_all(destination, cleanup_error);
        QMessageBox::critical(this, QStringLiteral("Save As failed"),
            QStringLiteral("The new session was discarded; the source session was unchanged."));
    };
    if (ec) {
        fail_save_as();
        return;
    }
    auto clone = [&](zscan::schema::FileIdentity& identity, bool rewrite_header) -> bool {
        const auto source
            = std::filesystem::path(session_root_.toStdString()) / identity.path.value();
        const auto target = destination / identity.path.value();
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ifstream input(source, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), { });
        if (bytes.empty())
            return false;
        if (rewrite_header) {
            auto header = zscan::schema::binary::decode_header(bytes);
            if (!header)
                return false;
            header->session_id = *new_id;
            auto encoded = zscan::schema::binary::encode_header(*header);
            if (!encoded)
                return false;
            std::copy(encoded->begin(), encoded->end(), bytes.begin());
        }
        std::ofstream output(target, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto digest = zscan::schema::binary::sha256(bytes);
        if (!digest)
            return false;
        identity.byte_size = bytes.size();
        identity.sha256 = *digest;
        return true;
    };
    auto cloned = *manifest_;
    cloned.session_id = *new_id;
    cloned.manifest_revision = 1;
    cloned.derived_runs.clear();
    auto copy_snapshot = [&](zscan::schema::FileIdentity& identity) {
        const auto source
            = std::filesystem::path(session_root_.toStdString()) / identity.path.value();
        const auto target = destination / identity.path.value();
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(
            source, target, std::filesystem::copy_options::overwrite_existing, ec);
        identity.byte_size = std::filesystem::file_size(target, ec);
        std::ifstream input(target, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), { });
        identity.sha256 = zscan::schema::binary::sha256(bytes).value();
        return !ec;
    };
    if (!copy_snapshot(cloned.sample_snapshot) || !copy_snapshot(cloned.instrument_snapshot)) {
        fail_save_as();
        return;
    }
    for (auto& record : cloned.scans)
        if (!clone(record.file, true)) {
            fail_save_as();
            return;
        }
    for (auto& record : cloned.darks)
        if (!clone(record.file, true)) {
            fail_save_as();
            return;
        }
    for (auto& record : cloned.energy_bursts)
        if (!clone(record.file, true)) {
            fail_save_as();
            return;
        }
    if (auto saved = zscan::schema::storage::write_session_manifest_atomic(destination, cloned);
        !saved)
        fail_save_as();
    else
        QMessageBox::information(
            this, QStringLiteral("Session copied"), QString::fromStdString(destination.string()));
}

void SessionWindow::editSnapshots()
{
    if (!manifest_ || !sample_ || !instrument_ || !materials_) {
        QMessageBox::information(this, QStringLiteral("Snapshots unavailable"),
            QStringLiteral(
                "Open a session with readable snapshots and config/materials.toml first."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit session snapshots"));
    auto* layout = new QFormLayout(&dialog);
    auto* solvent = new QComboBox(&dialog);
    auto* window = new QComboBox(&dialog);
    for (const auto& material : materials_->materials) {
        const auto label = QString::fromStdString(material.name
            + " · n=" + std::to_string(material.refractive_index) + " · " + material.status);
        if (material.kind == zscan::schema::MaterialKind::Solvent)
            solvent->addItem(label, QString::fromStdString(material.material_id));
        else
            window->addItem(label, QString::fromStdString(material.material_id));
    }
    layout->addRow(QStringLiteral("Solvent material"), solvent);
    layout->addRow(QStringLiteral("Entrance-window material"), window);
    auto* buttons
        = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto find = [&](const QString& id) -> const zscan::schema::MaterialPreset* {
        for (const auto& material : materials_->materials)
            if (QString::fromStdString(material.material_id) == id)
                return &material;
        return nullptr;
    };
    const auto* solvent_material = find(solvent->currentData().toString());
    const auto* window_material = find(window->currentData().toString());
    if (!solvent_material || !window_material)
        return;
    auto sample = *sample_;
    sample.solvent_id = solvent_material->material_id;
    sample.solvent_refractive_index.value = solvent_material->refractive_index;
    sample.solvent_refractive_index.standard_uncertainty
        = solvent_material->refractive_index * solvent_material->relative_standard_uncertainty;
    sample.solvent_refractive_index.provenance_id = solvent_material->provenance_id;
    sample.solvent_refractive_index.uncertainty_evaluation_id = "catalog-draft-1pct";
    auto instrument = *instrument_;
    instrument.entrance_window_refractive_index.value = window_material->refractive_index;
    instrument.entrance_window_refractive_index.standard_uncertainty
        = window_material->refractive_index * window_material->relative_standard_uncertainty;
    instrument.entrance_window_refractive_index.provenance_id = window_material->provenance_id;
    instrument.entrance_window_refractive_index.uncertainty_evaluation_id = "catalog-draft-1pct";
    const auto sample_text = zscan::schema::toml_io::format_sample(sample);
    const auto instrument_text = zscan::schema::toml_io::format_instrument(instrument);
    if (!sample_text || !instrument_text) {
        QMessageBox::critical(this, QStringLiteral("Snapshot validation failed"),
            QStringLiteral("The selected material values could not be serialized."));
        return;
    }
    const auto write_snapshot = [&](const QString& name, const std::string& text) {
        const auto path = std::filesystem::path(session_root_.toStdString()) / name.toStdString();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        const auto digest = zscan::schema::binary::sha256(
            std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        return zscan::schema::FileIdentity { *zscan::schema::SessionPath::parse(name.toStdString()),
            static_cast<std::uint64_t>(text.size()), *digest };
    };
    const auto revision = std::to_string(manifest_->manifest_revision + 1);
    manifest_->sample_snapshot = write_snapshot(
        QStringLiteral("sample_") + QString::fromStdString(revision) + QStringLiteral(".toml"),
        *sample_text);
    manifest_->instrument_snapshot = write_snapshot(
        QStringLiteral("instrument_") + QString::fromStdString(revision) + QStringLiteral(".toml"),
        *instrument_text);
    ++manifest_->manifest_revision;
    const auto saved = zscan::schema::storage::write_session_manifest_atomic(
        session_root_.toStdString(), *manifest_);
    if (!saved) {
        QMessageBox::critical(this, QStringLiteral("Snapshot save failed"),
            QString::fromStdString(saved.error().message));
        return;
    }
    sample_ = std::move(sample);
    instrument_ = std::move(instrument);
    dirty_ = false;
    status_->setText(QStringLiteral("New snapshots and session revision saved"));
}

void SessionWindow::showCurveDetails(int row)
{
    details_->setPlainText(row >= 0 && row < curve_details_.size()
            ? curve_details_[row]
            : QStringLiteral("Select a curve"));
}

void SessionWindow::showReport(
    const QString& root, bool valid, QStringList errors, QStringList warnings)
{
    status_->setText(valid ? QStringLiteral("Session valid") : QStringLiteral("Session invalid"));
    diagnostics_->appendPlainText(QStringLiteral("Session: ") + root);
    for (const auto& error : errors)
        diagnostics_->appendPlainText(QStringLiteral("ERROR: ") + error);
    for (const auto& warning : warnings)
        diagnostics_->appendPlainText(QStringLiteral("WARNING: ") + warning);
}

} // namespace zscan::app
