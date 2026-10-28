#include "robot.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMaximumGestureSpeed=400.0; // mm/s = 0.4 m/s
constexpr double kBackwardModeDistance=0.05;
// Index knuckle-to-tip length relative to palm length (wrist -> middle MCP).
constexpr double kFingerCurledRatio=0.25;   // at or below: 0 speed
constexpr double kFingerExtendedRatio=0.80; // at or above: full speed
constexpr double kWheelBase=230.0; // mm between Kobuki wheels
constexpr double kPredictionSeconds=2.0;
// View-switch gesture: thumb and pinky tips of both hands form a square.
constexpr double kSquareSideTolerance=0.20; // each side within +-20% of the mean
constexpr double kSquareMinimumSide=0.05;   // normalised; rejects untracked (0,0) hands
constexpr double kViewToggleCooldownSeconds=3.0;

#ifndef DISABLE_OPENCV
// Camera model shared by the LiDAR overlay and the predicted path.
constexpr double kFocalLength=681.743;
constexpr double kLidarBehindCamera=145.0;
constexpr double kLidarAboveCamera=95.0;

// Projects a point in the LiDAR plane (robot frame, mm: x forward, y right).
bool projectToCamera(const cv::Mat &image, double x, double y, cv::Point &pixel)
{
    const double denominator=x+kLidarBehindCamera;
    if (denominator<=1.0)
        return false;
    pixel=cv::Point(
        static_cast<int>(std::lround(image.cols/2.0+kFocalLength*y/denominator)),
        static_cast<int>(std::lround(image.rows/2.0+kFocalLength*kLidarAboveCamera/denominator)));
    return true;
}
#endif
}

void robot::predictWheelPaths(double leftSpeed, double rightSpeed, double seconds,
                              QVector<QPointF> &leftPath, QVector<QPointF> &rightPath)
{
    constexpr int steps=30;
    const double v=(leftSpeed+rightSpeed)/2.0;
    const double omega=(rightSpeed-leftSpeed)/kWheelBase; // + = turning left
    const double dt=seconds/steps;
    // Integrate in (forward, left) coordinates, output as (forward, right).
    double forward=0.0, left=0.0, heading=0.0;
    leftPath.clear();
    rightPath.clear();
    for (int i=0; i<=steps; ++i) {
        const double offsetForward=-std::sin(heading)*kWheelBase/2.0;
        const double offsetLeft=std::cos(heading)*kWheelBase/2.0;
        leftPath.append(QPointF(forward+offsetForward, -(left+offsetLeft)));
        rightPath.append(QPointF(forward-offsetForward, -(left-offsetLeft)));
        const double midHeading=heading+omega*dt/2.0;
        forward+=v*std::cos(midHeading)*dt;
        left+=v*std::sin(midHeading)*dt;
        heading+=omega*dt;
    }
}

void robot::setCommandedWheels(double forward, double rotation)
{
    commandedLeft_=forward-rotation*kWheelBase/2.0;
    commandedRight_=forward+rotation*kWheelBase/2.0;
}

robot::robot(QObject *parent) : QObject(parent)
{
    startLoging=false;
    x=y=fi=0;
    datacounter=0;
    forwardspeed=rotationspeed=0;
    useDirectCommands=1;
    copyOfLaserData={};
#ifndef DISABLE_OPENCV
    actIndex=-1;
#endif
#ifndef DISABLE_SKELETON
    gestureControlEnabled_=false;
    leftWheelSpeed_=0;
    rightWheelSpeed_=0;
    gestureRampTime_=std::chrono::steady_clock::now();
#endif
    qRegisterMetaType<LaserMeasurement>("LaserMeasurement");
    #ifndef DISABLE_OPENCV
    qRegisterMetaType<cv::Mat>("cv::Mat");
#endif
#ifndef DISABLE_SKELETON
qRegisterMetaType<skeleton>("skeleton");
#endif
}

robot::~robot()
{
    stopRobot(); // Join callbacks while all robot members are still alive.
}

