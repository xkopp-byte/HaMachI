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
namespace Ui { class MainWindow; }

// UI interface plus a replaceable adapter to the supplied robot communication.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
public slots:
    void setConnectionState(bool connected, const QString &message = QString());
    void setCameraImage(const QImage &image);
    void setWarning(const QString &message);
    void setDanger(const QString &message);
    void setObstacleIndicators(bool front, bool rear, bool left, bool right);
signals:
    void connectRequested(const QString &ipAddress, bool simulation);
    void disconnectRequested();
    void stopRequested();
    void motionRequested(double forwardSpeed, double rotationSpeed);
    void viewSourceRequested(bool simulation);
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
    void requestMotion(double forward, double rotation);
    void applyTheme(int index);
    Ui::MainWindow *ui;
    bool connected_ = false;
    bool connectionPending_ = false;
    QImage cameraImage_;
    QString lightStyle_;
    QString warningText_;
};
#endif
