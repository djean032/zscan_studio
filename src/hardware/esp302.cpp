#include "zscan/hardware/esp302.hpp"

#include <QStringList>
#include <cmath>

namespace zscan::hardware {

Esp302Stage::Esp302Stage(QString host, quint16 port, int axis, QObject* parent)
    : StageController()
    , socket_(parent)
    , host_(std::move(host))
    , port_(port)
    , axis_(axis)
{
}

Result<QString> Esp302Stage::command(const QByteArray& command, bool expect_response)
{
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        socket_.connectToHost(host_, port_);
        if (!socket_.waitForConnected(2000))
            return std::unexpected(HardwareError { socket_.errorString().toStdString() });
    }
    QByteArray wire = command;
    if (!wire.endsWith('\r'))
        wire.append('\r');
    if (socket_.write(wire) != wire.size() || !socket_.waitForBytesWritten(1000))
        return std::unexpected(HardwareError { socket_.errorString().toStdString() });
    if (!expect_response)
        return QString { };
    if (!socket_.waitForReadyRead(20000))
        return std::unexpected(HardwareError { "ESP302 response timeout" });
    QByteArray response = socket_.readLine();
    while (!response.contains('\n') && socket_.waitForReadyRead(100))
        response += socket_.readLine();
    response.replace('\r', "");
    response.replace('\n', "");
    return QString::fromLatin1(response).trimmed();
}

Result<double> Esp302Stage::read_position()
{
    const auto response = command(QByteArray::number(axis_) + "TP?");
    if (!response)
        return std::unexpected(response.error());
    const auto parts = response->split(',');
    bool ok = false;
    const double value = parts.last().trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return std::unexpected(HardwareError { "invalid ESP302 position response" });
    // The ESP302 stage is configured in millimeters; the schema uses meters.
    return value * 1e-3;
}

Result<StagePosition> Esp302Stage::move_and_readback(double commanded_z_m)
{
    if (!std::isfinite(commanded_z_m))
        return std::unexpected(HardwareError { "nonfinite commanded stage position" });
    if (const auto move = command(
            QByteArray::number(axis_) + "PA" + QByteArray::number(commanded_z_m * 1e3, 'g', 17),
            false);
        !move)
        return std::unexpected(move.error());
    if (const auto wait = command(QByteArray::number(axis_) + "WS", false); !wait)
        return std::unexpected(wait.error());
    auto readback = read_position();
    if (!readback)
        return std::unexpected(readback.error());
    return StagePosition { commanded_z_m, *readback };
}

} // namespace zscan::hardware
