#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "robot.h"
#include <QEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    installEventFilter(this);
    ui->cameraPlaceholderLabel->installEventFilter(this);
    lightStyle_ = styleSheet();
    connect(ui->themeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyTheme);
    connect(ui->pushButton_9, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(ui->safetyStopButton, &QPushButton::clicked, this, &MainWindow::stopRequested);
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
            this, [this](int index){
        setCameraImage(QImage());
        emit viewSourceRequested(index == 1);
    });
    // Adapter connections: replace these when the project backend is ready.
    connect(this, &MainWindow::connectRequested, this, &MainWindow::startRobotSession);
    connect(this, &MainWindow::disconnectRequested, this, [this]{ endRobotSession(); });
    connect(this, &MainWindow::motionRequested, this, [this](double forward, double rotation){
        if (robot_ && connected_ && telemetryAge_.elapsed() < 2000)
            robot_->setSpeed(forward, rotation);
    });
    connect(this, &MainWindow::stopRequested, this, [this]{
        if (robot_ && connected_) robot_->setSpeed(0, 0);
    });
    connect(&sessionTimer_, &QTimer::timeout, this, &MainWindow::checkRobotSession);
    sessionTimer_.setInterval(200);
    ui->viewComboBox->setToolTip("Both sources use the selected IP: local simulator 127.0.0.1 or the real robot address.");
    setConnectionState(false);
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
    emit connectRequested(address, ui->viewComboBox->currentIndex() == 1);
}

void MainWindow::setConnectionState(bool connected, const QString &message)
{
    connected_ = connected;
    connectionPending_ = false;
    ui->pushButton_9->setEnabled(true);
    ui->pushButton_9->setText(connected ? "Disconnect" : "Connect");
    ui->lineEdit->setReadOnly(connected);
    ui->manualGroup->setEnabled(connected);
    ui->connectionStatusLabel->setText(message.isEmpty() ? (connected ? "Connected" : "Disconnected") : message);
    ui->connectionDotLabel->setStyleSheet(connected ? "color: #16a34a;" : "color: #808890;");
    if (!connected) {
        setCameraImage(QImage());
        setWarning(QString());
        setDanger(QString());
        setObstacleIndicators(false, false, false, false);
    }
}

void MainWindow::requestMotion(double forward, double rotation)
{
    if (connected_ && !connectionPending_) emit motionRequested(forward, rotation);
}

void MainWindow::setCameraImage(const QImage &image)
{
    // Own the pixels, including when the backend constructs a QImage over cv::Mat memory.
    cameraImage_ = image.copy();
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->cameraPlaceholderLabel && event->type() == QEvent::Paint && !cameraImage_.isNull()) {
        QPainter painter(ui->cameraPlaceholderLabel);
        painter.fillRect(ui->cameraPlaceholderLabel->rect(), Qt::black);
        const QSize size = cameraImage_.size().scaled(ui->cameraPlaceholderLabel->size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((ui->cameraPlaceholderLabel->width()-size.width())/2,
                                  (ui->cameraPlaceholderLabel->height()-size.height())/2), size);
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
    ui->viewComboBox->setEnabled(false);
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