void robot::initAndStartRobot(std::string ipaddress)
{

    forwardspeed=0;
    rotationspeed=0;
    ///setovanie veci na komunikaciu s robotom/lidarom/kamerou.. su tam adresa porty a callback.. laser ma ze sa da dat callback aj ako lambda.
    /// lambdy su super, setria miesto a ak su rozumnej dlzky,tak aj prehladnost... ak ste o nich nic nepoculi poradte sa s vasim doktorom alebo lekarnikom...
    robotCom.setLaserParameters(ipaddress,52999,5299,/*[](LaserMeasurement dat)->int{std::cout<<"som z lambdy callback"<<std::endl;return 0;}*/std::bind(&robot::processThisLidar,this,std::placeholders::_1));
    robotCom.setRobotParameters(ipaddress,53000,5300,std::bind(&robot::processThisRobot,this,std::placeholders::_1));
    #ifndef DISABLE_AMCL
    robotCom.setAMCLParameters("mapa.txt",1500,0.01,2,std::bind(&robot::processThisAMCLPosition,this,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));
#endif
  #ifndef DISABLE_OPENCV
    robotCom.setCameraParameters("http://"+ipaddress+":8000/stream.mjpg",std::bind(&robot::processThisCamera,this,std::placeholders::_1));
#endif
   #ifndef DISABLE_SKELETON
      robotCom.setSkeletonParameters("127.0.0.1",23432,23432,std::bind(&robot::processThisSkeleton,this,std::placeholders::_1));
#endif
    ///ked je vsetko nasetovane tak to tento prikaz spusti (ak nieco nieje setnute,tak to normalne nenastavi.cize ak napr nechcete kameru,vklude vsetky info o nej vymazte)
    robotCom.robotStart();


}

void robot::setSpeedVal(double forw, double rots)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    forwardspeed=forw;
    rotationspeed=rots;
    useDirectCommands=0;
}

void robot::setSpeed(double forw, double rots)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(forw==0 && rots!=0)
        robotCom.setRotationSpeed(rots);
    else if(forw!=0 && rots==0)
        robotCom.setTranslationSpeed(forw);
    else if((forw!=0 && rots!=0))
        robotCom.setArcSpeed(forw,forw/rots);
    else
        robotCom.setTranslationSpeed(0);
    setCommandedWheels(forw, rots);
    useDirectCommands=1;
}

void robot::setGestureControlEnabled(bool enabled)
{
#ifndef DISABLE_SKELETON
    std::lock_guard<std::mutex> lock(commandMutex_);
    gestureControlEnabled_=enabled;
    if (!enabled) {
        leftWheelSpeed_=0;
        rightWheelSpeed_=0;
        robotCom.setTranslationSpeed(0);
        setCommandedWheels(0, 0);
        useDirectCommands=1;
    }
#else
    Q_UNUSED(enabled);
#endif
}
void robot::lidarFusion(bool enabled)
{
    std::lock_guard<std::mutex> lock(lidarMutex_);
    lidarFusionEnabled_=enabled;
}

