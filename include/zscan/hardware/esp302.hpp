#pragma once

#include "zscan/hardware/interfaces.hpp"

#include <QHostAddress>
#include <QTcpSocket>

namespace zscan::hardware {

class Esp302Stage final : public StageController {
public:
    Esp302Stage(QString host = QStringLiteral("192.168.254.254"), quint16 port = 5001, int axis = 3,
        QObject* parent = nullptr);

    Result<StagePosition> move_and_readback(double commanded_z_m) override;

private:
    Result<QString> command(const QByteArray& command, bool expect_response = true);
    Result<double> read_position();

    QTcpSocket socket_;
    QString host_;
    quint16 port_;
    int axis_;
};

} // namespace zscan::hardware
