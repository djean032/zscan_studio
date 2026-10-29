#include "acquisition_wizard.hpp"
#include "material_editor.hpp"
#include "zscan/acquisition/dark_runner.hpp"
#include "zscan/acquisition/session.hpp"
#include "zscan/acquisition/worker.hpp"
#include "zscan/hardware/mock.hpp"
#include "zscan/schema/materials.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace zscan::app {
namespace {

    QString state_name(acquisition::State state)
    {
        switch (state) {
        case acquisition::State::Idle:
            return "Not started";
        case acquisition::State::Setup:
            return "Session setup";
        case acquisition::State::Preflight:
            return "Hardware preflight";
        case acquisition::State::OperatorPreparation:
            return "Operator preparation";
        case acquisition::State::DarkAcquisition:
            return "Dark acquisition";
        case acquisition::State::EnergyCalibration:
            return "Energy calibration";
        case acquisition::State::SamplePreparation:
            return "Sample preparation";
        case acquisition::State::SampleAcquisition:
            return "Sample acquisition";
        case acquisition::State::Review:
            return "Review";
        case acquisition::State::Published:
            return "Published";
        case acquisition::State::Failed:
            return "Failed";
        case acquisition::State::Cancelled:
            return "Cancelled";
        case acquisition::State::EmergencyStopped:
            return "Emergency stopped";
        }
        return "Unknown";
    }

    int step_index(acquisition::State state)
    {
        switch (state) {
        case acquisition::State::Setup:
            return 0;
        case acquisition::State::Preflight:
            return 1;
        case acquisition::State::OperatorPreparation:
            return 2;
        case acquisition::State::DarkAcquisition:
            return 3;
        case acquisition::State::EnergyCalibration:
            return 4;
        case acquisition::State::SamplePreparation:
            return 5;
        case acquisition::State::SampleAcquisition:
            return 6;
        case acquisition::State::Review:
            return 7;
        case acquisition::State::Published:
            return 8;
        default:
            return -1;
        }
    }

} // namespace

