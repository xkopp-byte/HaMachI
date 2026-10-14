#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "robot.h"
#include <QEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QShortcut>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    installEventFilter(this);
    ui->cameraPlaceholderLabel->installEventFilter(this);
    ui->widget->installEventFilter(this);
    lightStyle_ = styleSheet();
    // Ctrl+H remains available when its button is inside the collapsed settings.
    auto hiddenControlsShortcut = new QShortcut(QKeySequence("Ctrl+H"), this);
    hiddenControlsShortcut->setEnabled(!ui->headerToggleButton->isChecked());
    connect(hiddenControlsShortcut, &QShortcut::activated, ui->controlsToggleButton, &QPushButton::toggle);
    connect(ui->headerToggleButton, &QPushButton::toggled, this, [this, hiddenControlsShortcut](bool visible){
        ui->headerToggleButton->setText(visible ? "Hide settings" : "Show settings");
        hiddenControlsShortcut->setEnabled(!visible);
    });

    connect(ui->themeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyTheme);
    connect(ui->pushButton_9, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(ui->safetyStopButton, &QPushButton::toggled, this, &MainWindow::setSafetyStopActive);
    connect(ui->pushButton_4, &QPushButton::clicked, this, &MainWindow::stopRequested);
    // Existing demo command values are preserved; this window only emits requests.
    connect(ui->pushButton_2, &QPushButton::pressed, this, [this]{ requestMotion(500, 0); });
    connect(ui->pushButton_3, &QPushButton::pressed, this, [this]{ requestMotion(-250, 0); });
    connect(ui->pushButton_6, &QPushButton::pressed, this, [this]{ requestMotion(0, 3.14159/2); });
    connect(ui->pushButton_5, &QPushButton::pressed, this, [this]{ requestMotion(0, -3.14159/2); });
    for (auto button : {ui->pushButton_2, ui->pushButton_3, ui->pushButton_6, ui->pushButton_5})
        connect(button, &QPushButton::released, this, &MainWindow::stopRequested);
    connect(ui->controlsToggleButton, &QPushButton::toggled, this, [this](bool visible){
        if (!visible) emit stopRequested();
    });
    connect(ui->viewComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::setMainView);
    // Adapter connections: replace these when the project backend is ready.
    connect(this, &MainWindow::connectRequested, this, &MainWindow::startRobotSession);
    connect(this, &MainWindow::disconnectRequested, this, [this]{ endRobotSession(); });
    connect(this, &MainWindow::motionRequested, this, [this](double forward, double rotation){
        if (robot_ && connected_ && !safetyStopActive_ && telemetryAge_.elapsed() < 2000)
            robot_->setSpeed(forward, rotation);
    });
    connect(this, &MainWindow::stopRequested, this, [this]{
        if (robot_ && connected_) robot_->setSpeed(0, 0);
    });
    connect(&sessionTimer_, &QTimer::timeout, this, &MainWindow::checkRobotSession);
    sessionTimer_.setInterval(200);
    ui->viewComboBox->setToolTip("Select the large view; the other view stays in the top-right preview.");
    setConnectionState(false);
    setSafetyStopActive(ui->safetyStopButton->isChecked());
    applyTheme(ui->themeComboBox->currentIndex());
}

MainWindow::~MainWindow() { endRobotSession(); delete ui; }

void MainWindow::toggleConnection()
{
    if (connectionPending_) return;
    if (connected_) {
        connectionPending_ = true;
        ui->pushButton_9->setEnabled(false);
        ui->manualGroup->setEnabled(false);
        ui->connectionStatusLabel->setText("Disconnecting...");
        emit stopRequested();
        emit disconnectRequested();
        return;
    }
    const QString address = ui->lineEdit->text().trimmed();
    const QStringList octets = address.split('.');
    bool valid = octets.size() == 4;
    for (const QString &octet : octets) {
        bool ok = false;
        const int value = octet.toInt(&ok);
        valid = valid && ok && value >= 0 && value <= 255
                && QRegularExpression("^[0-9]{1,3}$").match(octet).hasMatch();
    }
    if (!valid) {
        QMessageBox::warning(this, "IP address", "Enter an IPv4 address, for example 127.0.0.1.");
        return;
    }
    connectionPending_ = true;
    ui->lineEdit->setReadOnly(true);
    ui->pushButton_9->setEnabled(false);
    ui->connectionStatusLabel->setText("Connecting...");
    emit connectRequested(address, address.startsWith("127."));
}

void MainWindow::setConnectionState(bool connected, const QString &message)
{
    connected_ = connected;
    connectionPending_ = false;
    ui->pushButton_9->setEnabled(true);
    ui->pushButton_9->setText(connected ? "Disconnect" : "Connect");
    ui->lineEdit->setReadOnly(connected);
    ui->manualGroup->setEnabled(connected && !safetyStopActive_);
    if (connected && safetyStopActive_) emit stopRequested();
    ui->connectionStatusLabel->setText(message.isEmpty() ? (connected ? "Connected" : "Disconnected") : message);
    ui->connectionDotLabel->setStyleSheet(connected ? "color: #16a34a;" : "color: #808890;");
    if (!connected) {
        setCameraImage(QImage());
        setWarning(QString());
        setDanger(QString());
        setObstacleIndicators(false, false, false, false);
    }
}

