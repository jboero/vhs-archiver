// VHS-Codec: Digital data storage on VHS tape
// Copyright (C) 2025 John Boero
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("VHS-Codec");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("vhs-codec");

    MainWindow window;
    window.show();

    return app.exec();
}