AcquisitionWizard::AcquisitionWizard(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    steps_ = new QListWidget(this);
    steps_->addItems({ "Session setup", "Hardware preflight", "Operator preparation",
        "Dark acquisition", "Energy calibration", "Sample preparation", "Sample acquisition",
        "Review", "Published" });
    steps_->setFixedWidth(190);
    root->addWidget(steps_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* page = new QWidget(scroll);
    auto* content = new QVBoxLayout(page);
    auto* heading = new QLabel(QStringLiteral("Guided acquisition"), page);
    heading->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    content->addWidget(heading);
    state_label_ = new QLabel(this);
    instruction_ = new QLabel(this);
    instruction_->setWordWrap(true);
    instruction_->setMinimumHeight(48);
    content->addWidget(state_label_);
    content->addWidget(instruction_);

    auto* setup = new QGroupBox(QStringLiteral("Acquisition settings"), this);
    setup_group_ = setup;
    auto* setup_form = new QFormLayout(setup);
    session_name_ = new QLineEdit(setup);
    session_name_->setObjectName("Session name");
    session_name_->setPlaceholderText(QStringLiteral("New session name"));
    session_root_ = new QLineEdit(setup);
    session_root_->setObjectName("New session directory");
    session_root_->setPlaceholderText(QStringLiteral("Choose a new session directory"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), setup);
    auto* root_row = new QHBoxLayout;
    root_row->addWidget(session_root_);
    root_row->addWidget(browse);
    sample_id_ = new QLineEdit(setup);
    solvent_id_ = new QLineEdit(setup);
    sample_id_->setObjectName("Sample ID");
    solvent_id_->setObjectName("Solvent ID");
    solvent_type_ = new QComboBox(setup);
    window_type_ = new QComboBox(setup);
    solvent_type_->setObjectName("Solvent type");
    window_type_->setObjectName("Entrance window");
    solvent_type_->addItem(QStringLiteral("Select solvent type"));
    window_type_->addItem(QStringLiteral("Select entrance window type"));
    const auto built_in = schema::load_material_catalog(
        QDir::currentPath().toStdString() + "/config/materials.toml");
    catalog_.schema_version = 1;
    catalog_.wavelength_m = 532e-9;
    if (built_in)
        catalog_.materials = built_in->materials;
    const auto user_file = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/materials.toml");
    if (QFileInfo::exists(user_file)) {
        const auto user_catalog = schema::load_material_catalog(user_file.toStdString());
        if (user_catalog) {
            for (const auto& entry : user_catalog->materials)
                catalog_.materials.push_back(entry);
        }
    }
    auto populate = [this](const schema::MaterialPreset& material) {
        auto* combo = material.kind == schema::MaterialKind::Solvent ? solvent_type_ : window_type_;
        combo->addItem(
            QStringLiteral("%1 · n=%2 ± %3")
                .arg(QString::fromStdString(material.name))
                .arg(material.refractive_index, 0, 'g', 8)
                .arg(material.refractive_index * material.relative_standard_uncertainty, 0, 'g', 3),
            QString::fromStdString(material.material_id));
    };
    for (const auto& material : catalog_.materials)
        populate(material);
    auto* add_solvent = new QPushButton(QStringLiteral("Add solvent type…"), setup);
    auto* add_window = new QPushButton(QStringLiteral("Add entrance window…"), setup);
    auto add_material = [this, user_file, populate](schema::MaterialKind kind) {
        MaterialEditor editor(kind, this);
        if (editor.exec() != QDialog::Accepted)
            return;
        auto entry = editor.result();
        if (!entry)
            return;
        for (const auto& existing : catalog_.materials)
            if (existing.material_id == entry->material_id) {
                show_failure(
                    QStringLiteral("Material ID already exists. Use a new ID for a new type."));
                return;
            }
        schema::MaterialCatalog user { 1, 532e-9, { *entry } };
        if (QFileInfo::exists(user_file)) {
            auto old = schema::load_material_catalog(user_file.toStdString());
            if (!old) {
                show_failure(QString::fromStdString(old.error().message));
                return;
            }
            user = *old;
            user.materials.push_back(*entry);
        }
        QDir().mkpath(QFileInfo(user_file).absolutePath());
        if (auto saved = schema::save_material_catalog(user_file.toStdString(), user); !saved) {
            show_failure(QString::fromStdString(saved.error().message));
            return;
        }
        catalog_.materials.push_back(*entry);
        populate(*entry);
    };
    connect(add_solvent, &QPushButton::clicked, this,
        [add_material] { add_material(schema::MaterialKind::Solvent); });
    connect(add_window, &QPushButton::clicked, this,
        [add_material] { add_material(schema::MaterialKind::EntranceWindow); });
    wavelength_nm_ = new QLineEdit(setup);
    wavelength_nm_->setObjectName("Wavelength (nm)");
    wavelength_nm_->setText(QStringLiteral("532"));
    setup_form->addRow(QStringLiteral("Session name"), session_name_);
    setup_form->addRow(QStringLiteral("New session directory"), root_row);
    setup_form->addRow(QStringLiteral("Sample ID"), sample_id_);
    setup_form->addRow(QStringLiteral("Solvent ID"), solvent_id_);
    setup_form->addRow(QStringLiteral("Solvent type"), solvent_type_);
    setup_form->addRow(QStringLiteral("Entrance window"), window_type_);
    setup_form->addRow(add_solvent, add_window);
    setup_form->addRow(QStringLiteral("Wavelength (nm)"), wavelength_nm_);
    auto field = [setup, setup_form](const QString& label, const QString& hint) {
        auto* input = new QLineEdit(setup);
        input->setObjectName(label);
        input->setPlaceholderText(hint);
        setup_form->addRow(label, input);
        return input;
    };
    concentration_ = field("Concentration (mol/L)", "Required");
    concentration_uncertainty_ = field("Concentration standard uncertainty (mol/L)", "Required");
    waist_ = field("Beam waist (m)", "Required");
    waist_uncertainty_ = field("Beam waist standard uncertainty (m)", "Required");
    m2_ = field("M²", "Required");
    m2_uncertainty_ = field("M² standard uncertainty", "Required");
    meter_id_ = field("Energy meter calibration ID", "Required");
    meter_uncertainty_ = field("Energy meter standard uncertainty (J)", "Required");
    sample_cuvette_id_ = field("Sample cuvette ID", "Required");
    blank_cuvette_id_ = field("Blank cuvette ID", "Required");
    cuvette_path_ = field("Cuvette path length (m)", "Required");
    cuvette_path_uncertainty_ = field("Cuvette path length uncertainty (m)", "Required");
    cuvette_index_ = field("Cuvette refractive index", "Required");
    cuvette_index_uncertainty_ = field("Cuvette index uncertainty", "Required");
    blank_path_ = field("Blank cuvette path length (m)", "Required");
    blank_path_uncertainty_ = field("Blank path length uncertainty (m)", "Required");
    blank_index_ = field("Blank cuvette refractive index", "Required");
    blank_index_uncertainty_ = field("Blank cuvette index uncertainty", "Required");
    sample_cuvette_provenance_ = field("Sample cuvette provenance ID", "Required");
    blank_cuvette_provenance_ = field("Blank cuvette provenance ID", "Required");
    const auto cuvette_file = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/cuvettes.ini");
    auto* types = new QComboBox(setup);
    types->addItem("Choose a saved cuvette type…");
    {
        QSettings catalog(cuvette_file, QSettings::IniFormat);
        types->addItems(catalog.childGroups());
    }
    auto* save_type = new QPushButton("Save cuvette type…", setup);
    auto* apply_sample = new QPushButton("Use for sample", setup);
    auto* apply_blank = new QPushButton("Use for blank", setup);
    setup_form->addRow("Saved cuvette types", types);
    setup_form->addRow(save_type, apply_sample);
    setup_form->addRow(apply_blank);
    auto apply = [this, types, cuvette_file](bool blank) {
        if (types->currentIndex() == 0)
            return;
        QSettings catalog(cuvette_file, QSettings::IniFormat);
        catalog.beginGroup(types->currentText());
        auto* path = blank ? blank_path_ : cuvette_path_;
        auto* path_uncertainty = blank ? blank_path_uncertainty_ : cuvette_path_uncertainty_;
        auto* index = blank ? blank_index_ : cuvette_index_;
        auto* index_uncertainty = blank ? blank_index_uncertainty_ : cuvette_index_uncertainty_;
        path->setText(catalog.value("path_m").toString());
        path_uncertainty->setText(catalog.value("path_uncertainty_m").toString());
        index->setText(catalog.value("index").toString());
        index_uncertainty->setText(catalog.value("index_uncertainty").toString());
        (blank ? blank_cuvette_provenance_ : sample_cuvette_provenance_)
            ->setText(catalog.value("provenance_id").toString());
    };
    connect(apply_sample, &QPushButton::clicked, this, [apply] { apply(false); });
    connect(apply_blank, &QPushButton::clicked, this, [apply] { apply(true); });
    connect(save_type, &QPushButton::clicked, this, [this, types, cuvette_file] {
        bool accepted = false;
        const auto name = QInputDialog::getText(
            this, "Save cuvette type", "Name:", QLineEdit::Normal, { }, &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty() || name.contains('/') || name.contains('\\'))
            return;
        QSettings catalog(cuvette_file, QSettings::IniFormat);
        if (catalog.childGroups().contains(name)) {
            show_failure("Cuvette type already exists; saved types cannot be overwritten.");
            return;
        }
        for (auto* value : { cuvette_path_, cuvette_path_uncertainty_, cuvette_index_,
                 cuvette_index_uncertainty_ }) {
            bool parsed = false;
            const double number = value->text().toDouble(&parsed);
            if (!parsed || !std::isfinite(number) || number <= 0) {
                show_failure("Enter positive cuvette values and uncertainties first.");
                return;
            }
        }
        QDir().mkpath(QFileInfo(cuvette_file).absolutePath());
        if (sample_cuvette_provenance_->text().trimmed().isEmpty()) {
            show_failure("Cuvette provenance ID is required.");
            return;
        }
        catalog.beginGroup(name);
        catalog.setValue("path_m", cuvette_path_->text());
        catalog.setValue("path_uncertainty_m", cuvette_path_uncertainty_->text());
        catalog.setValue("index", cuvette_index_->text());
        catalog.setValue("index_uncertainty", cuvette_index_uncertainty_->text());
        catalog.setValue("provenance_id", sample_cuvette_provenance_->text().trimmed());
        catalog.endGroup();
        catalog.sync();
        if (catalog.status() != QSettings::NoError) {
            show_failure("Could not save cuvette type.");
            return;
        }
        types->addItem(name);
    });
    provenance_ = field("Calibration provenance ID", "Required for all entered quantities");
    laser_id_ = field("Laser characterization ID", "Required");
    correction_ = field("Cuvette pair correction factor", "Required");
    correction_uncertainty_ = field("Pair correction uncertainty", "Required");
    start_z_ = field("Start z (m)", "Required; check stage travel");
    end_z_ = field("End z (m)", "Required; check stage travel");
    positions_ = new QSpinBox(setup);
    positions_->setObjectName("Positions");
    positions_->setRange(2, 1001);
    positions_->setValue(101);
    readings_ = new QSpinBox(setup);
    readings_->setObjectName("Accepted readings / position");
    readings_->setRange(2, 1000);
    readings_->setValue(100);
    setup_form->addRow(QStringLiteral("Positions"), positions_);
    setup_form->addRow(QStringLiteral("Accepted readings / position"), readings_);
    mock_mode_ = new QCheckBox(QStringLiteral("Mock mode — no hardware will be contacted"), setup);
    mock_mode_->setChecked(true);
    setup_form->addRow(mock_mode_);
    setup_check_
        = new QCheckBox(QStringLiteral("I reviewed the session and acquisition settings"), setup);
    setup_form->addRow(setup_check_);
    content->addWidget(setup);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose new session directory"));
        if (!directory.isEmpty())
            session_root_->setText(directory);
    });

    auto* preflight = new QGroupBox(QStringLiteral("Preflight checklist"), this);
    auto* preflight_layout = new QVBoxLayout(preflight);
    preflight_stage_ = new QCheckBox(
        QStringLiteral("ESP302 configuration and stage wiring verified"), preflight);
    preflight_daq_
        = new QCheckBox(QStringLiteral("NI-DAQmx channels configured and available"), preflight);
    preflight_trigger_
        = new QCheckBox(QStringLiteral("Trigger source and sample timing verified"), preflight);
    preflight_layout->addWidget(preflight_stage_);
    preflight_layout->addWidget(preflight_daq_);
    preflight_layout->addWidget(preflight_trigger_);
    content->addWidget(preflight);

    auto* preparation = new QGroupBox(QStringLiteral("Operator confirmations"), this);
    auto* preparation_layout = new QVBoxLayout(preparation);
    operator_ready_ = new QCheckBox(
        QStringLiteral("The laser is blocked and the dark path is ready"), preparation);
    energy_ready_ = new QCheckBox(
        QStringLiteral("Energy readings are entered/imported with provenance"), preparation);
    energy_readings_ = new QLineEdit(preparation);
    energy_readings_->setObjectName("Energy readings (J)");
    energy_readings_->setPlaceholderText(
        QStringLiteral("At least two energy readings in J, comma-separated"));
    sample_ready_ = new QCheckBox(
        QStringLiteral("The correct sample is installed and the beam path is ready"), preparation);
    preparation_layout->addWidget(operator_ready_);
    preparation_layout->addWidget(energy_ready_);
    preparation_layout->addWidget(energy_readings_);
    preparation_layout->addWidget(sample_ready_);
    content->addWidget(preparation);

    progress_ = new QLabel(this);
    content->addWidget(progress_);
    content->addStretch();

    auto* buttons = new QHBoxLayout;
    action_ = new QPushButton(this);
    cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
    restart_ = new QPushButton(QStringLiteral("Restart with new ID"), this);
    emergency_stop_ = new QPushButton(QStringLiteral("EMERGENCY STOP"), this);
    emergency_stop_->setStyleSheet(
        QStringLiteral("color: white; background: #a00000; font-weight: bold;"));
    buttons->addWidget(action_);
    buttons->addStretch();
    buttons->addWidget(restart_);
    buttons->addWidget(cancel_);
    buttons->addWidget(emergency_stop_);
    scroll->setWidget(page);
    auto* right = new QVBoxLayout;
    right->addWidget(scroll, 1);
    right->addLayout(buttons);
    root->addLayout(right, 1);

    connect(action_, &QPushButton::clicked, this, &AcquisitionWizard::advance);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (worker_)
            worker_->cancel();
        controller_.cancel();
        refresh();
    });
    connect(restart_, &QPushButton::clicked, this, [this] {
        if (worker_thread_ && worker_thread_->isRunning())
            return;
        controller_.restart();
        session_.reset();
        blank_complete_ = false;
        session_root_->clear();
        setup_check_->setChecked(false);
        preflight_stage_->setChecked(false);
        preflight_daq_->setChecked(false);
        preflight_trigger_->setChecked(false);
        operator_ready_->setChecked(false);
        energy_ready_->setChecked(false);
        sample_ready_->setChecked(false);
        energy_readings_->clear();
        refresh();
    });
    connect(emergency_stop_, &QPushButton::clicked, this, [this] {
        if (worker_)
            worker_->emergency_stop();
        controller_.emergency_stop();
        refresh();
    });
    refresh();
}

