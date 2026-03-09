// VHS-Codec: Digital data storage on VHS tape
// Copyright (C) 2025 John Boero
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QProcess>
#include <QFileDialog>
#include <QTimer>
#include <QDir>
#include <QMenuBar>
#include <QChart>
#include <QChartView>
#include <QBarSeries>
#include <QBarSet>
#include <QBarCategoryAxis>
#include <QValueAxis>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onBrowseInput();
    void onBrowseOutput();
    void onStartEncode();
    void onStartDecode();
    void onStartCalibrate();
    void onStop();
    void onEstimateUpdate();
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onPreviewUpdate();
    void onAbout();

private:
    void setupUI();
    void setupMenuBar();
    QWidget* createEncodeTab();
    QWidget* createDecodeTab();
    QWidget* createCalibrateTab();
    QWidget* createEstimateTab();
    QGroupBox* createParameterGroup();
    void appendLog(const QString &text);
    QStringList buildEncodeArgs();
    QStringList buildDecodeArgs();
    QString findCLI();
    void startPreviewPolling();
    void stopPreviewPolling();

    // Tabs
    QTabWidget *m_tabs;

    // Parameter controls
    QSpinBox *m_qrVersion;
    QComboBox *m_ecLevel;
    QSpinBox *m_modulePx;
    QComboBox *m_grayLevels;
    QDoubleSpinBox *m_dataFPS;
    QDoubleSpinBox *m_fecRatio;
    QSpinBox *m_syncEvery;
    QCheckBox *m_audioEnabled;

    // Encode tab
    QLineEdit *m_inputFile;
    QLineEdit *m_outputFile;
    QComboBox *m_videoDeviceOut;
    QPushButton *m_encodeBtn;

    // Decode tab
    QLineEdit *m_decodeInput;
    QLineEdit *m_decodeOutput;
    QComboBox *m_videoDeviceIn;
    QPushButton *m_decodeBtn;

    // Calibrate tab
    QPushButton *m_calibrateBtn;

    // Estimate display
    QLabel *m_estPayload;
    QLabel *m_estThroughput;
    QLabel *m_estSP;
    QLabel *m_estLP;
    QLabel *m_estEP;
    QChartView *m_chartView;

    // Video preview
    QLabel *m_previewLabel;
    QLabel *m_previewInfo;
    QTimer *m_previewTimer;
    QString m_frameTmpDir;
    int m_lastPreviewFrame;

    // Shared
    QPushButton *m_stopBtn;
    QProgressBar *m_progress;
    QPlainTextEdit *m_logOutput;
    QProcess *m_process;
};
