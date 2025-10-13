/* Simple header file to include all libraries used and PORT defines for proxy.cpp*/


#ifndef PROXY_H
#define PROXY_H

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <sstream>

#define PROXY_PORT 5465
#define BUFFER_SIZE 65536

#endif