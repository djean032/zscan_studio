import QtQuick
import QtGraphs

GraphsView {
    property var externalSeries
    property real xMin: -1
    property real xMax: 1
    property real yMin: 0
    property real yMax: 1
    Component { id: marker; Rectangle { property color pointColor; width: 6; height: 6; radius: 3; color: pointColor } }
    axisX: ValueAxis { titleText: "z (mm)"; min: xMin; max: xMax }
    axisY: ValueAxis { titleText: "Transmission"; min: yMin; max: yMax }
    seriesList: externalSeries
    marginLeft: 72
    marginBottom: 58
    marginTop: 32
    marginRight: 32
    dynamicLabelMargins: true
    Component.onCompleted: {
        for (var i = 0; i < externalSeries.length; ++i)
            if (externalSeries[i].name.indexOf("experimental") >= 0
                && externalSeries[i].pointDelegate !== undefined)
                externalSeries[i].pointDelegate = marker
    }
}
