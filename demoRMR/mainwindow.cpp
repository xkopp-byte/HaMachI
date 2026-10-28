#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "robot.h"
#include <QEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QShortcut>
#include <cmath>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    installEventFilter(this);
    ui->cameraPlaceholderLabel->installEventFilter(this);
    ui->widget->installEventFilter(this);
    ui->lidarView->installEventFilter(this);
    lightStyle_ = styleSheet();
    // The shortcut restores the toolbar even when its button is hidden.
    auto controlsShortcut = new QShortcut(QKeySequence("Ctrl+H"), this);
    connect(controlsShortcut, &QShortcut::activated, ui->controlsToggleButton, &QPushButton::toggle);

    connect(ui->themeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyTheme);
    connect(ui->pushButton_9, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(ui->safetyStopButton, &QPushButton::toggled, this, &MainWindow::setSafetyStopActive);
    connect(ui->pushButton_4, &QPushButton::clicked, this, [this]{
        stopManualMotion();
        emit stopRequested();
    });
    // Existing demo command values are preserved; this window only emits requests.
    connect(ui->pushButton_2, &QPushButton::pressed, this, [this]{ requestMotion(ui->pushButton_2, 500, 0); });
    connect(ui->pushButton_3, &QPushButton::pressed, this, [this]{ requestMotion(ui->pushButton_3, -250, 0); });
    connect(ui->pushButton_6, &QPushButton::pressed, this, [this]{ requestMotion(ui->pushButton_6, 0, 3.14159/2); });
    connect(ui->pushButton_5, &QPushButton::pressed, this, [this]{ requestMotion(ui->pushButton_5, 0, -3.14159/2); });
    for (auto button : {ui->pushButton_2, ui->pushButton_3, ui->pushButton_6, ui->pushButton_5})
        connect(button, &QPushButton::released, this, [this, button]{
            if (activeManualButton_ == button) stopManualMotion();
        });
    const QString visibleToolbarText = ui->controlsToggleButton->text();
    connect(ui->controlsToggleButton, &QPushButton::toggled, this, [this, visibleToolbarText](bool visible){
        ui->controlsToggleButton->setText(visible ? visibleToolbarText
                : ui->controlsToggleButton->property("collapsedText").toString());
    });
    connect(ui->controlModeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::selectControlMode);
    connect(ui->controlModeComboBox, QOverload<int>::of(&QComboBox::activated), this, [this](int index){
        // Selecting the current entry also permits explicit re-enabling after a stop.
        if (controlSource_ == ControlSource::Disabled) selectControlMode(index);
    });
    connect(ui->previewSwapButton, &QPushButton::clicked, this, [this]{
        setMainView(parkingViewMain_ ? 0 : 1);
    });
    // Adapter connections: replace these when the project backend is ready.
    connect(this, &MainWindow::connectRequested, this, &MainWindow::startRobotSession);
    connect(this, &MainWindow::disconnectRequested, this, [this]{ endRobotSession(); });
    connect(this, &MainWindow::motionRequested, this, [this](double forward, double rotation){
        if (robot_ && connected_ && !connectionPending_ && !safetyStopActive_
                && controlSource_ == ControlSource::Manual && activeManualButton_
                && telemetryAge_.elapsed() < 2000)
            robot_->setSpeed(forward, rotation);
    });
    connect(this, &MainWindow::stopRequested, this, [this]{
        if (robot_ && connected_) robot_->setSpeed(0, 0);
    });
    connect(&sessionTimer_, &QTimer::timeout, this, &MainWindow::checkRobotSession);
    sessionTimer_.setInterval(200);
    setConnectionState(false);
    setSafetyStopActive(ui->safetyStopButton->isChecked());
    applyTheme(ui->themeComboBox->currentIndex());
}

MainWindow::~MainWindow() { endRobotSession(); delete ui; }

