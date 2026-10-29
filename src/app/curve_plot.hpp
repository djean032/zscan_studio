#pragma once
#include <QWidget>
#include <vector>
namespace zscan::app {
class CurvePlot final : public QWidget {
    Q_OBJECT
public:
    struct Series {
        QString label;
        std::vector<double> z;
        std::vector<double> observed;
        std::vector<double> errors;
        std::vector<double> predicted;
    };
    explicit CurvePlot(QWidget* parent = nullptr);
    void setData(
        std::vector<double> z, std::vector<double> transmission, std::vector<double> errors);
    void setPrediction(std::vector<double> prediction);
    void setSeries(std::vector<Series> series);

private:
    class Private;
    Private* d_ { };
    std::vector<double> z_, transmission_, errors_, prediction_;
    std::vector<Series> series_;
};
}
