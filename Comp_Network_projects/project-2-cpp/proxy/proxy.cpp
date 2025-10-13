#include "proxy.h"

void forwardData(int fromSock, int toSock) {
    char buffer[BUFFER_SIZE];
    ssize_t n;
    while ((n = recv(fromSock, buffer, sizeof(buffer), 0)) > 0) {
        if (send(toSock, buffer, n, 0) <= 0) break;
    }
    shutdown(toSock, SHUT_WR);
    shutdown(fromSock, SHUT_RD);
}
/*Simple Berkely Socket used*/
int main() {
    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in proxyAddr{};
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = INADDR_ANY;
    proxyAddr.sin_port = htons(PROXY_PORT);

    if (bind(listenSock, (struct sockaddr*)&proxyAddr, sizeof(proxyAddr)) < 0) {
        perror("bind");
        return 1;
    }

    listen(listenSock, 5);
    std::cout << "Proxy listening on port " << PROXY_PORT << std::endl;

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &len);
        if (clientSock < 0) {
            perror("accept");
            continue;
        }

        std::thread([clientSock]() {
            // 1. Read first line (server address + port)
            std::string firstLine;
            char ch;
            while (recv(clientSock, &ch, 1, 0) > 0) {
                if (ch == '\n') break;
                firstLine.push_back(ch);
            }

            std::istringstream iss(firstLine);
            std::string serverHost;
            int serverPort;
            iss >> serverHost >> serverPort;

            std::cout << "Proxy connecting to " << serverHost << ":" << serverPort << std::endl;

            // 2. Connect to the destination server
            int serverSock = socket(AF_INET, SOCK_STREAM, 0);
            if (serverSock < 0) {
                perror("socket");
                close(clientSock);
                return;
            }

            hostent* hp = gethostbyname(serverHost.c_str());
            if (!hp) {
                std::cerr << "Unknown host: " << serverHost << std::endl;
                close(clientSock);
                close(serverSock);
                return;
            }

            sockaddr_in serverAddr{};
            serverAddr.sin_family = AF_INET;
            bcopy(hp->h_addr, &serverAddr.sin_addr, hp->h_length);
            serverAddr.sin_port = htons(serverPort);

            if (connect(serverSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
                perror("connect");
                close(clientSock);
                close(serverSock);
                return;
            }

            std::cout << "Proxy connected to destination." << std::endl;

            // 3. Bidirectional forwarding
            std::thread t1(forwardData, clientSock, serverSock);
            std::thread t2(forwardData, serverSock, clientSock);

            t1.join();
            t2.join();

            close(clientSock);
            close(serverSock);
            std::cout << "Connection closed.\n";
        }).detach();
    }

    close(listenSock);
    return 0;
}
