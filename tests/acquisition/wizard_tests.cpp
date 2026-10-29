#include "src/app/acquisition_wizard.hpp"

#include "zscan/schema/materials.hpp"
#include "zscan/schema/session_validator.hpp"
#include "zscan/schema/storage.hpp"
#include "zscan/hardware/esp302.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest/QTest>

#include <future>
#include <thread>

class WizardTests : public QObject {
    Q_OBJECT
private slots:
    void mockWorkflow();
    void esp302ProtocolWithoutHardware();
};

void WizardTests::esp302ProtocolWithoutHardware()
{
    std::promise<quint16> port;
    auto future = port.get_future();
    QByteArray received;
    std::jthread server([&] {
        QTcpServer listener;
        const bool listening = listener.listen(QHostAddress::LocalHost, 0);
        port.set_value(listening ? listener.serverPort() : 0);
        if (!listening || !listener.waitForNewConnection(5000)) return;
        auto* client = listener.nextPendingConnection();
        while (!received.contains("3TP?\r")) {
            if (!client->waitForReadyRead(5000)) return;
            received += client->readAll();
        }
        client->write("1.000000\r\n");
        client->waitForBytesWritten(5000);
    });
    const auto local_port = future.get();
    QVERIFY(local_port != 0);
    zscan::hardware::Esp302Stage stage("127.0.0.1", local_port);
    auto position = stage.move_and_readback(0.001);
    QVERIFY(position);
    QCOMPARE(position->readback_z_m, 0.001);
    server.join();
    QVERIFY(received.contains("3PA1\r"));
    QVERIFY(received.contains("3WS\r"));
    QVERIFY(received.contains("3TP?\r"));
}

void WizardTests::mockWorkflow()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    qputenv("XDG_DATA_HOME", parent.path().toUtf8());
    const auto user_file
        = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/materials.toml";
    QDir().mkpath(QFileInfo(user_file).absolutePath());
    zscan::schema::MaterialCatalog catalog { 1, 532e-9,
        { { "test-solvent", "Test solvent", zscan::schema::MaterialKind::Solvent, 1.34, 0.01,
              "characterized", "test-source" },
            { "test-window", "Test window", zscan::schema::MaterialKind::EntranceWindow, 1.52, 0.01,
                "characterized", "test-source" } } };
    QVERIFY(zscan::schema::save_material_catalog(user_file.toStdString(), catalog));
    const auto root = parent.path() + "/new-session";
    zscan::app::AcquisitionWizard wizard;
    QStringList dialogs;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QMessageBox*>(widget); dialog && dialog->isVisible()) {
                dialogs << dialog->text();
                dialog->accept();
            }
    });
    dismiss.start(10);
    auto set = [&](const char* name, const char* value) {
        auto* edit = wizard.findChild<QLineEdit*>(name);
        QVERIFY(edit);
        edit->setText(value);
    };
    auto click = [&](const QString& label) {
        for (auto* button : wizard.findChildren<QPushButton*>())
            if (button->text() == label) {
                button->click();
                return true;
            }
        return false;
    };
    QVERIFY(click("Begin guided acquisition"));
    set("Session name", "test");
    wizard.findChild<QLineEdit*>("New session directory")->setText(root);
    set("Sample ID", "test-sample");
    set("Solvent ID", "test-solvent-batch");
    auto* solvent = wizard.findChild<QComboBox*>("Solvent type");
    auto* window = wizard.findChild<QComboBox*>("Entrance window");
    solvent->setCurrentIndex(solvent->findData("test-solvent"));
    window->setCurrentIndex(window->findData("test-window"));
    set("Concentration (mol/L)", "0.00001");
    set("Concentration standard uncertainty (mol/L)", "0.0000001");
    set("Beam waist (m)", "0.000014");
    set("Beam waist standard uncertainty (m)", "0.0000001");
    set("M²", "1.2");
    set("M² standard uncertainty", "0.03");
    set("Energy meter calibration ID", "meter-test");
    set("Energy meter standard uncertainty (J)", "0.000000001");
    set("Sample cuvette ID", "sample-cuvette");
    set("Blank cuvette ID", "blank-cuvette");
    set("Cuvette path length (m)", "0.001");
    set("Cuvette path length uncertainty (m)", "0.00001");
    set("Cuvette refractive index", "1.52");
    set("Cuvette index uncertainty", "0.01");
    set("Blank cuvette path length (m)", "0.001");
    set("Blank path length uncertainty (m)", "0.00001");
    set("Blank cuvette refractive index", "1.52");
    set("Blank cuvette index uncertainty", "0.01");
    set("Sample cuvette provenance ID", "sample-cuvette-report");
    set("Blank cuvette provenance ID", "blank-cuvette-report");
    set("Calibration provenance ID", "test-provenance");
    set("Laser characterization ID", "test-laser");
    set("Cuvette pair correction factor", "1.05");
    set("Pair correction uncertainty", "0.03");
    set("Start z (m)", "-0.001");
    set("End z (m)", "0.001");
    wizard.findChild<QSpinBox*>("Positions")->setValue(2);
    wizard.findChild<QSpinBox*>("Accepted readings / position")->setValue(2);
    for (auto* box : wizard.findChildren<QCheckBox*>())
        box->setChecked(true);
    QVERIFY(click("Validate settings"));
    QVERIFY2(dialogs.isEmpty(), qPrintable(dialogs.join("; ")));
    QVERIFY(QFileInfo::exists(root + "/session.toml"));
    QVERIFY(zscan::schema::storage::read_session_manifest(root.toStdString())
                ->extensions_toml.find("name") != std::string::npos);
    for (auto* box : wizard.findChildren<QCheckBox*>())
        if (box->text().contains("Mock mode"))
            box->setChecked(false);
    QVERIFY(click("Complete preflight"));
    QVERIFY(dialogs.join(" ").contains("Only mock preflight is available"));
    dialogs.clear();
    for (auto* box : wizard.findChildren<QCheckBox*>())
        if (box->text().contains("Mock mode"))
            box->setChecked(true);
    QVERIFY(click("Complete preflight"));
    QVERIFY(click("Begin dark acquisition"));
    QVERIFY(click("Continue to energy calibration"));
    set("Energy readings (J)", "5e-8, 5.2e-8");
    QVERIFY(click("Continue to sample preparation"));
    QVERIFY(click("Begin sample acquisition"));
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            auto manifest = zscan::schema::storage::read_session_manifest(root.toStdString());
            return manifest && manifest->curves.size() == 1;
        }(),
        10000);
    QVERIFY(click("Publish acquisition"));
    auto manifest = zscan::schema::storage::read_session_manifest(root.toStdString());
    QVERIFY(manifest);
    QCOMPARE(manifest->scans.size(), std::size_t(2));
    QCOMPARE(manifest->darks.size(), std::size_t(2));
    QCOMPARE(manifest->manifest_revision, std::uint64_t(7));
    auto production = zscan::schema::load_production_session(root.toStdString());
    if (!production)
        QFAIL(qPrintable(QString::fromStdString(
            production.error().location + ": " + production.error().message)));
}

QTEST_MAIN(WizardTests)
#include "wizard_tests.moc"