AcquisitionWizard::~AcquisitionWizard()
{
    if (worker_)
        worker_->cancel();
    if (worker_thread_ && worker_thread_->isRunning())
        worker_thread_->wait();
}

void AcquisitionWizard::show_failure(const QString& message)
{
    QMessageBox::warning(this, QStringLiteral("Cannot continue"), message);
}

void AcquisitionWizard::advance()
{
    bool ok = false;
    switch (controller_.state()) {
    case acquisition::State::Idle:
        controller_.configure(static_cast<std::size_t>(positions_->value()),
            static_cast<std::size_t>(readings_->value()));
        ok = true;
        break;
    case acquisition::State::Setup:
        ok = setup_check_->isChecked() && !session_name_->text().trimmed().isEmpty()
            && !session_root_->text().trimmed().isEmpty() && !sample_id_->text().trimmed().isEmpty()
            && !solvent_id_->text().trimmed().isEmpty();
        if (ok) {
            bool wavelength_ok = false;
            const auto wavelength = wavelength_nm_->text().toDouble(&wavelength_ok);
            ok = wavelength_ok && wavelength == 532.0 && solvent_type_->currentIndex() > 0
                && window_type_->currentIndex() > 0;
            const auto solvent = std::find_if(
                catalog_.materials.begin(), catalog_.materials.end(), [this](const auto& m) {
                    return m.material_id == solvent_type_->currentData().toString().toStdString();
                });
            const auto window = std::find_if(
                catalog_.materials.begin(), catalog_.materials.end(), [this](const auto& m) {
                    return m.material_id == window_type_->currentData().toString().toStdString();
                });
            if (ok
                && (solvent == catalog_.materials.end() || window == catalog_.materials.end()
                    || solvent->status == "draft" || window->status == "draft")) {
                show_failure(
                    QStringLiteral("Draft material entries cannot be used for production "
                                   "acquisition. Add characterized entries with provenance."));
                return;
            }
            if (ok) {
                const auto provenance = provenance_->text().trimmed().toStdString();
                auto numeric = [&](QLineEdit* field) -> double {
                    bool valid = false;
                    const double value = field->text().toDouble(&valid);
                    if (!valid || !std::isfinite(value) || value <= 0)
                        ok = false;
                    return value;
                };
                auto scalar = [&](QLineEdit* value, QLineEdit* uncertainty) {
                    return schema::ScalarWithUncertainty { numeric(value), numeric(uncertainty),
                        provenance, provenance };
                };
                schema::SampleSnapshot sample;
                sample.sample_id = sample_id_->text().trimmed().toStdString();
                sample.solvent_id = solvent_id_->text().trimmed().toStdString();
                sample.concentration_mol_per_l = scalar(concentration_, concentration_uncertainty_);
                sample.solvent_refractive_index = { solvent->refractive_index,
                    solvent->refractive_index * solvent->relative_standard_uncertainty,
                    solvent->provenance_id, solvent->provenance_id };
                const auto now = schema::UtcTimestamp::parse(
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString());
                if (!now) {
                    show_failure("Cannot create UTC timestamp");
                    return;
                }
                sample.created_at = *now;
                schema::InstrumentSnapshot instrument;
                auto id = schema::binary::random_uuid_v4();
                if (!id) {
                    show_failure("Cannot generate snapshot ID");
                    return;
                }
                instrument.snapshot_id = *id;
                instrument.created_at = *now;
                instrument.source_catalog_id = "user-catalog";
                instrument.source_catalog_revision = "1";
                instrument.laser_characterization_id = laser_id_->text().trimmed().toStdString();
                instrument.wavelength_m = wavelength * 1e-9;
                instrument.pulse_fwhm_s = 9e-9;
                instrument.waist_m = scalar(waist_, waist_uncertainty_);
                instrument.m2 = scalar(m2_, m2_uncertainty_);
                instrument.energy_meter_calibration_id = meter_id_->text().trimmed().toStdString();
                instrument.energy_meter_standard_uncertainty_j = numeric(meter_uncertainty_);
                instrument.energy_meter_uncertainty_provenance_id = provenance;
                instrument.entrance_window_refractive_index = { window->refractive_index,
                    window->refractive_index * window->relative_standard_uncertainty,
                    window->provenance_id, window->provenance_id };
                auto cuvette_path = scalar(cuvette_path_, cuvette_path_uncertainty_);
                auto cuvette_index = scalar(cuvette_index_, cuvette_index_uncertainty_);
                auto blank_path = scalar(blank_path_, blank_path_uncertainty_);
                auto blank_index = scalar(blank_index_, blank_index_uncertainty_);
                const auto sample_source
                    = sample_cuvette_provenance_->text().trimmed().toStdString();
                const auto blank_source = blank_cuvette_provenance_->text().trimmed().toStdString();
                if (sample_source.empty() || blank_source.empty())
                    ok = false;
                for (auto* input : { &cuvette_path, &cuvette_index }) {
                    input->provenance_id = sample_source;
                    input->uncertainty_evaluation_id = sample_source;
                }
                for (auto* input : { &blank_path, &blank_index }) {
                    input->provenance_id = blank_source;
                    input->uncertainty_evaluation_id = blank_source;
                }
                instrument.cuvettes = { { sample_cuvette_id_->text().trimmed().toStdString(),
                                            cuvette_path, cuvette_index },
                    { blank_cuvette_id_->text().trimmed().toStdString(), blank_path,
                        blank_index } };
                instrument.cuvette_pairs = { { "pair-1", instrument.cuvettes[0].cuvette_id,
                    instrument.cuvettes[1].cuvette_id, "scan-1",
                    scalar(correction_, correction_uncertainty_) } };
                schema::DaqConfiguration daq;
                daq.configuration_id = "daq-1";
                daq.device_id = hardware_configuration_.daq_device;
                daq.trigger_terminal_index = 0;
                daq.channels
                    = { schema::DaqChannel { 0, schema::DetectorRole::Reference,
                            schema::TerminalConfiguration::Rse, schema::Coupling::Dc, -10, 10 },
                          schema::DaqChannel { 1, schema::DetectorRole::Signal,
                              schema::TerminalConfiguration::Rse, schema::Coupling::Dc, -10, 10 } };
                instrument.daq_configurations = { daq };
                instrument.optical_configurations = { { "optical-1", { }, { } } };
                instrument.stage_configurations
                    = { { "stage-1", "esp302", 3, 1e-6, provenance, 2 } };
                instrument.scan_configurations = { { "scan-1", "daq-1", "optical-1", "stage-1" } };
                bool start_ok = false, end_ok = false;
                const double start = start_z_->text().toDouble(&start_ok);
                const double end = end_z_->text().toDouble(&end_ok);
                if (!start_ok || !end_ok || !std::isfinite(start) || !std::isfinite(end))
                    ok = false;
                if (start >= end || positions_->value() < 2
                    || std::uint64_t(positions_->value()) * readings_->value() > 100000)
                    ok = false;
                if (!ok || provenance.empty())
                    break;
                const auto created = acquisition::create_session(
                    session_root_->text().trimmed().toStdString(), sample, instrument,
                    session_name_->text().trimmed().toStdString());
                if (!created) {
                    show_failure(QString::fromStdString(created.error().message));
                    return;
                }
                session_ = *created;
                ok = controller_.complete_setup(true);
            }
        }
        break;
    case acquisition::State::Preflight:
        if (!session_ || !mock_mode_->isChecked()) {
            show_failure(QStringLiteral(
                "Only mock preflight is available. Real hardware checks have not been verified."));
            return;
        }
        ok = preflight_stage_->isChecked() && preflight_daq_->isChecked()
            && preflight_trigger_->isChecked();
        if (ok) {
            hardware::MockStage stage;
            stage.responses.push_back(hardware::StagePosition { 0, 0 });
            hardware::MockDetector detector;
            detector.responses.push_back(hardware::DetectorCapture { 1, 0.8 });
            ok = stage.move_and_readback(0).has_value() && detector.capture_one().has_value();
            if (ok)
                ok = controller_.complete_preflight(true);
        }
        break;
    case acquisition::State::OperatorPreparation:
        ok = operator_ready_->isChecked() && controller_.confirm_operator_preparation();
        break;
    case acquisition::State::DarkAcquisition:
        if (!session_ || !mock_mode_->isChecked())
            return;
        {
            hardware::MockDetector detector;
            const auto count = static_cast<std::uint32_t>(readings_->value());
            for (std::uint32_t i = 0; i < count; ++i)
                detector.responses.push_back(hardware::DetectorCapture { 0.01, 0.02 });
            const auto id = schema::binary::random_uuid_v4();
            if (!id) {
                show_failure("Cannot generate dark ID");
                return;
            }
            schema::CaptureSettings capture { 1e6, 10, 140, 1, 1, count, 4 };
            acquisition::DarkRunner runner(
                { capture, -10, 10, -10, 10 }, session_->session_id, *id, detector);
            const auto result = runner.run();
            if (!result.error.empty()) {
                controller_.fail(result.error);
                break;
            }
            auto saved = acquisition::publish_dark(session_root_->text().toStdString(), result.dark,
                { true, true, true, true }, "daq-1");
            if (!saved) {
                controller_.fail(saved.error().message);
                break;
            }
            session_ = *saved;
            ok = controller_.complete_dark();
        }
        break;
    case acquisition::State::EnergyCalibration:
        if (energy_ready_->isChecked() && session_) {
            std::vector<double> values;
            for (const auto& token : energy_readings_->text().split(',')) {
                bool parsed = false;
                const double value = token.trimmed().toDouble(&parsed);
                if (!parsed || !std::isfinite(value) || value <= 0) {
                    values.clear();
                    break;
                }
                values.push_back(value);
            }
            if (values.size() >= 2) {
                const auto id = schema::binary::random_uuid_v4();
                const auto now = schema::UtcTimestamp::parse(
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString());
                if (!id || !now) {
                    show_failure("Cannot create energy identity");
                    return;
                }
                schema::binary::EnergyFile energy {
                    { schema::binary::FileKind::Energy, *id, session_->session_id, *now }, values
                };
                auto saved
                    = acquisition::publish_energy(session_root_->text().toStdString(), energy);
                if (!saved) {
                    controller_.fail(saved.error().message);
                    break;
                }
                session_ = *saved;
                ok = controller_.complete_energy(true);
            }
        }
        break;
    case acquisition::State::SamplePreparation:
        ok = sample_ready_->isChecked() && controller_.confirm_sample_preparation();
        if (ok) {
            start_sample_worker();
            refresh();
            return;
        }
        break;
    case acquisition::State::SampleAcquisition:
        // The real stage/DAQ worker will call record_accepted_reading(),
        // complete_position(), and complete_sample().
        show_failure(QStringLiteral("Mock scan is running. Wait for completion or cancel."));
        return;
    case acquisition::State::Review:
        if (session_ && session_->scans.size() == 2 && session_->darks.size() == 2
            && session_->energy_bursts.size() == 1 && session_->curves.size() == 1)
            ok = controller_.publish();
        break;
    default:
        return;
    }
    if (!ok && controller_.state() != acquisition::State::Idle)
        show_failure(QStringLiteral("Complete the required checklist items before continuing."));
    refresh();
}