// Latched UI stop: switching OFF only permits future commands; it never resumes motion.
void MainWindow::setSafetyStopActive(bool active)
{
    const bool changed = safetyStopActive_ != active;
    safetyStopActive_ = active;
    {
        const QSignalBlocker blocker(ui->safetyStopButton);
        ui->safetyStopButton->setChecked(active);
        ui->safetyStopButton->setText(active ? "SAFETY STOP ON" : "SAFETY STOP OFF");
    }
    ui->manualGroup->setEnabled(connected_ && !connectionPending_ && !active);
    if (changed) {
        if (active) emit stopRequested();
        emit safetyStopChanged(active);
    }
}

void MainWindow::requestMotion(double forward, double rotation)
{
    if (connected_ && !connectionPending_ && !safetyStopActive_) emit motionRequested(forward, rotation);
}

void MainWindow::setCameraImage(const QImage &image)
{
    // Own the pixels, including when the backend constructs a QImage over cv::Mat memory.
    cameraImage_ = image.copy();
    updateCameraLayout();
    ui->cameraPlaceholderLabel->setText(image.isNull() ? "No image data" : "");
    ui->cameraPlaceholderLabel->update();
}

void MainWindow::setWarning(const QString &message)
{
    warningText_ = message;
    ui->warningMessageLabel->setText(message);
    ui->warningFrame->setVisible(!message.isEmpty() && ui->dangerFrame->isHidden());
}

void MainWindow::setDanger(const QString &message)
{
    ui->dangerMessageLabel->setText(message);
    ui->dangerFrame->setVisible(!message.isEmpty());
    ui->warningFrame->setVisible(message.isEmpty() && !warningText_.isEmpty());
}

void MainWindow::setObstacleIndicators(bool front, bool rear, bool left, bool right)
{
    ui->frontObstacleIndicator->setVisible(front);
    ui->rearObstacleIndicator->setVisible(rear);
    ui->leftObstacleIndicator->setVisible(left);
    ui->rightObstacleIndicator->setVisible(right);
}

void MainWindow::applyTheme(int index)
{
    // Theme definitions stay in the .ui; C++ only selects a stored stylesheet.
    const QString darkOverrides = property("darkStyleSheet").toString();
    setStyleSheet(lightStyle_ + (index == 1 ? darkOverrides : QString()));
}

// Swap presentation only; keep the incoming camera stream and connection alive.
void MainWindow::setMainView(int index)
{
    parkingViewMain_ = index == 1;
    ui->cameraOverlayLayout->removeWidget(ui->cameraPlaceholderLabel);
    ui->cameraOverlayLayout->removeWidget(ui->parkingAssistantGroup);
    if (parkingViewMain_) {
        ui->parkingAssistantGroup->setMinimumSize(0, 0);
        ui->parkingAssistantGroup->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        ui->parkingAssistantGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->cameraOverlayLayout->addWidget(ui->parkingAssistantGroup, 0, 0, 3, 3);
        ui->cameraOverlayLayout->addWidget(ui->cameraPlaceholderLabel, 0, 2,
                                           Qt::AlignRight | Qt::AlignTop);
        ui->parkingAssistantGroup->lower();
        ui->cameraPlaceholderLabel->raise();
    } else {
        ui->cameraPlaceholderLabel->setMinimumSize(0, 0);
        ui->cameraPlaceholderLabel->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        ui->cameraPlaceholderLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->parkingAssistantGroup->setMinimumSize(140, 140);
        ui->parkingAssistantGroup->setMaximumSize(230, 230);
        ui->parkingAssistantGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        ui->cameraOverlayLayout->addWidget(ui->cameraPlaceholderLabel, 0, 0, 3, 3);
        ui->cameraOverlayLayout->addWidget(ui->parkingAssistantGroup, 0, 2,
                                           Qt::AlignRight | Qt::AlignTop);
        ui->cameraPlaceholderLabel->lower();
        ui->parkingAssistantGroup->raise();
    }
    for (QWidget *overlay : {static_cast<QWidget*>(ui->warningFrame),
                             static_cast<QWidget*>(ui->dangerFrame),
                             static_cast<QWidget*>(ui->frontObstacleIndicator),
                             static_cast<QWidget*>(ui->rearObstacleIndicator),
                             static_cast<QWidget*>(ui->leftObstacleIndicator),
                             static_cast<QWidget*>(ui->rightObstacleIndicator)})
        overlay->raise();
    updateCameraLayout();
    ui->cameraOverlayLayout->activate();
    ui->cameraPlaceholderLabel->update();
    emit mainViewChanged(parkingViewMain_);
}