void MainWindow::toggleConnection()
{
    if (connectionPending_) return;
    if (connected_) {
        setControlSource(ControlSource::Disabled);
        connectionPending_ = true;
        ui->pushButton_9->setEnabled(false);
        updateControlAvailability();
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
    setControlSource(ControlSource::Disabled);
    connectionPending_ = true;
    updateControlAvailability();
    ui->lineEdit->setReadOnly(true);
    ui->pushButton_9->setEnabled(false);
    ui->connectionStatusLabel->setText("Connecting...");
    emit connectRequested(address, address.startsWith("127."));
}

void MainWindow::setConnectionState(bool connected, const QString &message)
{
    const bool changed = connected_ != connected;
    connected_ = connected;
    connectionPending_ = false;
    ui->pushButton_9->setEnabled(true);
    ui->pushButton_9->setText(connected ? "Disconnect" : "Connect");
    ui->lineEdit->setReadOnly(connected);
    if (!connected) setControlSource(ControlSource::Disabled);
    else if (changed) selectControlMode(ui->controlModeComboBox->currentIndex());
    updateControlAvailability();
    if (connected && safetyStopActive_) emit stopRequested();
    ui->connectionStatusLabel->setText(message.isEmpty() ? (connected ? "Connected" : "Disconnected") : message);
    ui->connectionDotLabel->setStyleSheet(connected ? "color: #16a34a;" : "color: #808890;");
    if (!connected) {
        setCameraImage(QImage());
        setLidarPoints({});
        hasLidarData_ = false;
        leftWheelSpeed_ = rightWheelSpeed_ = 0.0;
        ui->lidarPlaceholderLabel->show();
        ui->robotCenterLabel->show();
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
    if (active) setControlSource(ControlSource::Disabled);
    {
        const QSignalBlocker blocker(ui->safetyStopButton);
        ui->safetyStopButton->setChecked(active);
        ui->safetyStopButton->setText(active ? "SAFETY STOP\nON" : "SAFETY STOP\nOFF");
    }
    updateControlAvailability();
    if (changed) {
        emit safetyStopChanged(active);
    }
}

void MainWindow::setControlSource(ControlSource source)
{
    if (!connected_ || connectionPending_ || safetyStopActive_)
        source = ControlSource::Disabled;
#ifdef DISABLE_SKELETON
    if (source == ControlSource::Gestures) source = ControlSource::Disabled;
#endif

    const bool changed = controlSource_ != source;
    controlSource_ = ControlSource::Disabled;
    stopManualMotion();
    // Reuse the existing gesture enable switch; gesture recognition stays in the backend.
    if (robot_) robot_->setGestureControlEnabled(false);
    emit stopRequested();
    controlSource_ = source;
    updateControlAvailability();
    if (robot_ && source == ControlSource::Gestures)
        robot_->setGestureControlEnabled(true);
    if (changed) emit controlSourceChanged(source);
}

void MainWindow::selectControlMode(int index)
{
    setControlSource(index == 0 ? ControlSource::Gestures
                               : index == 1 ? ControlSource::Manual : ControlSource::Disabled);
}

void MainWindow::updateControlAvailability()
{
    const bool allowed = connected_ && !connectionPending_ && !safetyStopActive_;
    ui->controlModeComboBox->setEnabled(!connectionPending_ && !safetyStopActive_);
    const bool manual = allowed && controlSource_ == ControlSource::Manual;
    ui->manualGroup->setEnabled(manual);
    // Only the selected control mode controls the bottom panel, independently of the toolbar.
    ui->controlsPanel->setVisible(ui->controlModeComboBox->currentIndex() == 1);
}

void MainWindow::stopManualMotion()
{
    if (!activeManualButton_) return;
    auto button = activeManualButton_;
    activeManualButton_ = nullptr;
    button->setDown(false);
    emit stopRequested();
}

void MainWindow::requestMotion(QPushButton *button, double forward, double rotation)
{
    if (!connected_ || connectionPending_ || safetyStopActive_
            || controlSource_ != ControlSource::Manual || !ui->controlsPanel->isVisible()
            || !button->isEnabled() || !button->isDown()) return;
    if (activeManualButton_ && activeManualButton_ != button) stopManualMotion();
    activeManualButton_ = button;
    emit motionRequested(forward, rotation);
}

void MainWindow::setCameraImage(const QImage &image)
{
    // Own the pixels, including when the backend constructs a QImage over cv::Mat memory.
    cameraImage_ = image.copy();
    updateCameraLayout();
    ui->cameraPlaceholderLabel->setText(image.isNull() ? "No image data" : "");
    ui->cameraPlaceholderLabel->update();
}

void MainWindow::setLidarPoints(const QVector<QPointF> &points)
{
    lidarPoints_ = points;
    if (!hasLidarData_ && !points.isEmpty()) {
        hasLidarData_ = true;
        // The painted robot replaces the static placeholder symbols.
        ui->lidarPlaceholderLabel->hide();
        ui->robotCenterLabel->hide();
    }
    ui->lidarView->update();
}

// Robot-centred top view: the robot stays in the middle facing up (Front)
// and the scan points move around it.
void MainWindow::paintLidarView()
{
    constexpr double viewRangeMm = 2000.0;   // distance shown at the view edge
    constexpr double robotRadiusMm = 177.0;  // Kobuki base radius

    QWidget *view = ui->lidarView;
    QPainter painter(view);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(view->rect(), QColor(13, 35, 53));

    const QPointF center(view->width() / 2.0, view->height() / 2.0);
    const double radiusPx = qMin(view->width(), view->height()) / 2.0 - 4.0;
    if (radiusPx <= 0.0) return;
    const double scale = radiusPx / viewRangeMm;

    // Range rings every half metre.
    painter.setPen(QPen(QColor(49, 73, 94), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    for (double ringMm = 500.0; ringMm <= viewRangeMm; ringMm += 500.0)
        painter.drawEllipse(center, ringMm * scale, ringMm * scale);

    // Same colour scale as the camera overlay: pale blue at 2 m, strong blue up close.
    painter.setPen(Qt::NoPen);
    for (const QPointF &p : lidarPoints_) {
        const double distance = std::hypot(p.x(), p.y());
        if (distance > viewRangeMm) continue;
        const double closeness = 1.0 - distance / viewRangeMm;
        painter.setBrush(QColor(qRound(200 - 200 * closeness),
                                qRound(220 - 180 * closeness), 255));
        painter.drawEllipse(QPointF(center.x() + p.y() * scale,
                                    center.y() - p.x() * scale), 1.8, 1.8);
    }

    // Predicted wheel tracks for the next 2 seconds at the commanded speeds.
    if (std::abs(leftWheelSpeed_) >= 1.0 || std::abs(rightWheelSpeed_) >= 1.0) {
        QVector<QPointF> leftPath, rightPath;
        robot::predictWheelPaths(leftWheelSpeed_, rightWheelSpeed_, 2.0, leftPath, rightPath);
        painter.setPen(QPen(QColor(0, 0, 255), 2, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        for (const QVector<QPointF> *path : {&leftPath, &rightPath}) {
            QPolygonF line;
            for (const QPointF &p : *path)
                line << QPointF(center.x() + p.y() * scale, center.y() - p.x() * scale);
            painter.drawPolyline(line);
        }
    }

    // Kobuki: round base with a heading mark pointing forward.
    const double robotPx = qMax(6.0, robotRadiusMm * scale);
    painter.setPen(QPen(QColor(226, 231, 236), 1.5));
    painter.setBrush(QColor(60, 72, 84));
    painter.drawEllipse(center, robotPx, robotPx);
    painter.drawLine(center, center - QPointF(0, robotPx));
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
    ui->previewLayout->removeWidget(ui->cameraPlaceholderLabel);
    ui->previewLayout->removeWidget(ui->parkingAssistantGroup);
    if (parkingViewMain_) {
        ui->parkingAssistantGroup->setMinimumSize(0, 0);
        ui->parkingAssistantGroup->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        ui->parkingAssistantGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->cameraOverlayLayout->addWidget(ui->parkingAssistantGroup, 0, 0, 3, 3);
        ui->previewLayout->insertWidget(0, ui->cameraPlaceholderLabel, 0, Qt::AlignRight);
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
        ui->previewLayout->insertWidget(0, ui->parkingAssistantGroup, 0, Qt::AlignRight);
        ui->cameraPlaceholderLabel->lower();
        ui->parkingAssistantGroup->raise();
    }
    ui->cameraPlaceholderLabel->show();
    ui->parkingAssistantGroup->show();
    ui->previewSwapButton->setToolTip(ui->previewSwapButton->property(
            parkingViewMain_ ? "cameraToolTip" : "parkingToolTip").toString());
    ui->previewSwapButton->raise();
    ui->previewPanel->raise();
    for (QWidget *overlay : {static_cast<QWidget*>(ui->warningFrame),
                             static_cast<QWidget*>(ui->dangerFrame),
                             static_cast<QWidget*>(ui->frontObstacleIndicator),
                             static_cast<QWidget*>(ui->rearObstacleIndicator),
                             static_cast<QWidget*>(ui->leftObstacleIndicator),
                             static_cast<QWidget*>(ui->rightObstacleIndicator)})
        overlay->raise();
    ui->safetyStopOverlay->raise();
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
    if (parkingViewMain_) {
        // A landscape camera needs a wider budget than the square LiDAR minimap.
        const int previewWidth = qBound(240, qRound(mainSize.width() * 0.28), 420);
        const int previewHeight = qBound(120, qRound(mainSize.height() * 0.45), 260);
        const QSize budget(previewWidth, previewHeight);
        const QSize fittedPreview = (cameraImage_.isNull() ? QSize(16, 9) : cameraImage_.size())
                .scaled(budget, Qt::KeepAspectRatio);
        const QSize preview(qMax(140, fittedPreview.width()), fittedPreview.height());
        if (ui->cameraPlaceholderLabel->minimumSize() != preview
                || ui->cameraPlaceholderLabel->maximumSize() != preview)
            ui->cameraPlaceholderLabel->setFixedSize(preview);
    } else {
        // Keep the square LiDAR minimap proportional to the main camera's height.
        const int edge = qBound(140, qRound(qMin(mainSize.width(), mainSize.height()) * 0.30), 230);
        const QSize preview(edge, edge);
        if (ui->parkingAssistantGroup->minimumSize() != preview
                || ui->parkingAssistantGroup->maximumSize() != preview)
            ui->parkingAssistantGroup->setFixedSize(preview);
    }
    ui->previewLayout->activate();
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
    if (watched == ui->lidarView && event->type() == QEvent::Paint) {
        paintLidarView();
        return true;
    }
    if (watched == this && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Close))
        setControlSource(ControlSource::Disabled);
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
    connect(robot_, &robot::publishLidar, this, [this, id](const LaserMeasurement &scan){
        if (id != sessionId_ || !robot_) return;
        // RPLidar: angle in degrees clockwise from forward, distance in mm.
        QVector<QPointF> points;
        points.reserve(scan.numberOfScans);
        for (int i = 0; i < scan.numberOfScans && i < 1000; ++i) {
            const LaserData &d = scan.Data[i];
            if (!std::isfinite(d.scanDistance) || !std::isfinite(d.scanAngle) || d.scanDistance <= 0.0f)
                continue;
            const double angle = d.scanAngle * 3.14159265358979323846 / 180.0;
            points.append(QPointF(d.scanDistance * std::cos(angle), d.scanDistance * std::sin(angle)));
        }
        setLidarPoints(points);
    }, Qt::QueuedConnection);
#ifndef DISABLE_SKELETON
    connect(robot_, &robot::viewToggleGesture, this, [this, id]{
        if (id != sessionId_ || !robot_) return;
        // Same effect as picking the other entry in the View combo box.
        ui->viewComboBox->setCurrentIndex(ui->viewComboBox->currentIndex() == 0 ? 1 : 0);
    }, Qt::QueuedConnection);
#endif
    connect(robot_, &robot::publishWheelSpeeds, this, [this, id](double left, double right){
        if (id != sessionId_ || !robot_) return;
        leftWheelSpeed_ = left;
        rightWheelSpeed_ = right;
        ui->lidarView->update();
    }, Qt::QueuedConnection);
    ui->viewComboBox->setEnabled(true);
    robot_->lidarFusion(true);
    robot_->initAndStartRobot(address.toStdString());
    sessionTimer_.start();
}

void MainWindow::endRobotSession(const QString &message)
{
    setControlSource(ControlSource::Disabled);
    sessionTimer_.stop();
    ++sessionId_; // Ignore queued frames/telemetry from the old session.
    if (robot_) {
        if (connected_) {
            robot_->setSpeed(0, 0);
        }
        robot_->stopRobot();
        delete robot_;
        robot_ = nullptr;
    }
    imageAge_.invalidate();
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
