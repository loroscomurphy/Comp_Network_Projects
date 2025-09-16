#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <iostream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <functional>
#include <filesystem> 
#include "../common/CommandHandler.h"

#define MAX_LINE 256
#define SERVER_PORT 5432

class Client {
    private:
        FILE *fp;
        struct hostent *hp;
        struct sockaddr_in sin;
        char *host;
        char buf[MAX_LINE];
        int s;
        int len;
        CommandHandler commandHandler;

    public:
        Client() = default;
        // create a client with the host name from when you run the program
        // like ./client localhost
        Client(int argc, char* argv[]);
        ~Client() = default;
        void registerCommands();
        void builtin_put(int argc, char* argv[]);
        void builtin_get(int argc, char* argv[]);
        void connectToServer();
        void mainloop();

};

#endif // CLIENT_H
