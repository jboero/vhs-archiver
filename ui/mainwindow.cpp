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
#include <QImage>
#include <QRegularExpression>

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

void MainWindow::setupUI()
{
    auto *central = new QWidget;
    setCentralWidget(central);
    auto *mainLayout = new QVBoxLayout(central);

    // Main horizontal splitter: left (tabs + log) | right (preview)
    auto *hSplitter = new QSplitter(Qt::Horizontal);

    // Left side: tabs + log in a vertical splitter
    auto *leftSplitter = new QSplitter(Qt::Vertical);

    m_tabs = new QTabWidget;
    m_tabs->addTab(createEncodeTab(), "Encode");
    m_tabs->addTab(createDecodeTab(), "Decode");
    m_tabs->addTab(createCalibrateTab(), "Calibrate");
    m_tabs->addTab(createEstimateTab(), "Estimate");
    leftSplitter->addWidget(m_tabs);

    // Log output
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

    // Right side: video preview
    auto *previewGroup = new QGroupBox("Video Preview");
    auto *previewLayout = new QVBoxLayout(previewGroup);

    m_previewLabel = new QLabel("No video");
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(360, 240);
    m_previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_previewLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_previewLabel->setToolTip(
        "Live preview of QR frames during encoding.
"
        "Shows the most recently generated frame from the
"
        "encoder's temp directory, updating ~4 times per second.");
    previewLayout->addWidget(m_previewLabel, 1);

    m_previewInfo = new QLabel("Idle");
    m_previewInfo->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewInfo);

    hSplitter->addWidget(previewGroup);
    hSplitter->setStretchFactor(0, 3);
    hSplitter->setStretchFactor(1, 2);

    mainLayout->addWidget(hSplitter, 1);

    // Bottom bar: progress + stop
    auto *bottomLayout = new QHBoxLayout;
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_stopBtn = new QPushButton("Stop");
    m_stopBtn->setToolTip("Terminate the running encode, decode, or calibration process.");
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
    auto *form = new QFormLayout(group);

    // Helper: adds a row with tooltip on both the label and the widget
    auto addTipRow = [&](const QString &labelText, QWidget *widget) {
        auto *label = new QLabel(labelText);
        label->setToolTip(widget->toolTip());
        label->setBuddy(widget);
        form->addRow(label, widget);
    };

    m_qrVersion = new QSpinBox;
    m_qrVersion->setRange(1, 40);
    m_qrVersion->setValue(25);
    m_qrVersion->setToolTip(
        "QR code version (1–40). Higher versions have more modules
"
        "and can store more data per frame, but require finer detail
"
        "that may not survive the VHS analog round-trip.

"
        "Version 1 = 21×21 modules (7 bytes at EC-H)
"
        "Version 25 = 117×117 modules (~341 bytes at EC-H)
"
        "Version 40 = 177×177 modules (~1273 bytes at EC-H)

"
        "Practical sweet spot for VHS is typically 15–30.");
    connect(m_qrVersion, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("QR Version:", m_qrVersion);

    m_ecLevel = new QComboBox;
    m_ecLevel->addItems({"L (7%)", "M (15%)", "Q (25%)", "H (30%)"});
    m_ecLevel->setCurrentIndex(1);
    m_ecLevel->setToolTip(
        "QR error correction level. Higher levels recover from more
"
        "damage but reduce data capacity per frame.

"
        "L — 7% recovery. Maximum density, least resilient.
"
        "M — 15% recovery. Good balance for testing.
"
        "Q — 25% recovery. Recommended for aged tapes.
"
        "H — 30% recovery. Best for long-term archival.

"
        "VHS introduces dropout artifacts, tracking errors, and
"
        "chroma bleed — higher EC helps survive these.");
    connect(m_ecLevel, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("EC Level:", m_ecLevel);

    m_modulePx = new QSpinBox;
    m_modulePx->setRange(2, 8);
    m_modulePx->setValue(4);
    m_modulePx->setToolTip(
        "Pixels per QR module (the smallest square unit in a QR code).
"
        "Larger modules are easier to read through analog degradation
"
        "but reduce the effective QR version that fits in a frame.

"
        "2 px — Very aggressive, unlikely to survive VHS.
"
        "3 px — Pushing the limits of VHS horizontal resolution.
"
        "4 px — Good default for most hardware.
"
        "6–8 px — Conservative, high reliability.

"
        "The QR code must fit within the 720×480 NTSC frame.
"
        "At 4 px, a Version 25 QR is 468×468 pixels.");
    connect(m_modulePx, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("Module Pixels:", m_modulePx);

    m_grayLevels = new QComboBox;
    m_grayLevels->addItems({"2 (B&W)", "4 (2-bit)", "8 (3-bit)"});
    m_grayLevels->setToolTip(
        "Number of luminance levels per QR module.

"
        "2 (B&W) — Standard black/white. Most reliable through VHS.
"
        "4 (2-bit) — Four gray levels per module, doubles data density.
"
        "8 (3-bit) — Eight levels, triples density.

"
        "Multi-gray encoding is experimental. VHS luminance bandwidth
"
        "is ~3 MHz (~240 TV lines), so distinguishing subtle gray
"
        "differences is difficult. B&W recommended for real hardware.
"
        "Multi-gray is interesting for blog testing and benchmarks.");
    connect(m_grayLevels, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("Gray Levels:", m_grayLevels);

    m_dataFPS = new QDoubleSpinBox;
    m_dataFPS->setRange(1.0, 29.97);
    m_dataFPS->setValue(10.0);
    m_dataFPS->setSingleStep(1.0);
    m_dataFPS->setToolTip(
        "Unique QR data frames generated per second.
"
        "NTSC video runs at 29.97 fps — this controls how many of
"
        "those frames carry new data vs. repeating the previous frame.

"
        "Lower values give the decoder more chances to read each frame
"
        "but reduce throughput. Higher values maximize data rate but
"
        "may cause missed frames during playback capture.

"
        "10 fps is a good starting point. Each QR frame is held for
"
        "~3 video frames at 10 fps, giving the decoder redundancy.");
    connect(m_dataFPS, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("Data FPS:", m_dataFPS);

    m_fecRatio = new QDoubleSpinBox;
    m_fecRatio->setRange(0.0, 0.9);
    m_fecRatio->setValue(0.3);
    m_fecRatio->setSingleStep(0.05);
    m_fecRatio->setToolTip(
        "Application-level forward error correction redundancy ratio.
"
        "This is additional redundancy on top of the QR code's own EC.

"
        "0.0 — No extra redundancy. Every frame must decode correctly.
"
        "0.3 — 30% redundant frames. Can lose ~30% of frames and
"
        "       still recover the full file. Good default.
"
        "0.5 — 50% redundancy. Very resilient but halves throughput.

"
        "Uses fountain-code-style encoding so the decoder doesn't
"
        "need specific frames — just enough total frames.");
    connect(m_fecRatio, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("FEC Ratio:", m_fecRatio);

    m_syncEvery = new QSpinBox;
    m_syncEvery->setRange(0, 1000);
    m_syncEvery->setValue(50);
    m_syncEvery->setSpecialValueText("Disabled");
    m_syncEvery->setToolTip(
        "Insert a synchronization frame every N data frames.
"
        "Sync frames carry sequence numbers and timestamps that
"
        "help the decoder recover from tracking jumps, head-switch
"
        "noise, or brief signal dropouts during VHS playback.

"
        "0 — Disabled. No sync frames inserted.
"
        "50 — One sync frame per 50 data frames (~5 sec at 10 fps).
"
        "10 — Frequent sync, more resilient but reduces throughput.

"
        "Each sync frame replaces one data frame in the stream.");
    connect(m_syncEvery, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onEstimateUpdate);
    addTipRow("Sync Every N:", m_syncEvery);

    m_audioEnabled = new QCheckBox("Enable FSK audio data channel");
    m_audioEnabled->setToolTip(
        "Encode additional data on the VHS hi-fi stereo audio tracks
"
        "using FSK (Frequency Shift Keying) modulation.

"
        "VHS hi-fi audio has 20 Hz – 20 kHz bandwidth with ~70 dB SNR,
"
        "which is much better than the video channel. At 1200 baud FSK,
"
        "this adds ~120 bytes/sec of extra throughput per channel.

"
        "Requires a separate USB audio capture/output device or a
"
        "composite adapter that also handles audio. The audio data
"
        "stream is independent of the video QR stream and provides
"
        "a bonus ~10–20%% capacity on top of video throughput.

"
        "The linear mono track is not used (low quality, ~10 kHz).
"
        "Only the hi-fi stereo tracks are used.");
    connect(m_audioEnabled, &QCheckBox::toggled, this, &MainWindow::onEstimateUpdate);
    addTipRow("Audio:", m_audioEnabled);

    return group;
}

QWidget* MainWindow::createEncodeTab()
{
    auto *widget = new QWidget;
    auto *layout = new QHBoxLayout(widget);

    layout->addWidget(createParameterGroup());

    auto *rightLayout = new QVBoxLayout;

    auto *fileGroup = new QGroupBox("Encode");
    auto *fileForm = new QFormLayout(fileGroup);

    auto *inputLayout = new QHBoxLayout;
    m_inputFile = new QLineEdit;
    m_inputFile->setPlaceholderText("Select file to encode...");
    auto *browseIn = new QPushButton("Browse...");
    connect(browseIn, &QPushButton::clicked, this, &MainWindow::onBrowseInput);
    inputLayout->addWidget(m_inputFile, 1);
    inputLayout->addWidget(browseIn);
    auto *inputLabel = new QLabel("Input File:");
    inputLabel->setToolTip(
        "The file to encode onto VHS. Can be any file type —
"
        "it will be treated as raw binary data, chunked, and
"
        "encoded into QR code video frames.");
    fileForm->addRow(inputLabel, inputLayout);

    m_videoDeviceOut = new QComboBox;
    m_videoDeviceOut->setEditable(true);
    m_videoDeviceOut->addItems({"/dev/video0", "/dev/video1", "/dev/video2"});
    auto *devOutLabel = new QLabel("Video Out:");
    devOutLabel->setToolTip(
        "V4L2 video output device for your USB composite adapter.
"
        "Streams QR video directly to composite output while the VCR records.

"
        "Run 'v4l2-ctl --list-devices' to find your adapter.
"
        "Leave empty if using Output File instead.");
    fileForm->addRow(devOutLabel, m_videoDeviceOut);

    auto *outLayout = new QHBoxLayout;
    m_outputFile = new QLineEdit;
    m_outputFile->setPlaceholderText("Or save as video file...");
    auto *browseOut = new QPushButton("Browse...");
    connect(browseOut, &QPushButton::clicked, this, &MainWindow::onBrowseOutput);
    outLayout->addWidget(m_outputFile, 1);
    outLayout->addWidget(browseOut);
    auto *outLabel = new QLabel("Output File:");
    outLabel->setToolTip(
        "Save the encoded QR video to a file instead of streaming
"
        "to a device. Useful for previewing or playing back later
"
        "through a video player to composite output.

"
        "Supported formats: .mp4, .avi, .mkv
"
        "Uses lossless H.264 with YUV 4:4:4 to preserve QR edges.");
    fileForm->addRow(outLabel, outLayout);

    rightLayout->addWidget(fileGroup);

    m_encodeBtn = new QPushButton("Start Encoding");
    m_encodeBtn->setToolTip("Begin encoding the input file into QR code video frames.
"
                            "Frames are written to a temp directory, then assembled
"
                            "into the output video with ffmpeg.");
    connect(m_encodeBtn, &QPushButton::clicked, this, &MainWindow::onStartEncode);
    rightLayout->addWidget(m_encodeBtn);

    rightLayout->addStretch();
    layout->addLayout(rightLayout, 1);

    return widget;
}

QWidget* MainWindow::createDecodeTab()
{
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);

    auto *group = new QGroupBox("Decode from VHS Playback");
    auto *form = new QFormLayout(group);

    m_videoDeviceIn = new QComboBox;
    m_videoDeviceIn->setEditable(true);
    m_videoDeviceIn->addItems({"/dev/video0", "/dev/video1", "/dev/video2"});
    auto *capLabel = new QLabel("Capture Device:");
    capLabel->setToolTip(
        "V4L2 video capture device for your USB composite adapter.
"
        "Connect the VCR's composite output to this adapter and
"
        "press play on the VCR before starting decode.

"
        "Run 'v4l2-ctl --list-devices' to find your adapter.");
    form->addRow(capLabel, m_videoDeviceIn);

    auto *fileLayout = new QHBoxLayout;
    m_decodeInput = new QLineEdit;
    m_decodeInput->setPlaceholderText("Or decode from video file...");
    auto *browseDec = new QPushButton("Browse...");
    connect(browseDec, &QPushButton::clicked, [this]() {
        auto path = QFileDialog::getOpenFileName(this, "Select Video", {},
            "Video Files (*.avi *.mp4 *.mkv);;All Files (*)");
        if (!path.isEmpty()) m_decodeInput->setText(path);
    });
    fileLayout->addWidget(m_decodeInput, 1);
    fileLayout->addWidget(browseDec);
    auto *vidFileLabel = new QLabel("Video File:");
    vidFileLabel->setToolTip(
        "Decode from a previously captured video file instead of
"
        "a live device. Useful for testing — encode to file, then
"
        "decode from the same file to verify the round-trip.");
    form->addRow(vidFileLabel, fileLayout);

    auto *outLayout = new QHBoxLayout;
    m_decodeOutput = new QLineEdit;
    m_decodeOutput->setPlaceholderText("Output file for restored data...");
    auto *browseDecOut = new QPushButton("Browse...");
    connect(browseDecOut, &QPushButton::clicked, [this]() {
        auto path = QFileDialog::getSaveFileName(this, "Save Restored File");
        if (!path.isEmpty()) m_decodeOutput->setText(path);
    });
    outLayout->addWidget(m_decodeOutput, 1);
    outLayout->addWidget(browseDecOut);
    auto *decOutLabel = new QLabel("Output:");
    decOutLabel->setToolTip(
        "Where to write the restored file after decoding.
"
        "The output should be byte-identical to the original input
"
        "if all frames were decoded successfully (CRC32 verified).");
    form->addRow(decOutLabel, outLayout);

    layout->addWidget(group);

    m_decodeBtn = new QPushButton("Start Decoding");
    m_decodeBtn->setToolTip("Begin capturing and decoding QR frames from VHS playback.
"
                            "Detects QR codes in each video frame, verifies CRC32
"
                            "checksums, and reassembles the original file.");
    connect(m_decodeBtn, &QPushButton::clicked, this, &MainWindow::onStartDecode);
    layout->addWidget(m_decodeBtn);

    layout->addStretch();
    return widget;
}

QWidget* MainWindow::createCalibrateTab()
{
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);

    auto *info = new QLabel(
        "Calibration runs a sweep across QR versions, module sizes, gray levels, "
        "and frame rates to find the optimal settings for your specific hardware.

"
        "For file-based testing (no VCR needed), leave device fields empty.
"
        "For hardware loopback, connect composite out → VCR → composite in.");
    info->setWordWrap(true);
    layout->addWidget(info);

    m_calibrateBtn = new QPushButton("Run Calibration Sweep");
    m_calibrateBtn->setToolTip(
        "Tests all combinations of QR version, module size, gray levels,
"
        "and frame rate to find the Pareto frontier of density vs. reliability
"
        "for your specific VCR and USB adapter hardware.");
    connect(m_calibrateBtn, &QPushButton::clicked, this, &MainWindow::onStartCalibrate);
    layout->addWidget(m_calibrateBtn);

    layout->addStretch();
    return widget;
}

QWidget* MainWindow::createEstimateTab()
{
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);

    auto *group = new QGroupBox("Capacity Estimate (updates live from Parameters)");
    auto *grid = new QGridLayout(group);

    grid->addWidget(new QLabel("Payload/Frame:"), 0, 0);
    m_estPayload = new QLabel("—");
    grid->addWidget(m_estPayload, 0, 1);

    grid->addWidget(new QLabel("Throughput:"), 1, 0);
    m_estThroughput = new QLabel("—");
    grid->addWidget(m_estThroughput, 1, 1);

    grid->addWidget(new QLabel("SP (2 hr):"), 2, 0);
    m_estSP = new QLabel("—");
    grid->addWidget(m_estSP, 2, 1);

    grid->addWidget(new QLabel("LP (4 hr):"), 3, 0);
    m_estLP = new QLabel("—");
    grid->addWidget(m_estLP, 3, 1);

    grid->addWidget(new QLabel("EP (6 hr):"), 4, 0);
    m_estEP = new QLabel("—");
    grid->addWidget(m_estEP, 4, 1);

    layout->addWidget(group);

    // Capacity bar chart
    auto *chart = new QChart;
    chart->setTitle("Estimated Capacity by Tape Speed");
    chart->setAnimationOptions(QChart::SeriesAnimations);

    m_chartView = new QChartView(chart);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    m_chartView->setMinimumHeight(250);
    layout->addWidget(m_chartView);

    layout->addStretch();
    return widget;
}

void MainWindow::onBrowseInput()
{
    auto path = QFileDialog::getOpenFileName(this, "Select File to Encode");
    if (!path.isEmpty()) {
        m_inputFile->setText(path);
        QFileInfo fi(path);
        appendLog(QString("Selected: %1 (%2 bytes)")
            .arg(fi.fileName())
            .arg(fi.size()));
    }
}

void MainWindow::onBrowseOutput()
{
    auto path = QFileDialog::getSaveFileName(this, "Save Encoded Video", {},
        "AVI Files (*.avi);;MP4 Files (*.mp4);;All Files (*)");
    if (!path.isEmpty()) m_outputFile->setText(path);
}

void MainWindow::onStartEncode()
{
    if (m_inputFile->text().isEmpty()) {
        QMessageBox::warning(this, "VHS-Codec", "Please select an input file.");
        return;
    }

    auto args = buildEncodeArgs();
    appendLog("$ vhs-codec " + args.join(" "));

    m_process->start(findCLI(), args);
    m_encodeBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_progress->setRange(0, 0);
    startPreviewPolling();
}

void MainWindow::onStartDecode()
{
    auto args = buildDecodeArgs();
    appendLog("$ vhs-codec " + args.join(" "));

    m_process->start(findCLI(), args);
    m_decodeBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_progress->setRange(0, 0);
}

void MainWindow::onStartCalibrate()
{
    QStringList args = {"calibrate"};
    appendLog("$ vhs-codec " + args.join(" "));

    m_process->start(findCLI(), args);
    m_calibrateBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_progress->setRange(0, 0);
}

void MainWindow::onStop()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        appendLog("Process terminated.");
    }
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

    int grayIdx = m_grayLevels->currentIndex();
    int grayLevels = (grayIdx == 0) ? 2 : (grayIdx == 1) ? 4 : 8;

    struct Cap { int v; int caps[4]; };
    Cap table[] = {
        {1,  {17, 14, 11, 7}},
        {5,  {106, 84, 62, 46}},
        {10, {271, 213, 151, 119}},
        {15, {412, 311, 235, 178}},
        {20, {586, 450, 331, 261}},
        {25, {755, 590, 427, 341}},
        {30, {1003, 769, 573, 445}},
        {35, {1249, 959, 706, 552}},
        {40, {2953, 2331, 1663, 1273}},
    };

    int closest = 0;
    for (auto &t : table) {
        if (t.v <= version) closest = t.v;
    }

    int rawCap = 0;
    for (auto &t : table) {
        if (t.v == closest) {
            rawCap = t.caps[ec];
            break;
        }
    }

    // Account for base64 overhead: binary = floor(qrCap/4)*3
    int binaryCap = (rawCap / 4) * 3;
    int payload = binaryCap - 17; // header size
    if (payload < 1) payload = 1;

    int bitsPerMod = 1;
    if (grayLevels == 4) bitsPerMod = 2;
    if (grayLevels == 8) bitsPerMod = 3;
    payload *= bitsPerMod;

    double effective = payload * (1.0 - fecRatio);
    double syncOH = (syncN > 0) ? (1.0 - 1.0 / syncN) : 1.0;
    double videoBPS = effective * fps * syncOH;
    double audioBPS = audio ? (1200.0 / 8.0 * 0.8) : 0.0;
    double totalBPS = videoBPS + audioBPS;

    double sp = totalBPS * 7200;
    double lp = totalBPS * 14400;
    double ep = totalBPS * 21600;

    m_estPayload->setText(QString("%1 bytes").arg(int(effective)));
    m_estThroughput->setText(QString("%1 KB/s (video: %2, audio: %3)")
        .arg(totalBPS / 1024, 0, 'f', 1)
        .arg(videoBPS / 1024, 0, 'f', 1)
        .arg(audioBPS / 1024, 0, 'f', 1));
    m_estSP->setText(QString("%1 MB").arg(sp / 1024 / 1024, 0, 'f', 1));
    m_estLP->setText(QString("%1 MB").arg(lp / 1024 / 1024, 0, 'f', 1));
    m_estEP->setText(QString("%1 MB").arg(ep / 1024 / 1024, 0, 'f', 1));

    // Update chart
    auto *chart = m_chartView->chart();
    chart->removeAllSeries();

    auto *set = new QBarSet("Capacity (MB)");
    *set << sp / 1024 / 1024 << lp / 1024 / 1024 << ep / 1024 / 1024;

    auto *series = new QBarSeries;
    series->append(set);
    chart->addSeries(series);

    auto axes = chart->axes();
    for (auto *a : axes) chart->removeAxis(a);

    auto *axisX = new QBarCategoryAxis;
    axisX->append({"SP (2hr)", "LP (4hr)", "EP (6hr)"});
    chart->addAxis(axisX, Qt::AlignBottom);
    series->attachAxis(axisX);

    auto *axisY = new QValueAxis;
    axisY->setTitleText("MB");
    double maxVal = ep / 1024 / 1024;
    axisY->setRange(0, maxVal > 0 ? maxVal * 1.15 : 100);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisY);
}

