#ifndef PROXY_HTTP_H
#define PROXY_HTTP_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

#define DEFAULT_PORT "5465"
#define BACKLOG 128
#define BUF_SIZE 65536
#define LOGFILE "proxy_http.log"

#endif // PROXY_HTTP_H