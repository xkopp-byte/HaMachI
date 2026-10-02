#ifndef UDP_COMMUNICATION_H
#define UDP_COMMUNICATION_H

#ifdef _WIN32
#define NOMINMAX
#include<windows.h>

#else
#include <termios.h>
#include <unistd.h>
#include "unistd.h"
#include<arpa/inet.h>
#include<unistd.h>
#include<sys/socket.h>
#endif
#include <string>
#include <vector>
class udp_communication
{
public:
    udp_communication();
    ~udp_communication(){ closeConnection(); }
    void closeConnection();

    void init_connection(std::string addres,int inport,int outport);
    int sendMessage(const std::vector<unsigned char> &mess);
    int getMessage(char *message, int maxSize);
private:
    struct sockaddr_in las_si_me, las_si_other,las_si_posli;

#ifdef _WIN32
    SOCKET las_s = INVALID_SOCKET;
    bool winsockStarted_ = false;
#else
    int las_s = -1;
#endif
    int las_recv_len;
    int ip_portOut;
    int ip_portIn;
#ifdef _WIN32

        int las_slen;

#else
         unsigned int las_slen;
#endif
};

#endif // UDP_COMMUNICATION_H
