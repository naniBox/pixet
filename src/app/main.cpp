#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("pixet photo/video viewer"));
    parser.addHelpOption();
    QCommandLineOption resetLayoutOption(
        QStringLiteral("reset-layout"),
        QStringLiteral("Ignore saved window position/size and pane layout for this launch, "
                        "and save fresh defaults back on exit."));
    parser.addOption(resetLayoutOption);
    parser.process(app);

    MainWindow window(parser.isSet(resetLayoutOption));
    window.show();

    return app.exec();
}