#ifndef DISABLE_SKELETON
void robot::updateGestureSpeeds()
{
    skeleton gestureData;
    {
        std::lock_guard<std::mutex> lock(skeletonMutex_);
        gestureData=skeleJoints;
    }

    const klb &leftMiddle=gestureData.joints[left_middle_tip];
    const klb &rightMiddle=gestureData.joints[right_middle_tip];
    const double middleTipDistance=std::sqrt(
        std::pow(leftMiddle.x-rightMiddle.x, 2.0)
        + std::pow(leftMiddle.y-rightMiddle.y, 2.0)
        + std::pow(leftMiddle.z-rightMiddle.z, 2.0));

    double targetLeft=0.0;
    double targetRight=0.0;
    if (middleTipDistance<=kBackwardModeDistance) {
        // Reverse speed is linear in the index knuckle-to-tip distance,
        // normalised by palm length so it does not depend on hand distance.
        const auto planarDistance = [](const klb &a, const klb &b) {
            return std::hypot(a.x-b.x, a.y-b.y);
        };
        const auto reverseSpeed = [&](int wrist, int middleMcp,
                                      int indexMcp, int indexTip) {
            const double palm=planarDistance(gestureData.joints[wrist],
                                             gestureData.joints[middleMcp]);
            if (palm<1e-6)
                return 0.0; // hand not tracked
            const double extension=planarDistance(gestureData.joints[indexMcp],
                                                  gestureData.joints[indexTip])/palm;
            const double ratio=(extension-kFingerCurledRatio)
                /(kFingerExtendedRatio-kFingerCurledRatio);
            return -kMaximumGestureSpeed*std::max(0.0, std::min(ratio, 1.0));
        };
        targetLeft=reverseSpeed(left_wrist, left_middle_mcp,
                                left_index_mcp, left_index_tip);
        targetRight=reverseSpeed(right_wrist, right_middle_mcp,
                                 right_index_mcp, right_index_tip);
    } else {
        // Forward speed follows the on-screen angle of the middle finger
        // (knuckle -> tip) in 5 steps of 18 deg each: 0-18 deg (tip level
        // with the knuckle) or pointing down = 0, 72-90 deg = full speed.
        const auto forwardSpeed = [&](int middleMcp, int middleTip) {
            const klb &mcp=gestureData.joints[middleMcp];
            const klb &tip=gestureData.joints[middleTip];
            const double up=mcp.y-tip.y; // screen y grows downwards
            const double sideways=std::abs(tip.x-mcp.x);
            if (up<=0.0)
                return 0.0;
            const double angleDegrees=std::atan2(up, sideways)*180.0/3.14159265358979323846;
            const int segment=std::min(static_cast<int>(angleDegrees/18.0), 4);
            return kMaximumGestureSpeed*segment/4.0;
        };
        targetLeft=forwardSpeed(left_middle_mcp, left_middle_tip);
        targetRight=forwardSpeed(right_middle_mcp, right_middle_tip);
    }

    const auto now=std::chrono::steady_clock::now();
    const double elapsed=std::chrono::duration<double>(now-gestureRampTime_).count();
    const double maxChange=kMaximumGestureSpeed*std::min(elapsed/0.5, 1.0);
    const auto rampTowardsTarget = [maxChange](double current, double target) {
        const double difference=target-current;
        if (std::abs(difference)<=maxChange)
            return target;
        return current+(difference>0 ? maxChange : -maxChange);
    };
    leftWheelSpeed_=rampTowardsTarget(leftWheelSpeed_, targetLeft);
    rightWheelSpeed_=rampTowardsTarget(rightWheelSpeed_, targetRight);
    gestureRampTime_=now;
    commandedLeft_=leftWheelSpeed_;
    commandedRight_=rightWheelSpeed_;

    const double forward=(leftWheelSpeed_+rightWheelSpeed_)/2.0;
    const double rotation=(rightWheelSpeed_-leftWheelSpeed_)/230.0;
    if (std::abs(rotation)<1e-9) {
        robotCom.setTranslationSpeed(static_cast<int>(std::lround(forward)));
    } else if (std::abs(forward)<1e-9) {
        robotCom.setRotationSpeed(rotation);
    } else {
        robotCom.setArcSpeed(static_cast<int>(std::lround(forward)),
                             static_cast<int>(std::lround(forward/rotation)));
    }
}
#endif