// Fit the camera area and size its preview relative to the actual main view.
void MainWindow::updateCameraLayout()
{
    const int border = 14; // Matches the viewport margins defined in the .ui.
    QMargins margins(border, border, border, border);
    QSize mainSize = ui->widget->size() - QSize(2 * border, 2 * border);
    if (mainSize.width() <= 0 || mainSize.height() <= 0) return;
    if (!parkingViewMain_ && !cameraImage_.isNull()) {
        const QSize fitted = cameraImage_.size().scaled(mainSize, Qt::KeepAspectRatio);
        const int extraWidth = mainSize.width() - fitted.width();
        const int extraHeight = mainSize.height() - fitted.height();
        margins = QMargins(border + extraWidth / 2, border + extraHeight / 2,
                           border + extraWidth - extraWidth / 2,
                           border + extraHeight - extraHeight / 2);
        mainSize = fitted;
    }
    // About 30% of the shorter side; bounds keep the preview readable and unobtrusive.
    const int edge = qBound(140, qRound(qMin(mainSize.width(), mainSize.height()) * 0.30), 230);
    if (parkingViewMain_) {
        const QSize preview = cameraImage_.isNull() ? QSize(edge, qRound(edge * 2.0 / 3.0))
                : cameraImage_.size().scaled(QSize(edge, edge), Qt::KeepAspectRatio);
        if (ui->cameraPlaceholderLabel->minimumSize() != preview
                || ui->cameraPlaceholderLabel->maximumSize() != preview)
            ui->cameraPlaceholderLabel->setFixedSize(preview);
    } else {
        const QSize preview(edge, edge);
        if (ui->parkingAssistantGroup->minimumSize() != preview
                || ui->parkingAssistantGroup->maximumSize() != preview)
            ui->parkingAssistantGroup->setFixedSize(preview);
    }
    if (ui->cameraOverlayLayout->contentsMargins() != margins)
        ui->cameraOverlayLayout->setContentsMargins(margins);
    ui->cameraOverlayLayout->activate();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->widget && event->type() == QEvent::Resize)
        updateCameraLayout();
    if (watched == ui->cameraPlaceholderLabel && event->type() == QEvent::Paint && !cameraImage_.isNull()) {
        QPainter painter(ui->cameraPlaceholderLabel);
        painter.fillRect(ui->cameraPlaceholderLabel->rect(), Qt::black);
        const QSize size = cameraImage_.size().scaled(ui->cameraPlaceholderLabel->size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((ui->cameraPlaceholderLabel->width()-size.width())/2,
                                  (ui->cameraPlaceholderLabel->height()-size.height())/2), size);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, cameraImage_);
        return true;
    }
    if (watched == this && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Close))
        emit stopRequested();
    return QMainWindow::eventFilter(watched, event);
}

// Both the simulator and real robot use the original protocol, ports and camera URL.
void MainWindow::startRobotSession(const QString &address, bool simulation)
{
    Q_UNUSED(simulation);
    if (robot_) return;
    robot_ = new robot();
    const unsigned id = ++sessionId_;
    telemetryAge_.start();
    imageAge_.invalidate();
    connect(robot_, &robot::telemetryReceived, this, [this, id]{
        if (id != sessionId_ || !robot_) return;
        telemetryAge_.restart();
        if (!connected_) setConnectionState(true);
    }, Qt::QueuedConnection);
#ifndef DISABLE_OPENCV
    connect(robot_, &robot::publishCamera, this, [this, id](const cv::Mat &frame){
        if (id != sessionId_ || !robot_ || frame.empty() || frame.depth() != CV_8U) return;
        cv::Mat rgb;
        if (frame.channels() == 3) cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
        else if (frame.channels() == 4) cv::cvtColor(frame, rgb, cv::COLOR_BGRA2RGB);
        else if (frame.channels() == 1) cv::cvtColor(frame, rgb, cv::COLOR_GRAY2RGB);
        else return;
        setCameraImage(QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888));
        imageAge_.restart();
    }, Qt::QueuedConnection);
#else
    ui->cameraPlaceholderLabel->setText("Camera unavailable: OpenCV disabled");
#endif
    ui->viewComboBox->setEnabled(true);
    robot_->initAndStartRobot(address.toStdString());
    sessionTimer_.start();
}

void MainWindow::endRobotSession(const QString &message)
{
    sessionTimer_.stop();
    ++sessionId_; // Ignore queued frames/telemetry from the old session.
    if (robot_) {
        if (connected_) robot_->setSpeed(0, 0);
        robot_->stopRobot();
        delete robot_;
        robot_ = nullptr;
    }
    imageAge_.invalidate();
    ui->viewComboBox->setEnabled(true);
    setConnectionState(false, message);
}

void MainWindow::checkRobotSession()
{
    if (!robot_) return;
    const int timeout = connected_ ? 2000 : 5000;
    if (telemetryAge_.elapsed() > timeout) {
        endRobotSession(connected_ ? "Connection lost" : "No robot data - retry Connect");
        return;
    }
    if (imageAge_.isValid() && imageAge_.elapsed() > 3000) {
        setCameraImage(QImage());
        imageAge_.invalidate();
    }
}
