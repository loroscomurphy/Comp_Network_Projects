#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include "../common/CommandHandler.h"

#define SERVER_PORT 5432
#define MAX_PENDING 5
#define MAX_LINE 256

/*
Server
------

Commands:
    - put <pathLen> <fileSize>\n [<path bytes>][<file bytes>]
        Uploads a file into `server_storage/`.
        Steps inside `builtin_put`:
          1) Parse header tokens: pathLen, fileSize.
          2) Read exactly `pathLen` bytes for the relative path.
          3) Sanitize the path (no absolute/.. traversal) and prefix `server_storage/`.
          4) Stream exactly `fileSize` bytes from the socket to a temporary file,
             then atomically rename to the final path.
          5) Send `OK\n` on success.

    - get <pathLen>\n [<path bytes>]
        Sends the file size followed by the file contents.
        Steps inside `builtin_get`:
          1) Parse header token: pathLen.
          2) Read exactly `pathLen` bytes for the relative path.
          3) Sanitize and resolve to `server_storage/<path>`.
          4) Compute file size and send `OK <size>\n`.
          5) Stream file bytes to the client.

I/O Helpers:
    - `sendAll`, `recvExact`, `recvLine` enforce reliable framed I/O semantics.
    - `sanitizePath` validates and builds a safe server-local destination path.
    - `writeFileFromSocket`, `sendFileToSocket`, `computeFileSize` encapsulate
      file system operations with robust, incremental I/O.
*/
class Server {
    private:
        struct sockaddr_in sin;
        char buf[MAX_LINE];
        socklen_t addrLen;
        int listenSocket;
        int clientSocket;
        int bytesReceived;
        CommandHandler commandHandler;
        bool sendAll(int sock, const void* buf, size_t len);
        bool recvExact(int sock, void* buf, size_t len);
        bool recvLine(int sock, std::string &out);
        bool sanitizePath(const std::string& requested, std::string& safeOut);
        bool writeFileFromSocket(int sock, const std::string& destPath, size_t size);
        bool computeFileSize(const std::string& path, size_t &outSize);
        bool sendFileToSocket(int sock, const std::string& path);
    public:
        Server();
        ~Server();
        void registerCommands();
        void builtin_put(int argc, char* argv[]);
        void builtin_get(int argc, char* argv[]);
        void setup();
        void run();
};

#endif // SERVER_H


