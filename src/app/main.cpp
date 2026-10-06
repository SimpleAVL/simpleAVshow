#include "app/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QFileInfo>
#include <QDir>
#include <QPalette>
#include <QFont>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion"); app.setFont(QFont("Arial", 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#192331")); palette.setColor(QPalette::WindowText, QColor("#dee7f3"));
    palette.setColor(QPalette::Base, QColor("#141d28")); palette.setColor(QPalette::AlternateBase, QColor("#202d3d"));
    palette.setColor(QPalette::Text, QColor("#dee7f3")); palette.setColor(QPalette::Button, QColor("#263448"));
    palette.setColor(QPalette::ButtonText, QColor("#dee7f3")); palette.setColor(QPalette::Highlight, QColor("#486ca5"));
    palette.setColor(QPalette::HighlightedText, Qt::white); palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#607188"));
    app.setPalette(palette);
    app.setOrganizationName("SimpleAVShow"); app.setApplicationName("SimpleAVShow"); app.setApplicationVersion("0.1.0");
    QCommandLineParser parser; parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"demo", "Open the built-in demonstration"});
    parser.addOption({"screenshot", "Save an editor screenshot and exit (for visual QA)", "path"});
    parser.addOption({"preview-time", "Seek before capturing a screenshot (seconds)", "seconds"});
    parser.addPositionalArgument("project", "Project to open (.savshow)"); parser.process(app);
    sav::MainWindow window;
    if (parser.isSet("demo")) { window.controller().replace(sav::demoProject()); window.controller().dirty = true; }
    if (!parser.positionalArguments().isEmpty()) window.openPath(parser.positionalArguments().front());
    if (parser.isSet("preview-time")) {
        bool ok; double time = parser.value("preview-time").toDouble(&ok);
        if (!ok || !std::isfinite(time) || time < 0 || time > 86400) return 2;
        window.controller().seek(sav::frames(time));
    }
    window.show();
    if (parser.isSet("screenshot")) QTimer::singleShot(1000, &app, [&] {
        const QString path = parser.value("screenshot"); QDir().mkpath(QFileInfo(path).absolutePath());
        if (!window.grab().save(path)) app.exit(2); else app.exit(0);
    });
    return app.exec();
}
