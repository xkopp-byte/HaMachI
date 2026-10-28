#ifndef ROBOT_H
#define ROBOT_H

#include <QObject>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <QPointF>
#include <QVector>
#include "librobot.h"
#ifndef DISABLE_OPENCV
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/core/utility.hpp"
#include "opencv2/videoio.hpp"
#include "opencv2/imgcodecs.hpp"


Q_DECLARE_METATYPE(cv::Mat)
#endif
#ifndef DISABLE_SKELETON
Q_DECLARE_METATYPE(skeleton)
#endif
Q_DECLARE_METATYPE(LaserMeasurement)
class robot : public QObject
{
    Q_OBJECT
public:
    bool startLoging;
    explicit robot(QObject *parent = nullptr);
    ~robot() override;

    void initAndStartRobot(std::string ipaddress);
    void stopRobot() { robotCom.robotStop(); }

    //tato funkcia len nastavuje hodnoty.. posielaju sa v callbacku(dobre, kvoli asynchronnosti a zabezpeceniu,ze sa poslu len raz pri viacero prepisoch vramci callu)
    void setSpeedVal(double forw,double rots);
    //tato funkcia fyzicky posiela hodnoty do robota
    void setSpeed(double forw,double rots);
    void setGestureControlEnabled(bool enabled);
    void lidarFusion(bool enabled);
    // Wheel tracks (robot frame, mm: x forward, y right) for the given wheel
    // speeds (mm/s) held for `seconds`, starting at the current wheel positions.
    static void predictWheelPaths(double leftSpeed, double rightSpeed, double seconds,
                                  QVector<QPointF> &leftPath, QVector<QPointF> &rightPath);
signals:
    void publishWheelSpeeds(double leftSpeed, double rightSpeed);
    void telemetryReceived();
    void publishPosition(double x, double y, double z);
    void publishAMCLPosition(double x, double y, double z);
    void publishLidar(const LaserMeasurement &lidata);
    #ifndef DISABLE_OPENCV
    void publishCamera(const cv::Mat &camframe);
#endif
#ifndef DISABLE_SKELETON
void publishSkeleton(const skeleton &skeledata);
void viewToggleGesture(); // thumbs + pinkies square, at most once per 3 s
#endif
private:
    std::mutex commandMutex_;

    /// toto su vase premenne na vasu odometriu
    double x;
    double y;
    double fi;


///-----------------------------
/// toto su rychlosti ktore sa nastavuju setSpeedVal a posielaju v processThisRobot
    double forwardspeed;//mm/s
    double rotationspeed;//omega/s

    ///toto su callbacky co sa sa volaju s novymi datami
    int processThisLidar(LaserMeasurement laserData);
    int processThisRobot(TKobukiData robotdata);
    int processThisAMCLPosition(float x,float y,float theta);
    #ifndef DISABLE_OPENCV
    int processThisCamera(cv::Mat cameraData);
    cv::Mat projectLidarToCamera(const cv::Mat &cameraData,
                                 const LaserMeasurement &laserData) const;
    void drawPredictedPath(cv::Mat &image, double leftSpeed, double rightSpeed) const;
#endif
    void setCommandedWheels(double forward, double rotation); // caller holds commandMutex_
    // Sends the speed to the robot, halved near obstacles. Caller holds commandMutex_.
    void sendSpeed(double forw, double rots);
    double commandedLeft_=0.0;  // mm/s, guarded by commandMutex_
    double commandedRight_=0.0; // mm/s, guarded by commandMutex_
    double directForward_=0.0;  // last setSpeed request, guarded by commandMutex_
    double directRotation_=0.0; // guarded by commandMutex_
    bool sentWithLimit_=false;  // speed limit state of the last sendSpeed, guarded by commandMutex_
    std::atomic<bool> obstacleNear_{false}; // a LiDAR point is closer than 0.5 m


    ///pomocne strukutry aby ste si trosku nerobili race conditions
    LaserMeasurement copyOfLaserData;
        std::mutex lidarMutex_;
        bool lidarFusionEnabled_=false;
    #ifndef DISABLE_OPENCV
        cv::Mat frame[3];
    #endif
    ///classa ktora riesi komunikaciu s robotom
    libRobot robotCom;


    ///pomocne premenne... moc nerieste naco su
    int datacounter;
    #ifndef DISABLE_OPENCV
    bool useCamera1;
    int actIndex;
#endif


#ifndef DISABLE_SKELETON
int processThisSkeleton(skeleton skeledata);
void updateGestureSpeeds();
static bool isViewSquareGesture(const skeleton &data);
std::chrono::steady_clock::time_point lastViewToggle_{};
std::mutex skeletonMutex_;
skeleton skeleJoints{};
bool gestureControlEnabled_;
double leftWheelSpeed_;
double rightWheelSpeed_;
std::chrono::steady_clock::time_point gestureRampTime_;
#endif
    int useDirectCommands;
#ifndef DISABLE_AMCL
public:
    Particle &getBestParticle(){return robotCom.getBestParticle();}
    GridMap &getAmclMap(){return robotCom.getAmclMap();}
    void getGridCoordinates(double realX,double realY, int &gX,int &gY){robotCom.getGridCoordinates(realX, realY, gX, gY);}

#endif

};

#endif // ROBOT_H