void MainWindow::onProcessOutput()
{
    auto out = m_process->readAllStandardOutput();
    auto err = m_process->readAllStandardError();

    if (!out.isEmpty()) {
        QString text = QString::fromUtf8(out).trimmed();
        appendLog(text);

        // Try to detect the temp frame directory from encoder output
        // The Go encoder logs to stderr, but we check both.
        // Look for paths like /tmp/vhs-codec-frames-XXXXXXX
        if (m_frameTmpDir.isEmpty()) {
            // The encoder writes frame files but doesn't print the dir.
            // We scan /tmp for vhs-codec-frames-* directories.
        }
    }
    if (!err.isEmpty()) {
        QString text = QString::fromUtf8(err).trimmed();
        appendLog(text);
    }
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
    m_encodeBtn->setEnabled(true);
    m_decodeBtn->setEnabled(true);
    m_calibrateBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(exitCode == 0 ? 100 : 0);

    stopPreviewPolling();
    appendLog(QString("Process finished (exit code: %1)").arg(exitCode));

    if (exitCode == 0) {
        m_previewInfo->setText("Done");
    } else {
        m_previewInfo->setText("Error — see log");
    }
}

void MainWindow::startPreviewPolling()
{
    m_frameTmpDir.clear();
    m_lastPreviewFrame = -1;
    m_previewInfo->setText("Encoding...");
    // Poll at ~4Hz to find and display frames
    m_previewTimer->start(250);
}

