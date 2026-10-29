#include "curve_plot.hpp"

#include <QLineSeries>
#include <QQuickWidget>
#include <QScatterSeries>
#include <QUrl>
#include <QVBoxLayout>

namespace zscan::app {

class CurvePlot::Private {
public:
    QQuickWidget* view { };
    QList<QAbstractSeries*> series;
};

CurvePlot::CurvePlot(QWidget* parent)
    : QWidget(parent)
    , d_(new Private)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    d_->view = new QQuickWidget(this);
    d_->view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    d_->view->setInitialProperties(
        { { QStringLiteral("externalSeries"), QVariant::fromValue(d_->series) } });
    d_->view->setSource(QUrl(QStringLiteral("qrc:/qml/CurvePlot.qml")));
    layout->addWidget(d_->view);
}

void CurvePlot::setData(
    std::vector<double> z, std::vector<double> transmission, std::vector<double> errors)
{
    setSeries({ Series { QStringLiteral("Experimental"), std::move(z), std::move(transmission),
        std::move(errors), { } } });
}

void CurvePlot::setPrediction(std::vector<double> prediction)
{
    if (!d_->series.isEmpty()) {
        if (auto* line = qobject_cast<QLineSeries*>(d_->series.last()))
            line->replace(QList<QPointF> { });
    }
    Q_UNUSED(prediction)
}

void CurvePlot::setSeries(std::vector<Series> series)
{
    qDeleteAll(d_->series);
    d_->series.clear();
    double xmin = 0, xmax = 1, ymin = 0, ymax = 1;
    bool first = true;
    for (std::size_t s = 0; s < series.size(); ++s) {
        const auto& item = series[s];
        if (item.z.empty())
            continue;
        auto* experimental = new QScatterSeries(d_->view);
        experimental->setName(item.label + QStringLiteral(" experimental"));
        auto* fitted = new QLineSeries(d_->view);
        fitted->setName(item.label + QStringLiteral(" fitted"));
        const QColor colors[] = { QColor(30, 100, 200), QColor(190, 80, 30), QColor(40, 140, 70),
            QColor(130, 70, 170) };
        const QColor color = colors[s % 4];
        experimental->setColor(color);
        fitted->setColor(color);
        QList<QPointF> observed_points;
        QList<QPointF> fitted_points;
        for (std::size_t i = 0; i < item.z.size(); ++i) {
            const double z_mm = item.z[i] * 1000.0;
            observed_points.append(QPointF(z_mm, item.observed[i]));
            if (i < item.predicted.size())
                fitted_points.append(QPointF(z_mm, item.predicted[i]));
            const double error = i < item.errors.size() ? item.errors[i] : 0.0;
            const double low = item.observed[i] - error;
            const double high = item.observed[i] + error;
            const double predicted
                = i < item.predicted.size() ? item.predicted[i] : item.observed[i];
            if (first) {
                xmin = z_mm;
                xmax = z_mm;
                ymin = std::min(low, predicted);
                ymax = std::max(high, predicted);
                first = false;
            } else {
                xmin = std::min(xmin, z_mm);
                xmax = std::max(xmax, z_mm);
                ymin = std::min(ymin, std::min(low, predicted));
                ymax = std::max(ymax, std::max(high, predicted));
            }
        }
        experimental->replace(observed_points);
        fitted->replace(fitted_points);
        d_->series.append(experimental);
        d_->series.append(fitted);
    }
    const double xpad = std::max((xmax - xmin) * 0.05, 0.01);
    const double ypad = std::max((ymax - ymin) * 0.08, 0.02);
    d_->view->setInitialProperties({
        { QStringLiteral("externalSeries"), QVariant::fromValue(d_->series) },
        { QStringLiteral("xMin"), xmin - xpad },
        { QStringLiteral("xMax"), xmax + xpad },
        { QStringLiteral("yMin"), ymin - ypad },
        { QStringLiteral("yMax"), ymax + ypad },
    });
    d_->view->setSource(QUrl(QStringLiteral("qrc:/qml/CurvePlot.qml")));
}

} // namespace zscan::app
