// VHS-Codec: Digital data storage on VHS tape
// Copyright (C) 2025 John Boero
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QFileInfo>
#include <QStandardPaths>
#include <QMessageBox>
#include <QDir>
#include <QPixmap>
#include <QRegularExpression>
#include <QApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_process(new QProcess(this))
    , m_previewTimer(new QTimer(this))
    , m_lastPreviewFrame(-1)
{
    setWindowTitle("VHS-Codec");
    setMinimumSize(960, 720);
    resize(1100, 800);
    setupMenuBar();
    setupUI();
    connect(m_process, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcessOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &MainWindow::onProcessOutput);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onProcessFinished);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::onPreviewUpdate);
}

void MainWindow::setupMenuBar()
{
    auto *helpMenu = menuBar()->addMenu("&Help");
    auto *aboutAction = helpMenu->addAction("&About VHS-Codec...");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
    auto *aboutQtAction = helpMenu->addAction("About &Qt...");
    connect(aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "About VHS-Codec",
        "<h2>VHS-Codec v0.1.0</h2>"
        "<p>Digital data storage on VHS tape via QR codes and FSK audio.</p>"
        "<p><b>Authors:</b><br>"
        "John Boero<br>"
        "Claude (Anthropic)</p>"
        "<p><b>License:</b><br>"
        "Copyright &copy; 2025 John Boero<br>"
        "GNU General Public License v3.0 or later.</p>"
        "<p><b>Disclaimer:</b><br>"
        "THIS SOFTWARE IS EXPERIMENTAL AND PROVIDED &ldquo;AS IS&rdquo;, "
        "WITHOUT WARRANTY OF ANY KIND. "
        "The authors shall not be held liable for any data loss, corruption, "
        "or damages arising from the use of this software. "
        "<b>Do not rely on this tool as your sole backup strategy.</b> "
        "Use at your own risk.</p>"
        "<p><a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
        "GNU General Public License v3.0</a></p>");
}

void MainWindow::setupUI()
{
    auto *central = new QWidget;
    setCentralWidget(central);
    auto *mainLayout = new QVBoxLayout(central);
    auto *hSplitter = new QSplitter(Qt::Horizontal);
    auto *leftSplitter = new QSplitter(Qt::Vertical);

    m_tabs = new QTabWidget;
    m_tabs->addTab(createEncodeTab(), "Encode");
    m_tabs->addTab(createDecodeTab(), "Decode");
    m_tabs->addTab(createCalibrateTab(), "Calibrate");
    m_tabs->addTab(createEstimateTab(), "Estimate");
    leftSplitter->addWidget(m_tabs);

    auto *logGroup = new QGroupBox("Output Log");
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logOutput = new QPlainTextEdit;
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumBlockCount(5000);
    m_logOutput->setFont(QFont("monospace", 9));
    logLayout->addWidget(m_logOutput);
    leftSplitter->addWidget(logGroup);
    leftSplitter->setStretchFactor(0, 3);
    leftSplitter->setStretchFactor(1, 1);
    hSplitter->addWidget(leftSplitter);

    auto *previewGroup = new QGroupBox("Video Preview");
    auto *previewLayout = new QVBoxLayout(previewGroup);
    m_previewLabel = new QLabel("No video");
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(360, 240);
    m_previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_previewLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_previewLabel->setToolTip("Live preview of QR frames during encoding.");
    previewLayout->addWidget(m_previewLabel, 1);
    m_previewInfo = new QLabel("Idle");
    m_previewInfo->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewInfo);
    hSplitter->addWidget(previewGroup);
    hSplitter->setStretchFactor(0, 3);
    hSplitter->setStretchFactor(1, 2);
    mainLayout->addWidget(hSplitter, 1);

    auto *bottomLayout = new QHBoxLayout;
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_stopBtn = new QPushButton("Stop");
    m_stopBtn->setToolTip("Terminate the running process.");
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);
    bottomLayout->addWidget(m_progress, 1);
    bottomLayout->addWidget(m_stopBtn);
    mainLayout->addLayout(bottomLayout);
    onEstimateUpdate();
}