///toto je calback na data z robota, ktory ste podhodili robotu vo funkcii initAndStartRobot
/// vola sa vzdy ked dojdu nove data z robota. nemusite nic riesit, proste sa to stane
int robot::processThisRobot(TKobukiData robotdata)
{


    ///tu mozete robit s datami z robota


    if(startLoging==true)
    {
        std::ofstream robotlog;
        robotlog.open("robot.log",std::ios::app);
        robotlog<<robotdata.synctimestamp<<" "<<robotdata.EncoderLeft<<" "<<robotdata.EncoderRight<<" "<<(double)(robotdata.GyroAngle/100.0)<<std::endl;
        robotlog.close();
    }


///TU PISTE KOD... TOTO JE TO MIESTO KED NEVIETE KDE ZACAT,TAK JE TO NAOZAJ TU. AK AJ TAK NEVIETE, SPYTAJTE SA CVICIACEHO MA TU NATO STRING KTORY DA DO HLADANIA XXX

    ///kazdy piaty krat, aby to ui moc nepreblikavalo..
    if(datacounter%5==0)
    {

        ///ak nastavite hodnoty priamo do prvkov okna,ako je to na tychto zakomentovanych riadkoch tak sa moze stat ze vam program padne
        // ui->lineEdit_2->setText(QString::number(robotdata.EncoderRight));
        //ui->lineEdit_3->setText(QString::number(robotdata.EncoderLeft));
        //ui->lineEdit_4->setText(QString::number(robotdata.GyroAngle));
        /// lepsi pristup je nastavit len nejaku premennu, a poslat signal oknu na prekreslenie
        /// okno pocuva vo svojom slote a vasu premennu nastavi tak ako chcete. prikaz emit to presne takto spravi
        /// viac o signal slotoch tu: https://doc.qt.io/qt-5/signalsandslots.html
        ///posielame sem nezmysli.. pohrajte sa nech sem idu zmysluplne veci
        emit publishPosition(x,y,fi);
        ///toto neodporucam na nejake komplikovane struktury.signal slot robi kopiu dat. radsej vtedy posielajte
        /// prazdny signal a slot bude vykreslovat strukturu (vtedy ju musite mat samozrejme ako member premmennu v mainwindow.ak u niekoho najdem globalnu premennu,tak bude cistit bludisko zubnou kefkou.. kefku dodam)
        /// vtedy ale odporucam pouzit mutex, aby sa vam nestalo ze budete pocas vypisovania prepisovat niekde inde

    }
    ///---tu sa posielaju rychlosti do robota... vklude zakomentujte ak si chcete spravit svoje
    std::lock_guard<std::mutex> lock(commandMutex_);
#ifndef DISABLE_SKELETON
    if (gestureControlEnabled_)
        updateGestureSpeeds();
    else
#endif
    if(useDirectCommands==0)
    {
        if(forwardspeed==0 && rotationspeed!=0)
            robotCom.setRotationSpeed(rotationspeed);
        else if(forwardspeed!=0 && rotationspeed==0)
            robotCom.setTranslationSpeed(forwardspeed);
        else if((forwardspeed!=0 && rotationspeed!=0))
            robotCom.setArcSpeed(forwardspeed,forwardspeed/rotationspeed);
        else
            robotCom.setTranslationSpeed(0);
        setCommandedWheels(forwardspeed, rotationspeed);
    }
    if (datacounter%5==0)
        emit publishWheelSpeeds(commandedLeft_, commandedRight_);
    emit telemetryReceived();
    datacounter++;

    return 0;

}

int robot::processThisAMCLPosition(float x, float y, float theta)
{
    std::cout<<"moja amcl poloha je "<<x<<" "<<y<<" "<<theta<<std::endl;
    emit publishAMCLPosition(x,y,fi);
    return 0;
}

///toto je calback na data z lidaru, ktory ste podhodili robotu vo funkcii initAndStartRobot
/// vola sa ked dojdu nove data z lidaru
int robot::processThisLidar(LaserMeasurement laserData)
{

    if(startLoging==true)
    {
        std::ofstream robotlog;
        robotlog.open("laser.log",std::ios::app);
        for(int i=0;i<laserData.numberOfScans;i++)
        {
            robotlog<<laserData.Data[i].timestamp<<" "<<laserData.Data[i].scanDistance<<" "<<laserData.Data[i].scanAngle<<" "<<laserData.Data[i].scanQuality<<std::endl;
        }
        robotlog.close();
    }
    {
        std::lock_guard<std::mutex> lock(lidarMutex_);
        copyOfLaserData=laserData;
    }
    //tu mozete robit s datami z lidaru.. napriklad najst prekazky, zapisat do mapy. naplanovat ako sa prekazke vyhnut.
    // ale nic vypoctovo narocne - to iste vlakno ktore cita data z lidaru
   // updateLaserPicture=1;
    emit publishLidar(laserData);
   // update();//tento prikaz prinuti prekreslit obrazovku.. zavola sa paintEvent funkcia


    return 0;

}

  #ifndef DISABLE_OPENCV
///toto je calback na data z kamery, ktory ste podhodili robotu vo funkcii initAndStartRobot
/// vola sa ked dojdu nove data z kamery
int robot::processThisCamera(cv::Mat cameraData)
{

    if (cameraData.empty())
        return 0;

    LaserMeasurement laserData;
    bool fusionEnabled;
    {
        std::lock_guard<std::mutex> lock(lidarMutex_);
        laserData=copyOfLaserData;
        fusionEnabled=lidarFusionEnabled_;
    }

    double leftSpeed, rightSpeed;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        leftSpeed=commandedLeft_;
        rightSpeed=commandedRight_;
    }

    cv::Mat output=cameraData.clone();
    if (fusionEnabled && laserData.numberOfScans>0) output=projectLidarToCamera(output, laserData);
    if (fusionEnabled) drawPredictedPath(output, leftSpeed, rightSpeed);
    emit publishCamera(output);
    return 0;
}