void MainWindow::stopPreviewPolling()
{
    m_previewTimer->stop();
}

void MainWindow::onPreviewUpdate()
{
    // Find the encoder's temp frame directory
    if (m_frameTmpDir.isEmpty()) {
        QDir tmp("/tmp");
        auto dirs = tmp.entryList({"vhs-codec-frames-*"}, QDir::Dirs, QDir::Time);
        if (!dirs.isEmpty()) {
            m_frameTmpDir = "/tmp/" + dirs.first();
        } else {
            return;
        }
    }

    // Find the latest frame PNG in the directory
    QDir frameDir(m_frameTmpDir);
    if (!frameDir.exists()) {
        m_frameTmpDir.clear();
        return;
    }

    auto files = frameDir.entryList({"frame_*.png"}, QDir::Files, QDir::Name);
    if (files.isEmpty()) return;

    // Get the last file
    QString latestFile = m_frameTmpDir + "/" + files.last();

    // Extract frame number from filename
    static QRegularExpression re("frame_(\d+)\.png");
    auto match = re.match(files.last());
    int frameNum = match.hasMatch() ? match.captured(1).toInt() : -1;

    if (frameNum <= m_lastPreviewFrame) return; // already shown this one
    m_lastPreviewFrame = frameNum;

    // Load and display the frame
    QPixmap pix(latestFile);
    if (pix.isNull()) return;

    m_previewLabel->setPixmap(pix.scaled(
        m_previewLabel->size(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));

    m_previewInfo->setText(QString("Frame %1 / %2 files")
        .arg(frameNum)
        .arg(files.size()));
}

void MainWindow::appendLog(const QString &text)
{
    m_logOutput->appendPlainText(text);
}

QStringList MainWindow::buildEncodeArgs()
{
    QStringList args = {"encode"};
    args << "--input" << m_inputFile->text();

    if (!m_outputFile->text().isEmpty()) {
        args << "--output" << m_outputFile->text();
    } else {
        args << "--device" << m_videoDeviceOut->currentText();
    }

    args << "--qr-version" << QString::number(m_qrVersion->value());

    QString ecLevels[] = {"L", "M", "Q", "H"};
    args << "--ec-level" << ecLevels[m_ecLevel->currentIndex()];

    args << "--module-px" << QString::number(m_modulePx->value());

    int grayValues[] = {2, 4, 8};
    args << "--gray-levels" << QString::number(grayValues[m_grayLevels->currentIndex()]);

    args << "--fps" << QString::number(m_dataFPS->value(), 'f', 2);
    args << "--fec-ratio" << QString::number(m_fecRatio->value(), 'f', 2);
    args << "--sync-every" << QString::number(m_syncEvery->value());

    if (m_audioEnabled->isChecked()) {
        args << "--audio";
    }

    return args;
}

QStringList MainWindow::buildDecodeArgs()
{
    QStringList args = {"decode"};

    if (!m_decodeInput->text().isEmpty()) {
        args << "--file" << m_decodeInput->text();
    } else {
        args << "--device" << m_videoDeviceIn->currentText();
    }

    if (!m_decodeOutput->text().isEmpty()) {
        args << "--output" << m_decodeOutput->text();
    }

    if (m_audioEnabled->isChecked()) {
        args << "--audio";
    }

    return args;
}

QString MainWindow::findCLI()
{
    QStringList searchPaths = {
        QDir::currentPath() + "/vhs-codec",
        QDir::currentPath() + "/../vhs-codec",
        QStandardPaths::findExecutable("vhs-codec"),
    };

    for (const auto &path : searchPaths) {
        if (QFileInfo::exists(path)) return path;
    }

    return "vhs-codec";
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
        "This program is free software: you can redistribute it and/or modify "
        "it under the terms of the GNU General Public License as published by "
        "the Free Software Foundation, either version 3 of the License, or "
        "(at your option) any later version.</p>"

        "<p><b>Disclaimer:</b><br>"
        "THIS SOFTWARE IS EXPERIMENTAL AND PROVIDED &ldquo;AS IS&rdquo;, "
        "WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED. "
        "The authors shall not be held liable for any data loss, corruption, "
        "or damages arising from the use of this software. "
        "VHS tape is an analog medium with inherent reliability limitations. "
        "<b>Do not rely on this tool as your sole backup strategy.</b> "
        "Use at your own risk.</p>"

        "<p><a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
        "GNU General Public License v3.0</a></p>"
    );
}