void AcquisitionWizard::start_sample_worker()
{
    if (!session_ || !mock_mode_->isChecked() || (worker_thread_ && worker_thread_->isRunning())) {
        controller_.fail("Mock session is not ready or a worker is already running");
        return;
    }
    const bool blank = !blank_complete_;
    const auto id = blank ? schema::binary::random_uuid_v4()
                          : schema::Uuid::parse(controller_.acquisition_id());
    if (!id) {
        controller_.fail("Could not create acquisition ID");
        return;
    }
    bool start_ok = false, end_ok = false;
    const double start = start_z_->text().toDouble(&start_ok);
    const double end = end_z_->text().toDouble(&end_ok);
    if (!start_ok || !end_ok || !std::isfinite(start) || !std::isfinite(end) || start >= end) {
        controller_.fail("Invalid stage grid");
        return;
    }
    acquisition::RunConfiguration config;
    config.session_id = session_->session_id;
    config.acquisition_id = *id;
    config.kind = blank ? schema::ScanKind::Blank : schema::ScanKind::Sample;
    config.session_root = session_root_->text().toStdString();
    const auto count = static_cast<std::uint32_t>(positions_->value());
    const double spacing = (end - start) / (count - 1);
    config.grid = { start, end, count, spacing };
    for (std::uint32_t i = 0; i < count; ++i)
        config.positions_m.push_back(i + 1 == count ? end : start + i * spacing);
    config.capture = { 1e6, 10, 140, 1, 1, static_cast<std::uint32_t>(readings_->value()), 4 };
    config.maximum_move_attempts = 2;
    config.maximum_readback_error_m = 1e-6;
    config.ai0_minimum_v = config.ai1_minimum_v = -10;
    config.ai0_maximum_v = config.ai1_maximum_v = 10;
    worker_thread_ = new QThread(this);
    worker_ = new acquisition::Worker(std::move(config), hardware_configuration_,
        blank ? blank_cuvette_id_->text().trimmed().toStdString()
              : sample_cuvette_id_->text().trimmed().toStdString(),
        true);
    worker_->moveToThread(worker_thread_);
    connect(worker_thread_, &QThread::started, worker_, &acquisition::Worker::run);
    connect(worker_, &acquisition::Worker::progress, this, &AcquisitionWizard::worker_progress);
    connect(worker_, &acquisition::Worker::finished, this, &AcquisitionWizard::worker_finished);
    connect(worker_, &acquisition::Worker::finished, worker_thread_, &QThread::quit,
        Qt::DirectConnection);
    connect(worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_thread_, &QThread::finished, this, [this] {
        worker_ = nullptr;
        worker_thread_ = nullptr;
        refresh();
        if (blank_complete_ && session_ && session_->scans.size() == 1
            && controller_.state() == acquisition::State::SampleAcquisition)
            start_sample_worker();
    });
    connect(worker_thread_, &QThread::finished, worker_thread_, &QObject::deleteLater);
    worker_thread_->start();
}

