#include "server.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <errno.h>
#include <cstdlib>

Server::Server() {
    bzero((char *)&this->sin, sizeof(this->sin));
    this->sin.sin_family = AF_INET;
    this->sin.sin_addr.s_addr = INADDR_ANY;
    this->sin.sin_port = htons(SERVER_PORT);
    this->addrLen = sizeof(this->sin);
    this->listenSocket = -1;
    this->clientSocket = -1;
    this->bytesReceived = 0;

}

Server::~Server() {
    if (this->clientSocket >= 0) {
        close(this->clientSocket);
    }
    if (this->listenSocket >= 0) {
        close(this->listenSocket);
    }
}

// Command registration: binds protocol verbs to member implementations
void Server::registerCommands() {
    this->commandHandler.registerCommand("put", [this](int argc, char* argv[]) {
        this->builtin_put(argc, argv);
        return 0;
    });
    this->commandHandler.registerCommand("get", [this](int argc, char* argv[]) {
        this->builtin_get(argc, argv);
        return 0;
    });
}

void Server::builtin_put(int argc, char* argv[]) {
    std::cout << "builtin_put" << std::endl;
    // Header parsing group: extract pathLen and fileSize
    if (argc < 3) { std::string err = "ERR 400 bad_header\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    char* end1 = nullptr; char* end2 = nullptr;
    unsigned long pathLenUl = std::strtoul(argv[1], &end1, 10);
    unsigned long fileSizeUl = std::strtoul(argv[2], &end2, 10);
    if (!argv[1] || !argv[2] || *end1 != '\0' || *end2 != '\0' || pathLenUl == 0UL) {
        std::string err = "ERR 400 bad_header\n"; sendAll(this->clientSocket, err.data(), err.size()); return;
    }
    size_t pathLen = static_cast<size_t>(pathLenUl);
    size_t fileSize = static_cast<size_t>(fileSizeUl);

    // Path group: read and sanitize the path bytes
    std::string path(pathLen, '\0');
    if (!recvExact(this->clientSocket, path.data(), pathLen)) { std::string err = "ERR 400 bad_path\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    std::string safePath;
    if (!sanitizePath(path, safePath)) { std::string err = "ERR 403 bad_path\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }

    // File transfer group: stream fileSize bytes to disk
    if (!writeFileFromSocket(this->clientSocket, safePath, fileSize)) { std::string err = "ERR 500 write_failed\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    std::string ok = "OK\n";
    sendAll(this->clientSocket, ok.data(), ok.size());
}

void Server::builtin_get(int argc, char* argv[]) {
    std::cout << "builtin_get" << std::endl;
    // Header parsing group: extract pathLen
    if (argc < 2) { std::string err = "ERR 400 bad_header\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    char* end = nullptr;
    unsigned long pathLenUl = std::strtoul(argv[1], &end, 10);
    if (!argv[1] || *end != '\0' || pathLenUl == 0UL) { std::string err = "ERR 400 bad_header\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    size_t pathLen = static_cast<size_t>(pathLenUl);

    // Path group: read and sanitize the path bytes
    std::string path(pathLen, '\0');
    if (!recvExact(this->clientSocket, path.data(), pathLen)) { std::string err = "ERR 400 bad_path\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    std::string safePath;
    if (!sanitizePath(path, safePath)) { std::string err = "ERR 403 bad_path\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }

    // File lookup and transfer group
    size_t fileSize = 0;
    if (!computeFileSize(safePath, fileSize)) { std::string err = "ERR 404 not_found\n"; sendAll(this->clientSocket, err.data(), err.size()); return; }
    std::string ok = std::string("OK ") + std::to_string(fileSize) + "\n";
    if (!sendAll(this->clientSocket, ok.data(), ok.size())) return;
    if (!sendFileToSocket(this->clientSocket, safePath)) return;
}


// Socket setup: create, configure, bind, and listen
void Server::setup() {
    int opt = 1;
    if ((this->listenSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) 
    {
        perror("simplex-talk: socket");
        exit(1);
    }
    
    if (setsockopt(this->listenSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }
    #ifdef SO_REUSEPORT
    if (setsockopt(this->listenSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
    #endif
    if ((this->listenSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("simplex-talk: socket");
        exit(1);
    }
    if ((bind(this->listenSocket, (struct sockaddr *)&this->sin, sizeof(this->sin))) < 0) {
        perror("simplex-talk: bind");
        exit(1);
    }
    listen(this->listenSocket, MAX_PENDING);
}

// Main loop: accept a client and process header lines via CommandHandler
void Server::run() {

    this->registerCommands();

    while (1) {
        if ((this->clientSocket = accept(this->listenSocket, (struct sockaddr *)&this->sin, &this->addrLen)) < 0) {
            perror("simplex-talk: accept");
            exit(1);
        }

        while (true) {
            std::string header;
            if (!this->recvLine(this->clientSocket, header)) break;


            std::cout << "header: " << header << std::endl;
            std::istringstream iss(header);
            std::string cmd;
            iss >> cmd;
            std::cout << "cmd: " << cmd << std::endl;
            // copy cmd
            char* header_copy = strdup(header.c_str());
            std::cout << "header_copy: " << header_copy << std::endl;
            this->commandHandler.executeCommand(header_copy);
            free(header_copy);

        }

        close(this->clientSocket);
        this->clientSocket = -1;
    }
}

bool Server::sendAll(int sock, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        total += static_cast<size_t>(n);
    }
    return true;
}

bool Server::recvExact(int sock, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, p + total, len - total, 0);
        if (n == 0) return false;
        if (n < 0) { if (errno == EINTR) continue; return false; }
        total += static_cast<size_t>(n);
    }
    return true;
}

bool Server::recvLine(int sock, std::string &out) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n == 0) return false;
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (ch == '\n') break;
        out.push_back(ch);
        if (out.size() > 4096) return false;
    }
    return true;
}

bool Server::sanitizePath(const std::string& requested, std::string& safeOut) {
    if (!requested.empty() && requested[0] == '/') return false;
    if (requested.find("..") != std::string::npos) return false;
    safeOut = std::string("server_storage/") + requested;
    return true;
}

bool Server::writeFileFromSocket(int sock, const std::string& destPath, size_t size) {
    std::string tmpPath = destPath + ".part";
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) return false;
    std::vector<char> buffer(64 * 1024);
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, buffer.size());
        if (!this->recvExact(sock, buffer.data(), chunk)) return false;
        out.write(buffer.data(), static_cast<std::streamsize>(chunk));
        if (!out) return false;
        remaining -= chunk;
    }
    out.close();
    if (std::rename(tmpPath.c_str(), destPath.c_str()) != 0) return false;
    return true;
}

bool Server::computeFileSize(const std::string& path, size_t &outSize) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streampos endPos = in.tellg();
    if (endPos < 0) return false;
    outSize = static_cast<size_t>(endPos);
    return true;
}

bool Server::sendFileToSocket(int sock, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        if (!this->sendAll(sock, buffer.data(), static_cast<size_t>(got))) return false;
    }
    return true;
}

int main() {
    Server server;
    server.setup();
    server.run();
    return 0;
}


