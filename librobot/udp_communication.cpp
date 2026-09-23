#include "udp_communication.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
//#include "pthread.h"
//#include "unistd.h"
#include "fcntl.h"
#include "string.h"
#include <math.h>
#include <stdint.h>
#include <iostream>
#include <fstream>
#include <cmath>
#include <iomanip>
//#include <chrono>
#include <sstream>
udp_communication::udp_communication()
{

}

void udp_communication::init_connection(std::string addres, int inport, int outport)
{
#ifdef _WIN32
    WSADATA wsaData = {0};
    int iResult = 0;
    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
#else
#endif
    las_slen = sizeof(las_si_other);
    if ((las_s=::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {

    }

    char las_broadcastene=1;
#ifdef _WIN32
    DWORD timeout=100;

    ::setsockopt(las_s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof timeout);
    ::setsockopt(las_s,SOL_SOCKET,SO_BROADCAST,&las_broadcastene,sizeof(las_broadcastene));
#else
    ::setsockopt(las_s,SOL_SOCKET,SO_BROADCAST,&las_broadcastene,sizeof(las_broadcastene));
#endif
    // zero out the structure
    memset((char *) &las_si_me, 0, sizeof(las_si_me));

    las_si_me.sin_family = AF_INET;
    las_si_me.sin_port = htons(outport);
    las_si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    las_si_posli.sin_family = AF_INET;
    las_si_posli.sin_port = htons(inport);
    las_si_posli.sin_addr.s_addr = inet_addr(addres.data());;//htonl(INADDR_BROADCAST);
    ::bind(las_s , (struct sockaddr*)&las_si_me, sizeof(las_si_me) );
}

int udp_communication::sendMessage(const std::vector<unsigned char> &mess)
{
    if (::sendto(las_s, (char*)mess.data(), sizeof(char)*mess.size(), 0, (struct sockaddr*) &las_si_posli, las_slen) == -1)
    {

        return -1;
    }
    return 0;
}

int udp_communication::getMessage(char *message, int maxSize)
{
    memset(message,0,maxSize*sizeof(char));
    if ((las_recv_len = ::recvfrom(las_s, message, sizeof(char)*maxSize, 0, (struct sockaddr *) &las_si_other, &las_slen)) == -1)
    {

        return -1;
    }
    return las_recv_len;
}
