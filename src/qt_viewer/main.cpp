/**
 * @file main.cpp
 * @brief Qt-based CAD viewer entry point.
 *
 * Usage:
 *   cad-viewer                    → Open empty, use File → Open
 *   cad-viewer drawing.dxf        → Open DXF directly
 *   cad-viewer drawing.dwg        → Open DWG directly
 */
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>
#include "CadViewer.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CAD Viewer");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("cad-parser");

    QCommandLineParser parser;
    parser.setApplicationDescription("CAD Viewer");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption screenshotOption(
        "screenshot", "Render the drawing canvas to a PNG and exit.", "file");
    QCommandLineOption screenshotSizeOption(
        "screenshot-size", "PNG size as WIDTHxHEIGHT.", "size", "1015x653");
    parser.addOption(screenshotOption);
    parser.addOption(screenshotSizeOption);
    parser.addPositionalArgument("file", "DWG or DXF file to open.");
    parser.process(app);

    CadViewerWindow window;
    const QStringList files = parser.positionalArguments();
    const QString filepath = files.isEmpty() ? QString() : files.first();
    if (!filepath.isEmpty() && QFileInfo::exists(filepath)) {
        window.loadFile(filepath);
    }

    const QString screenshot = parser.value(screenshotOption);
    if (!screenshot.isEmpty()) {
        const QStringList sizeParts = parser.value(screenshotSizeOption).toLower().split('x');
        bool widthOk = false;
        bool heightOk = false;
        const int width = sizeParts.value(0).toInt(&widthOk);
        const int height = sizeParts.value(1).toInt(&heightOk);
        const QSize size = widthOk && heightOk ? QSize(width, height) : QSize(1015, 653);
        QTimer::singleShot(100, &app, [&]() {
            app.exit(window.renderToImage(screenshot, size) ? 0 : 1);
        });
    } else {
        window.show();
    }

    return app.exec();
}
