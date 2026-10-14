#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QMainWindow>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
class robot;
class QPushButton;
namespace Ui { class MainWindow; }

// UI interface plus a replaceable adapter to the supplied robot communication.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum class ControlSource { Disabled, Manual, Gestures };
    Q_ENUM(ControlSource)
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool isSafetyStopActive() const { return safetyStopActive_; }
    ControlSource controlSource() const { return controlSource_; }
public slots:
    void setSafetyStopActive(bool active);
    void setConnectionState(bool connected, const QString &message = QString());
    void setCameraImage(const QImage &image);
    void setWarning(const QString &message);
    void setDanger(const QString &message);
    void setObstacleIndicators(bool front, bool rear, bool left, bool right);
signals:
    void safetyStopChanged(bool active);
    void controlSourceChanged(MainWindow::ControlSource source);
    void connectRequested(const QString &ipAddress, bool simulation);
    void disconnectRequested();
    void stopRequested();
    void motionRequested(double forwardSpeed, double rotationSpeed);
    void viewSourceRequested(bool simulation); // Legacy source selection; the preview now selects layout.
    void mainViewChanged(bool parkingAssistantMain);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void toggleConnection();
    void startRobotSession(const QString &address, bool simulation);
    void endRobotSession(const QString &message = QString());
    void checkRobotSession();
    robot *robot_ = nullptr;
    unsigned sessionId_ = 0;
    QTimer sessionTimer_;
    QElapsedTimer telemetryAge_;
    QElapsedTimer imageAge_;
    void setControlSource(ControlSource source);
    void selectControlMode(int index);
    void updateControlAvailability();
    void stopManualMotion();
    void requestMotion(QPushButton *button, double forward, double rotation);
    ControlSource controlSource_ = ControlSource::Disabled;
    QPushButton *activeManualButton_ = nullptr;
    void applyTheme(int index);
    void updateCameraLayout();
    void setMainView(int index);
    bool parkingViewMain_ = false;
    Ui::MainWindow *ui;
    bool safetyStopActive_ = false;
    bool connected_ = false;
    bool connectionPending_ = false;
    QImage cameraImage_;
    QString lightStyle_;
    QString warningText_;
};
#endif
