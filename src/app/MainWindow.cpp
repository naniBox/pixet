#include "MainWindow.h"

#include <QLabel>

#include "version.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("pixet %1").arg(pixet::version()));
    resize(1024, 700);

    auto *label = new QLabel(QStringLiteral("pixet %1 — P0 hello-world window").arg(pixet::version()), this);
    label->setAlignment(Qt::AlignCenter);
    setCentralWidget(label);
}
