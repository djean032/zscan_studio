#include "session_window.hpp"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    zscan::app::SessionWindow window;
    window.show();
    return application.exec();
}
