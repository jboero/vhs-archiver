// VHS-Codec: Digital data storage on VHS tape
// Copyright (C) 2025 John Boero
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QGroupBox>
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

    QTabWidget *m_tabs;
    QSpinBox *m_qrVersion;
    QComboBox *m_ecLevel;
    QSpinBox *m_modulePx;
    QComboBox *m_grayLevels;
    QDoubleSpinBox *m_dataFPS;
    QDoubleSpinBox *m_fecRatio;
    QSpinBox *m_syncEvery;
    QCheckBox *m_audioEnabled;
    QLineEdit *m_inputFile;
    QLineEdit *m_outputFile;
    QComboBox *m_videoDeviceOut;
    QPushButton *m_encodeBtn;
    QLineEdit *m_decodeInput;
    QLineEdit *m_decodeOutput;
    QComboBox *m_videoDeviceIn;
    QPushButton *m_decodeBtn;
    QPushButton *m_calibrateBtn;
    QLabel *m_estPayload;
    QLabel *m_estThroughput;
    QLabel *m_estSP;
    QLabel *m_estLP;
    QLabel *m_estEP;
    QChartView *m_chartView;
    QLabel *m_previewLabel;
    QLabel *m_previewInfo;
    QTimer *m_previewTimer;
    QString m_frameTmpDir;
    int m_lastPreviewFrame;
    QPushButton *m_stopBtn;
    QProgressBar *m_progress;
    QPlainTextEdit *m_logOutput;
    QProcess *m_process;
};