void AcquisitionWizard::worker_progress(std::size_t position, std::size_t positions,
    std::size_t accepted, std::size_t required, std::size_t rejected)
{
    progress_->setText(
        QStringLiteral("Position: %1 / %2   Accepted readings: %3 / %4   Rejected: %5")
            .arg(position + 1)
            .arg(positions)
            .arg(accepted)
            .arg(required)
            .arg(rejected));
}

void AcquisitionWizard::worker_finished(bool complete, const QString& message)
{
    if (complete && controller_.state() == acquisition::State::SampleAcquisition) {
        auto latest = schema::storage::read_session_manifest(session_root_->text().toStdString());
        if (!latest)
            controller_.fail(latest.error().message);
        else {
            session_ = *latest;
            if (!blank_complete_) {
                blank_complete_ = true;
                instruction_->setText(QStringLiteral("Blank complete. Starting sample scan…"));
                if (!worker_thread_)
                    start_sample_worker();
            } else {
                hardware::MockDetector detector;
                const auto count = static_cast<std::uint32_t>(readings_->value());
                for (std::uint32_t i = 0; i < count; ++i)
                    detector.responses.push_back(hardware::DetectorCapture { 0.01, 0.02 });
                auto id = schema::binary::random_uuid_v4();
                if (!id) {
                    controller_.fail("Cannot create final dark ID");
                    refresh();
                    return;
                }
                schema::CaptureSettings capture { 1e6, 10, 140, 1, 1, count, 4 };
                acquisition::DarkRunner runner(
                    { capture, -10, 10, -10, 10 }, session_->session_id, *id, detector);
                auto dark = runner.run();
                if (!dark.error.empty()) {
                    controller_.fail(dark.error);
                    refresh();
                    return;
                }
                auto saved = acquisition::publish_dark(session_root_->text().toStdString(),
                    dark.dark, { true, true, true, true }, "daq-1");
                if (!saved) {
                    controller_.fail(saved.error().message);
                    refresh();
                    return;
                }
                session_ = *saved;
                if (session_->scans.size() != 2 || session_->energy_bursts.size() != 1) {
                    controller_.fail("Missing blank, sample, or energy");
                    refresh();
                    return;
                }
                auto curve = acquisition::publish_curve(session_root_->text().toStdString(),
                    session_->scans.back().acquisition_id, session_->scans.front().acquisition_id,
                    session_->energy_bursts.front().burst_id, "pair-1");
                if (!curve) {
                    controller_.fail(curve.error().message);
                    refresh();
                    return;
                }
                session_ = *curve;
                controller_.complete_sample();
            }
        }
    } else if (!complete && controller_.state() == acquisition::State::SampleAcquisition)
        controller_.fail(message.isEmpty()
                ? QStringLiteral("sample acquisition failed").toStdString()
                : message.toStdString());
    refresh();
    if (!complete)
        if (controller_.state() == acquisition::State::Failed)
            show_failure(message.isEmpty() ? QStringLiteral("Sample acquisition failed") : message);
}

