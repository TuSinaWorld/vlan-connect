#include <QApplication>
#include <QIcon>
#include "ui/mainwindow.h"
#include "ui/style_manager.h"
#include "ui/log_manager.h"
#include "protocol.h"
#include "version.h"
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

bool VLan::g_verboseLog = false;

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    // Qt 5.9 rounds DPI scale factors to integers (1.5 -> 2), causing
    // the window to exceed screen bounds. Query the real DPI from Windows
    // and set the precise factor via QT_SCALE_FACTOR instead. app.manifest
    // must enable DPI awareness before main(), or Windows reports 96 DPI here.
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    if (dpi > 96) {
        qreal scale = qreal(dpi) / 96.0;
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 2));
    }
#else
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    app.setApplicationName("VLan");
    app.setApplicationVersion(VLAN_RELEASE_VERSION);
    app.setStyleSheet(VLan::StyleManager::getStyleSheet());
    app.setWindowIcon(QIcon(":/icons/vlan.ico"));

    VLan::LogManager::instance().installHandler();
    qInfo("%s", VLan::releaseBuildDescription("VLan GUI").c_str());

    VLan::MainWindow w;
    w.show();

    return app.exec();
}