cv::Mat robot::projectLidarToCamera(const cv::Mat &cameraData,
                                    const LaserMeasurement &laserData) const
{
    constexpr double degreesToRadians=3.14159265358979323846/180.0;

    cv::Mat projected=cameraData.clone();
    for (int i=0; i<laserData.numberOfScans && i<1000; ++i) {
        const LaserData &scan=laserData.Data[i];
        if (!std::isfinite(scan.scanDistance)
            || !std::isfinite(scan.scanAngle)
            || scan.scanDistance<=0.0f)
            continue;

        // RPLidar: angle in degrees, clockwise from forward; distance in mm.
        const double angle=scan.scanAngle*degreesToRadians;
        const double x=scan.scanDistance*std::cos(angle);
        const double y=scan.scanDistance*std::sin(angle); // +y = right
        if (x<=0.0)
            continue; // behind the robot, never in the camera view
        if (scan.scanDistance>2000.0f)
            continue; // only show obstacles within 2 m
        cv::Point pixel;
        if (!projectToCamera(projected, x, y, pixel)
            || pixel.x<0 || pixel.x>=projected.cols
            || pixel.y<0 || pixel.y>=projected.rows)
            continue;

        // Pale blue at 2 m, deepening to strong blue as the obstacle gets closer.
        const double closeness=1.0-scan.scanDistance/2000.0;
        const cv::Scalar color(255,                    // B
                               220-180*closeness,      // G
                               200-200*closeness);     // R
        cv::circle(projected, pixel, 4, color, -1);
    }
    return projected;
}

// Wheel tracks for the next second, projected in the LiDAR plane like the scan.
void robot::drawPredictedPath(cv::Mat &image, double leftSpeed, double rightSpeed) const
{
    if (std::abs(leftSpeed)<1.0 && std::abs(rightSpeed)<1.0)
        return;
    QVector<QPointF> leftPath, rightPath;
    predictWheelPaths(leftSpeed, rightSpeed, kPredictionSeconds, leftPath, rightPath);
    const cv::Scalar color(255, 0, 0); // BGR blue
    for (const QVector<QPointF> *path : {&leftPath, &rightPath}) {
        for (int i=1; i<path->size(); ++i) {
            cv::Point from, to;
            if (projectToCamera(image, (*path)[i-1].x(), (*path)[i-1].y(), from)
                && projectToCamera(image, (*path)[i].x(), (*path)[i].y(), to))
                cv::line(image, from, to, color, 3, cv::LINE_AA); // clipped by OpenCV
        }
    }
}
#endif

  #ifndef DISABLE_SKELETON
/// vola sa ked dojdu nove data z trackera
int robot::processThisSkeleton(skeleton skeledata)
{

    skeleton publishedSkeleton;
    {
        std::lock_guard<std::mutex> lock(skeletonMutex_);
        skeleJoints=skeledata;
        publishedSkeleton=skeleJoints;
    }

    bool gesturesEnabled;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        gesturesEnabled=gestureControlEnabled_;
    }
    if (gesturesEnabled && isViewSquareGesture(skeledata)) {
        const auto now=std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now-lastViewToggle_).count()>=kViewToggleCooldownSeconds) {
            lastViewToggle_=now;
            emit viewToggleGesture();
        }
    }

    emit publishSkeleton(publishedSkeleton);
    return 0;
}

// Corners in order around the square: left thumb, right thumb, right pinky, left pinky.
bool robot::isViewSquareGesture(const skeleton &data)
{
    const klb *corners[4]={&data.joints[left_thumb_tip], &data.joints[right_thumb_tip],
                           &data.joints[right_pink_tip], &data.joints[left_pink_tip]};
    const auto distance = [](const klb *a, const klb *b) {
        return std::hypot(a->x-b->x, a->y-b->y);
    };
    double sides[4];
    double meanSide=0.0;
    for (int i=0; i<4; ++i) {
        sides[i]=distance(corners[i], corners[(i+1)%4]);
        meanSide+=sides[i]/4.0;
    }
    if (meanSide<kSquareMinimumSide)
        return false;
    for (double side : sides)
        if (std::abs(side-meanSide)>kSquareSideTolerance*meanSide)
            return false;
    // Equal diagonals rule out a flattened rhombus.
    const double diagonalA=distance(corners[0], corners[2]);
    const double diagonalB=distance(corners[1], corners[3]);
    return std::abs(diagonalA-diagonalB)<=kSquareSideTolerance*std::max(diagonalA, diagonalB);
}
#endif