void AcquisitionWizard::refresh()
{
    const auto state = controller_.state();
    setup_group_->setEnabled(
        state == acquisition::State::Idle || state == acquisition::State::Setup);
    energy_readings_->setEnabled(state == acquisition::State::EnergyCalibration);
    state_label_->setText(QStringLiteral("Current step: %1").arg(state_name(state)));
    instruction_->setText(QString::fromStdString(controller_.message()));
    progress_->setText(
        QStringLiteral("Acquisition ID: %1\nPosition: %2 / %3   Accepted readings: %4 / %5")
            .arg(QString::fromStdString(controller_.acquisition_id()))
            .arg(controller_.current_position())
            .arg(controller_.position_count())
            .arg(controller_.accepted_readings())
            .arg(controller_.required_readings()));
    const int selected = step_index(state);
    if (selected >= 0)
        steps_->setCurrentRow(selected);
    action_->setEnabled(state != acquisition::State::SampleAcquisition
        && state != acquisition::State::Published && state != acquisition::State::Cancelled
        && state != acquisition::State::EmergencyStopped && state != acquisition::State::Failed);
    switch (state) {
    case acquisition::State::Idle:
        action_->setText("Begin guided acquisition");
        break;
    case acquisition::State::Setup:
        action_->setText("Validate settings");
        break;
    case acquisition::State::Preflight:
        action_->setText("Complete preflight");
        break;
    case acquisition::State::OperatorPreparation:
        action_->setText("Begin dark acquisition");
        break;
    case acquisition::State::DarkAcquisition:
        action_->setText("Continue to energy calibration");
        break;
    case acquisition::State::EnergyCalibration:
        action_->setText("Continue to sample preparation");
        break;
    case acquisition::State::SamplePreparation:
        action_->setText("Begin sample acquisition");
        break;
    case acquisition::State::Review:
        action_->setText("Publish acquisition");
        break;
    default:
        action_->setText("Continue");
        break;
    }
    cancel_->setEnabled(
        state != acquisition::State::Idle && state != acquisition::State::Published);
    restart_->setVisible(state == acquisition::State::Failed
        || state == acquisition::State::Cancelled || state == acquisition::State::EmergencyStopped);
    restart_->setEnabled(!worker_thread_ || !worker_thread_->isRunning());
    emergency_stop_->setEnabled(state == acquisition::State::DarkAcquisition
        || state == acquisition::State::SampleAcquisition);
}

} // namespace zscan::app
