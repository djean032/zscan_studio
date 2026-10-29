#pragma once

#include "zscan/acquisition/controller.hpp"
#include "zscan/acquisition/worker.hpp"
#include "zscan/hardware/factory.hpp"
#include "zscan/schema/materials.hpp"

#include <QWidget>
#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QCheckBox;
class QThread;
class QGroupBox;
class QLineEdit;
class QComboBox;

namespace zscan::app {

class AcquisitionWizard final : public QWidget {
    Q_OBJECT
public:
    explicit AcquisitionWizard(QWidget* parent = nullptr);
    ~AcquisitionWizard() override;

private:
    void refresh();
    void advance();
    void show_failure(const QString& message);
    void start_sample_worker();
    void worker_progress(std::size_t position, std::size_t positions, std::size_t accepted,
        std::size_t required, std::size_t rejected);
    void worker_finished(bool complete, const QString& message);

    acquisition::Controller controller_;
    hardware::HardwareConfiguration hardware_configuration_;
    QThread* worker_thread_ { };
    acquisition::Worker* worker_ { };
    QGroupBox* setup_group_ { };
    QListWidget* steps_ { };
    QLabel* state_label_ { };
    QLabel* instruction_ { };
    QLabel* progress_ { };
    QCheckBox* setup_check_ { };
    QCheckBox* preflight_stage_ { };
    QCheckBox* preflight_daq_ { };
    QCheckBox* preflight_trigger_ { };
    QCheckBox* operator_ready_ { };
    QCheckBox* energy_ready_ { };
    QCheckBox* sample_ready_ { };
    QLineEdit* session_name_ { };
    QLineEdit* session_root_ { };
    QLineEdit* sample_id_ { };
    QLineEdit* solvent_id_ { };
    QLineEdit* wavelength_nm_ { };
    QComboBox* solvent_type_ { };
    QComboBox* window_type_ { };
    schema::MaterialCatalog catalog_;
    QLineEdit* concentration_ { };
    QLineEdit* concentration_uncertainty_ { };
    QLineEdit* waist_ { };
    QLineEdit* waist_uncertainty_ { };
    QLineEdit* m2_ { };
    QLineEdit* m2_uncertainty_ { };
    QLineEdit* meter_id_ { };
    QLineEdit* meter_uncertainty_ { };
    QLineEdit* sample_cuvette_id_ { };
    QLineEdit* blank_cuvette_id_ { };
    QLineEdit* cuvette_path_ { };
    QLineEdit* cuvette_path_uncertainty_ { };
    QLineEdit* cuvette_index_ { };
    QLineEdit* cuvette_index_uncertainty_ { };
    QLineEdit* blank_path_ { };
    QLineEdit* blank_path_uncertainty_ { };
    QLineEdit* blank_index_ { };
    QLineEdit* blank_index_uncertainty_ { };
    QLineEdit* sample_cuvette_provenance_ { };
    QLineEdit* blank_cuvette_provenance_ { };
    QLineEdit* provenance_ { };
    QLineEdit* laser_id_ { };
    QLineEdit* correction_ { };
    QLineEdit* correction_uncertainty_ { };
    QLineEdit* energy_readings_ { };
    QLineEdit* start_z_ { };
    QLineEdit* end_z_ { };
    QCheckBox* mock_mode_ { };
    bool blank_complete_ { false };
    std::optional<schema::SessionManifest> session_;
    QSpinBox* positions_ { };
    QSpinBox* readings_ { };
    QPushButton* action_ { };
    QPushButton* cancel_ { };
    QPushButton* restart_ { };
    QPushButton* emergency_stop_ { };
};

} // namespace zscan::app