QGroupBox* MainWindow::createParameterGroup()
{
    auto *group = new QGroupBox("Parameters");
    group->setToolTip("Tunable encoding parameters. Changes update the capacity estimate in real time.");
    auto *form = new QFormLayout(group);

    auto addTipRow = [&](const QString &text, const QString &tip, QWidget *w) {
        auto *lbl = new QLabel(text);
        lbl->setToolTip(tip);
        lbl->setBuddy(w);
        form->addRow(lbl, w);
    };

    m_qrVersion = new QSpinBox;
    m_qrVersion->setRange(1, 40);
    m_qrVersion->setValue(25);
    connect(m_qrVersion, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("QR Version:", "QR code version (1-40). Higher = more data per frame but finer detail.\nVersion 25 = 117x117 modules. Sweet spot for VHS is 15-30.", m_qrVersion);

    m_ecLevel = new QComboBox;
    m_ecLevel->addItems({"L (7%)", "M (15%)", "Q (25%)", "H (30%)"});
    m_ecLevel->setCurrentIndex(1);
    connect(m_ecLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("EC Level:", "Error correction level. Higher = more damage recovery but less capacity.\nL=max density, M=balanced, Q=aged tapes, H=archival.", m_ecLevel);

    m_modulePx = new QSpinBox;
    m_modulePx->setRange(2, 8);
    m_modulePx->setValue(4);
    connect(m_modulePx, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("Module Pixels:", "Pixels per QR module. Larger = more reliable but less data.\n2px=aggressive, 4px=good default, 6-8px=conservative.", m_modulePx);

    m_grayLevels = new QComboBox;
    m_grayLevels->addItems({"2 (B&W)", "4 (2-bit)", "8 (3-bit)"});
    connect(m_grayLevels, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("Gray Levels:", "Luminance levels per module. B&W is most reliable through VHS.\n4/8 levels multiply density but are experimental.", m_grayLevels);

    m_dataFPS = new QDoubleSpinBox;
    m_dataFPS->setRange(1.0, 29.97);
    m_dataFPS->setValue(10.0);
    m_dataFPS->setSingleStep(1.0);
    connect(m_dataFPS, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("Data FPS:", "Unique QR frames per second. NTSC=29.97fps.\nLower = more redundancy per frame. 10fps = ~3 video frames per QR.", m_dataFPS);

    m_fecRatio = new QDoubleSpinBox;
    m_fecRatio->setRange(0.0, 0.9);
    m_fecRatio->setValue(0.3);
    m_fecRatio->setSingleStep(0.05);
    connect(m_fecRatio, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("FEC Ratio:", "Forward error correction redundancy (0.0-0.9).\n0.3 = can lose 30% of frames and still recover. 0.0 = no redundancy.", m_fecRatio);

    m_syncEvery = new QSpinBox;
    m_syncEvery->setRange(0, 1000);
    m_syncEvery->setValue(50);
    m_syncEvery->setSpecialValueText("Disabled");
    connect(m_syncEvery, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onEstimateUpdate);
    addTipRow("Sync Every N:", "Insert sync frame every N data frames.\nHelps decoder recover from tracking jumps. 0=disabled, 50=default.", m_syncEvery);

    m_audioEnabled = new QCheckBox("Enable FSK audio data channel");
    connect(m_audioEnabled, &QCheckBox::toggled, this, &MainWindow::onEstimateUpdate);
    addTipRow("Audio:", "Encode extra data on VHS hi-fi audio tracks via FSK modulation.\nAdds ~10-20% capacity. Requires audio capture/output hardware.", m_audioEnabled);

    return group;
}

QWidget* MainWindow::createEncodeTab()
{
    auto *w = new QWidget;
    auto *lay = new QHBoxLayout(w);
    lay->addWidget(createParameterGroup());

    auto *right = new QVBoxLayout;
    auto *fg = new QGroupBox("Encode");
    auto *ff = new QFormLayout(fg);

    auto *il = new QHBoxLayout;
    m_inputFile = new QLineEdit;
    m_inputFile->setPlaceholderText("Select file to encode...");
    auto *bi = new QPushButton("Browse...");
    connect(bi, &QPushButton::clicked, this, &MainWindow::onBrowseInput);
    il->addWidget(m_inputFile, 1);
    il->addWidget(bi);
    auto *inLbl = new QLabel("Input File:");
    inLbl->setToolTip("File to encode onto VHS. Any file type, treated as raw binary.");
    ff->addRow(inLbl, il);

    m_videoDeviceOut = new QComboBox;
    m_videoDeviceOut->setEditable(true);
    m_videoDeviceOut->addItems({"/dev/video0", "/dev/video1", "/dev/video2"});
    auto *dvLbl = new QLabel("Video Out:");
    dvLbl->setToolTip("V4L2 output device. Run 'v4l2-ctl --list-devices' to find yours.");
    ff->addRow(dvLbl, m_videoDeviceOut);

    auto *ol = new QHBoxLayout;
    m_outputFile = new QLineEdit;
    m_outputFile->setPlaceholderText("Or save as video file...");
    auto *bo = new QPushButton("Browse...");
    connect(bo, &QPushButton::clicked, this, &MainWindow::onBrowseOutput);
    ol->addWidget(m_outputFile, 1);
    ol->addWidget(bo);
    auto *outLbl = new QLabel("Output File:");
    outLbl->setToolTip("Save encoded video to file (.mp4/.avi). Lossless H.264 YUV444.");
    ff->addRow(outLbl, ol);

    right->addWidget(fg);
    m_encodeBtn = new QPushButton("Start Encoding");
    m_encodeBtn->setToolTip("Encode input file into QR code video frames.");
    connect(m_encodeBtn, &QPushButton::clicked, this, &MainWindow::onStartEncode);
    right->addWidget(m_encodeBtn);
    right->addStretch();
    lay->addLayout(right, 1);
    return w;
}

QWidget* MainWindow::createDecodeTab()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    auto *g = new QGroupBox("Decode from VHS Playback");
    auto *f = new QFormLayout(g);

    m_videoDeviceIn = new QComboBox;
    m_videoDeviceIn->setEditable(true);
    m_videoDeviceIn->addItems({"/dev/video0", "/dev/video1", "/dev/video2"});
    auto *capLbl = new QLabel("Capture Device:");
    capLbl->setToolTip("V4L2 capture device. Connect VCR composite out to this adapter.");
    f->addRow(capLbl, m_videoDeviceIn);

    auto *fl = new QHBoxLayout;
    m_decodeInput = new QLineEdit;
    m_decodeInput->setPlaceholderText("Or decode from video file...");
    auto *bd = new QPushButton("Browse...");
    connect(bd, &QPushButton::clicked, [this]() {
        auto p = QFileDialog::getOpenFileName(this, "Select Video", {}, "Video Files (*.avi *.mp4 *.mkv);;All (*)");
        if (!p.isEmpty()) m_decodeInput->setText(p);
    });
    fl->addWidget(m_decodeInput, 1);
    fl->addWidget(bd);
    auto *vfLbl = new QLabel("Video File:");
    vfLbl->setToolTip("Decode from captured video file instead of live device.");
    f->addRow(vfLbl, fl);

    auto *ol = new QHBoxLayout;
    m_decodeOutput = new QLineEdit;
    m_decodeOutput->setPlaceholderText("Output file for restored data...");
    auto *bdo = new QPushButton("Browse...");
    connect(bdo, &QPushButton::clicked, [this]() {
        auto p = QFileDialog::getSaveFileName(this, "Save Restored File");
        if (!p.isEmpty()) m_decodeOutput->setText(p);
    });
    ol->addWidget(m_decodeOutput, 1);
    ol->addWidget(bdo);
    auto *doLbl = new QLabel("Output:");
    doLbl->setToolTip("Restored file path. Should be byte-identical to original (CRC32 verified).");
    f->addRow(doLbl, ol);

    lay->addWidget(g);
    m_decodeBtn = new QPushButton("Start Decoding");
    m_decodeBtn->setToolTip("Capture and decode QR frames, reassemble original file.");
    connect(m_decodeBtn, &QPushButton::clicked, this, &MainWindow::onStartDecode);
    lay->addWidget(m_decodeBtn);
    lay->addStretch();
    return w;
}

QWidget* MainWindow::createCalibrateTab()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    auto *info = new QLabel("Calibration sweeps QR version, module size, gray levels, and FPS\n"
        "to find optimal settings for your hardware.\n\nLeave devices empty for file-based testing.");
    info->setWordWrap(true);
    lay->addWidget(info);
    m_calibrateBtn = new QPushButton("Run Calibration Sweep");
    m_calibrateBtn->setToolTip("Test all parameter combinations to find density vs. reliability frontier.");
    connect(m_calibrateBtn, &QPushButton::clicked, this, &MainWindow::onStartCalibrate);
    lay->addWidget(m_calibrateBtn);
    lay->addStretch();
    return w;
}

QWidget* MainWindow::createEstimateTab()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    auto *g = new QGroupBox("Capacity Estimate (live from Parameters)");
    auto *grid = new QGridLayout(g);
    grid->addWidget(new QLabel("Payload/Frame:"), 0, 0);
    m_estPayload = new QLabel("-"); grid->addWidget(m_estPayload, 0, 1);
    grid->addWidget(new QLabel("Throughput:"), 1, 0);
    m_estThroughput = new QLabel("-"); grid->addWidget(m_estThroughput, 1, 1);
    grid->addWidget(new QLabel("SP (2 hr):"), 2, 0);
    m_estSP = new QLabel("-"); grid->addWidget(m_estSP, 2, 1);
    grid->addWidget(new QLabel("LP (4 hr):"), 3, 0);
    m_estLP = new QLabel("-"); grid->addWidget(m_estLP, 3, 1);
    grid->addWidget(new QLabel("EP (6 hr):"), 4, 0);
    m_estEP = new QLabel("-"); grid->addWidget(m_estEP, 4, 1);
    lay->addWidget(g);

    auto *chart = new QChart;
    chart->setTitle("Estimated Capacity by Tape Speed");
    chart->setAnimationOptions(QChart::SeriesAnimations);
    m_chartView = new QChartView(chart);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    m_chartView->setMinimumHeight(250);
    lay->addWidget(m_chartView);
    lay->addStretch();
    return w;
}

void MainWindow::onBrowseInput()
{
    auto p = QFileDialog::getOpenFileName(this, "Select File to Encode");
    if (!p.isEmpty()) { m_inputFile->setText(p); appendLog("Selected: " + QFileInfo(p).fileName()); }
}

void MainWindow::onBrowseOutput()
{
    auto p = QFileDialog::getSaveFileName(this, "Save Encoded Video", {}, "AVI (*.avi);;MP4 (*.mp4);;All (*)");
    if (!p.isEmpty()) m_outputFile->setText(p);
}

void MainWindow::onStartEncode()
{
    if (m_inputFile->text().isEmpty()) { QMessageBox::warning(this, "VHS-Codec", "Select an input file."); return; }
    auto args = buildEncodeArgs();
    appendLog("$ vhs-codec " + args.join(" "));
    m_process->start(findCLI(), args);
    m_encodeBtn->setEnabled(false); m_stopBtn->setEnabled(true); m_progress->setRange(0, 0);
    startPreviewPolling();
}

void MainWindow::onStartDecode()
{
    auto args = buildDecodeArgs();
    appendLog("$ vhs-codec " + args.join(" "));
    m_process->start(findCLI(), args);
    m_decodeBtn->setEnabled(false); m_stopBtn->setEnabled(true); m_progress->setRange(0, 0);
}

void MainWindow::onStartCalibrate()
{
    QStringList args = {"calibrate"};
    appendLog("$ vhs-codec " + args.join(" "));
    m_process->start(findCLI(), args);
    m_calibrateBtn->setEnabled(false); m_stopBtn->setEnabled(true); m_progress->setRange(0, 0);
}

void MainWindow::onStop()
{
    if (m_process->state() != QProcess::NotRunning) { m_process->terminate(); appendLog("Terminated."); }
    stopPreviewPolling();
}

void MainWindow::onEstimateUpdate()
{
    int version = m_qrVersion->value();
    int ec = m_ecLevel->currentIndex();
    double fps = m_dataFPS->value();
    double fecRatio = m_fecRatio->value();
    int syncN = m_syncEvery->value();
    bool audio = m_audioEnabled->isChecked();
    int grayLevels = (m_grayLevels->currentIndex() == 0) ? 2 : (m_grayLevels->currentIndex() == 1) ? 4 : 8;

    struct Cap { int v; int c[4]; };
    Cap t[] = {{1,{17,14,11,7}},{5,{106,84,62,46}},{10,{271,213,151,119}},{15,{412,311,235,178}},
               {20,{586,450,331,261}},{25,{755,590,427,341}},{30,{1003,769,573,445}},
               {35,{1249,959,706,552}},{40,{2953,2331,1663,1273}}};
    int closest = 0;
    for (auto &e : t) { if (e.v <= version) closest = e.v; }
    int rawCap = 0;
    for (auto &e : t) { if (e.v == closest) { rawCap = e.c[ec]; break; } }

    int binCap = (rawCap / 4) * 3;
    int payload = binCap - 17;
    if (payload < 1) payload = 1;
    int bpm = (grayLevels == 4) ? 2 : (grayLevels == 8) ? 3 : 1;
    payload *= bpm;
    double eff = payload * (1.0 - fecRatio);
    double syncOH = (syncN > 0) ? (1.0 - 1.0 / syncN) : 1.0;
    double vBPS = eff * fps * syncOH;
    double aBPS = audio ? (1200.0 / 8.0 * 0.8) : 0.0;
    double total = vBPS + aBPS;
    double sp = total * 7200, lp = total * 14400, ep = total * 21600;

    m_estPayload->setText(QString("%1 bytes").arg(int(eff)));
    m_estThroughput->setText(QString("%1 KB/s (video: %2, audio: %3)").arg(total/1024,0,'f',1).arg(vBPS/1024,0,'f',1).arg(aBPS/1024,0,'f',1));
    m_estSP->setText(QString("%1 MB").arg(sp/1048576,0,'f',1));
    m_estLP->setText(QString("%1 MB").arg(lp/1048576,0,'f',1));
    m_estEP->setText(QString("%1 MB").arg(ep/1048576,0,'f',1));

    auto *chart = m_chartView->chart(); chart->removeAllSeries();
    auto *set = new QBarSet("Capacity (MB)");
    *set << sp/1048576 << lp/1048576 << ep/1048576;
    auto *series = new QBarSeries; series->append(set); chart->addSeries(series);
    for (auto *a : chart->axes()) chart->removeAxis(a);
    auto *ax = new QBarCategoryAxis; ax->append({"SP (2hr)","LP (4hr)","EP (6hr)"});
    chart->addAxis(ax, Qt::AlignBottom); series->attachAxis(ax);
    auto *ay = new QValueAxis; ay->setTitleText("MB");
    ay->setRange(0, ep/1048576 > 0 ? ep/1048576*1.15 : 100);
    chart->addAxis(ay, Qt::AlignLeft); series->attachAxis(ay);
}

void MainWindow::onProcessOutput()
{
    auto out = m_process->readAllStandardOutput();
    auto err = m_process->readAllStandardError();
    if (!out.isEmpty()) appendLog(QString::fromUtf8(out).trimmed());
    if (!err.isEmpty()) appendLog(QString::fromUtf8(err).trimmed());
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
    m_encodeBtn->setEnabled(true); m_decodeBtn->setEnabled(true);
    m_calibrateBtn->setEnabled(true); m_stopBtn->setEnabled(false);
    m_progress->setRange(0, 100); m_progress->setValue(exitCode == 0 ? 100 : 0);
    stopPreviewPolling();
    appendLog(QString("Process finished (exit code: %1)").arg(exitCode));
    m_previewInfo->setText(exitCode == 0 ? "Done" : "Error - see log");
}

void MainWindow::startPreviewPolling()
{
    m_frameTmpDir.clear(); m_lastPreviewFrame = -1;
    m_previewInfo->setText("Encoding...");
    m_previewTimer->start(250);
}

void MainWindow::stopPreviewPolling() { m_previewTimer->stop(); }

void MainWindow::onPreviewUpdate()
{
    if (m_frameTmpDir.isEmpty()) {
        QDir tmp("/tmp");
        auto dirs = tmp.entryList({"vhs-codec-frames-*"}, QDir::Dirs, QDir::Time);
        if (!dirs.isEmpty()) m_frameTmpDir = "/tmp/" + dirs.first();
        else return;
    }
    QDir fd(m_frameTmpDir);
    if (!fd.exists()) { m_frameTmpDir.clear(); return; }
    auto files = fd.entryList({"frame_*.png"}, QDir::Files, QDir::Name);
    if (files.isEmpty()) return;
    static QRegularExpression re("frame_(\\d+)\\.png");
    auto match = re.match(files.last());
    int fn = match.hasMatch() ? match.captured(1).toInt() : -1;
    if (fn <= m_lastPreviewFrame) return;
    m_lastPreviewFrame = fn;
    QPixmap pix(m_frameTmpDir + "/" + files.last());
    if (!pix.isNull())
        m_previewLabel->setPixmap(pix.scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_previewInfo->setText(QString("Frame %1 / %2 files").arg(fn).arg(files.size()));
}

void MainWindow::appendLog(const QString &text) { m_logOutput->appendPlainText(text); }

QStringList MainWindow::buildEncodeArgs()
{
    QStringList a = {"encode", "--input", m_inputFile->text()};
    if (!m_outputFile->text().isEmpty()) a << "--output" << m_outputFile->text();
    else a << "--device" << m_videoDeviceOut->currentText();
    a << "--qr-version" << QString::number(m_qrVersion->value());
    QString ec[] = {"L","M","Q","H"};
    a << "--ec-level" << ec[m_ecLevel->currentIndex()];
    a << "--module-px" << QString::number(m_modulePx->value());
    int gv[] = {2,4,8};
    a << "--gray-levels" << QString::number(gv[m_grayLevels->currentIndex()]);
    a << "--fps" << QString::number(m_dataFPS->value(),'f',2);
    a << "--fec-ratio" << QString::number(m_fecRatio->value(),'f',2);
    a << "--sync-every" << QString::number(m_syncEvery->value());
    if (m_audioEnabled->isChecked()) a << "--audio";
    return a;
}

QStringList MainWindow::buildDecodeArgs()
{
    QStringList a = {"decode"};
    if (!m_decodeInput->text().isEmpty()) a << "--file" << m_decodeInput->text();
    else a << "--device" << m_videoDeviceIn->currentText();
    if (!m_decodeOutput->text().isEmpty()) a << "--output" << m_decodeOutput->text();
    if (m_audioEnabled->isChecked()) a << "--audio";
    return a;
}

QString MainWindow::findCLI()
{
    for (const auto &p : {QDir::currentPath()+"/vhs-codec", QDir::currentPath()+"/../vhs-codec"})
        if (QFileInfo::exists(p)) return p;
    auto found = QStandardPaths::findExecutable("vhs-codec");
    return found.isEmpty() ? "vhs-codec" : found;
}
