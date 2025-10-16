/*
 * Simple HTTP/HTTPS Proxy Server
 * Supports HTTP methods and CONNECT for HTTPS tunneling.
 * Logs requests with timestamps.
 * 
 * Compile with: g++ -std=c++17 -pthread -o proxy_http proxy_http.cpp
 * Usage: ./proxy_http [port]
 * Default port is 8080 if not specified.
 */

#include "proxy_http.h"

// Forward declarations
void *client_thread(void *arg);

// Utility: print timestamped log
void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    std::ostringstream oss;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    oss << "[" << ts << "] ";

    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    oss << buf << "\n";

    std::string s = oss.str();
    // stdout
    fputs(s.c_str(), stdout);
    fflush(stdout);
    // append log file
    FILE *f = fopen(LOGFILE, "a");
    if (f) {
        fputs(s.c_str(), f);
        fclose(f);
    }
}

// sendAll: loop until all bytes are sent or error
ssize_t sendAll(int sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

// recvLine: read until '\n' (returns line without '\n', or empty + false on EOF/error)
bool recvLine(int sock, std::string &out, int maxlen = 8*1024) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\r') {
            // peek next; if '\n' consume it, otherwise continue
            ssize_t m = recv(sock, &ch, 1, MSG_PEEK);
            if (m > 0 && ch == '\n') {
                // consume '\n'
                recv(sock, &ch, 1, 0);
            }
            break;
        }
        if (ch == '\n') break;
        out.push_back(ch);
        if ((int)out.size() >= maxlen) return false;
    }
    return true;
}

// readHeaders: read header lines until blank line; returns full headers as one string
bool readHeaders(int sock, std::string &headersOut) {
    headersOut.clear();
    std::string line;
    while (true) {
        if (!recvLine(sock, line)) return false;
        if (line.empty()) break; // end of headers
        headersOut += line;
        headersOut += "\r\n";
        // safety: extremely large headers should abort
        if (headersOut.size() > 64*1024) return false;
    }
    headersOut += "\r\n";
    return true;
}

// parse host:port from an URL (absolute URI) or Host header fallback
// Input:
//   requestLine: e.g., "GET http://example.com/path HTTP/1.1"  OR "GET /path HTTP/1.1"
//   headers: full headers including Host header
// Outputs host (string), port (string), and pathToSend (string) which is the origin-form path to send to server.
bool determineHostPortAndPath(const std::string &requestLine, const std::string &headers,
                              std::string &hostOut, std::string &portOut, std::string &pathOut) {
    std::istringstream iss(requestLine);
    std::string method, uri, version;
    if (!(iss >> method >> uri >> version)) return false;
    // If uri begins with "http://" or "https://", parse out host and path
    if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0) {
        // strip scheme
        size_t pos = uri.find("://");
        size_t start = (pos==std::string::npos)?0:pos+3;
        size_t slash = uri.find('/', start);
        std::string authority = (slash == std::string::npos) ? uri.substr(start) : uri.substr(start, slash - start);
        pathOut = (slash == std::string::npos) ? "/" : uri.substr(slash);
        // authority may contain port
        size_t col = authority.find(':');
        if (col != std::string::npos) {
            hostOut = authority.substr(0, col);
            portOut = authority.substr(col + 1);
        } else {
            hostOut = authority;
            // default port
            if (uri.rfind("https://", 0) == 0) portOut = "443"; else portOut = "80";
        }
        return true;
    } else {
        // origin-form: use Host header
        // extract Host header from headers string
        std::istringstream hs(headers);
        std::string line;
        std::string hostHeader;
        while (std::getline(hs, line)) {
            if (line.size() > 5 && (line[0]=='H' || line[0]=='h')) {
                // transform to lower for safe compare
                std::string lower=line;
                for(char &c: lower) c=tolower(c);
                if (lower.rfind("host:", 0) == 0) {
                    hostHeader = line.substr(5);
                    break;
                }
            }
        }
        // trim whitespace
        auto trim = [](std::string &s){
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a==std::string::npos || b==std::string::npos) { s.clear(); return; }
            s = s.substr(a, b-a+1);
        };
        trim(hostHeader);
        if (hostHeader.empty()) return false;
        size_t colon = hostHeader.find(':');
        if (colon != std::string::npos) {
            hostOut = hostHeader.substr(0, colon);
            portOut = hostHeader.substr(colon + 1);
        } else {
            hostOut = hostHeader;
            portOut = "80";
        }
        // path is the uri itself (origin-form)
        pathOut = uri;
        return true;
    }
}

// connect to server by host:port; returns socket fd or -1
int connectToHostPort(const std::string &host, const std::string &port, std::string *resolvedIP = nullptr) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rv = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rv != 0) {
        logf("getaddrinfo(%s:%s) failed: %s", host.c_str(), port.c_str(), gai_strerror(rv));
        return -1;
    }
    int s = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        // optional: set socket non-blocking or timeouts (omitted for simplicity)
        if (connect(s, p->ai_addr, p->ai_addrlen) < 0) {
            close(s);
            s = -1;
            continue;
        }
        // connected
        char ipbuf[INET6_ADDRSTRLEN];
        void *addrptr = nullptr;
        if (p->ai_family == AF_INET) addrptr = &(((struct sockaddr_in*)p->ai_addr)->sin_addr);
        else addrptr = &(((struct sockaddr_in6*)p->ai_addr)->sin6_addr);
        if (resolvedIP) inet_ntop(p->ai_family, addrptr, ipbuf, sizeof(ipbuf));
        if (resolvedIP) *resolvedIP = ipbuf;
        break;
    }
    freeaddrinfo(res);
    return s;
}

// bidirectional pipe between two sockets (used for CONNECT tunnels)
// returns when one side closes
void tunnelRelay(int clientSock, int serverSock) {
    // Use two threads or simple loop with select; simple loop with splice-like behavior:
    fd_set readfds;
    char buf[BUF_SIZE];
    int maxfd = std::max(clientSock, serverSock) + 1;
    bool clientOpen = true, serverOpen = true;
    while (clientOpen && serverOpen) {
        FD_ZERO(&readfds);
        FD_SET(clientSock, &readfds);
        FD_SET(serverSock, &readfds);
        int rv = select(maxfd, &readfds, nullptr, nullptr, nullptr);
        if (rv <= 0) break;
        if (FD_ISSET(clientSock, &readfds)) {
            ssize_t n = recv(clientSock, buf, sizeof(buf), 0);
            if (n <= 0) { clientOpen = false; shutdown(serverSock, SHUT_WR); }
            else { if (sendAll(serverSock, buf, (size_t)n) <= 0) { serverOpen = false; shutdown(clientSock, SHUT_RD); } }
        }
        if (FD_ISSET(serverSock, &readfds)) {
            ssize_t n = recv(serverSock, buf, sizeof(buf), 0);
            if (n <= 0) { serverOpen = false; shutdown(clientSock, SHUT_WR); }
            else { if (sendAll(clientSock, buf, (size_t)n) <= 0) { clientOpen = false; shutdown(serverSock, SHUT_RD); } }
        }
    }
}

// Core per-client handler
void *client_thread(void *arg) {
    int clientSock = *((int*)arg);
    free(arg);

    // Set a generous recv timeout to avoid hanging forever (optional)
    struct timeval tv;
    tv.tv_sec = 300; tv.tv_usec = 0;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    // Read request line
    std::string reqLine;
    if (!recvLine(clientSock, reqLine)) {
        close(clientSock);
        return nullptr;
    }
    // Read headers
    std::string headers;
    if (!readHeaders(clientSock, headers)) {
        close(clientSock);
        return nullptr;
    }

    logf("Received request-line: %s", reqLine.c_str());

    // If the method is CONNECT -> open tunnel
    std::istringstream iss(reqLine);
    std::string method, uri, version;
    iss >> method >> uri >> version;
    if (method == "CONNECT") {
        // uri is host:port
        size_t col = uri.find(':');
        std::string host = (col==std::string::npos)?uri:uri.substr(0,col);
        std::string port = (col==std::string::npos)?"443":uri.substr(col+1);
        logf("CONNECT request for %s:%s", host.c_str(), port.c_str());
        // connect to target
        std::string resolved;
        int serverSock = connectToHostPort(host, port, &resolved);
        if (serverSock < 0) {
            std::string resp = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
            sendAll(clientSock, resp.c_str(), resp.size());
            close(clientSock);
            return nullptr;
        }
        // send 200 to client to establish tunnel
        std::string resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
        sendAll(clientSock, resp.c_str(), resp.size());
        logf("Tunnel established to %s (%s:%s)", host.c_str(), resolved.c_str(), port.c_str());
        // now relay data both ways until closed
        tunnelRelay(clientSock, serverSock);
        close(serverSock);
        close(clientSock);
        logf("Tunnel closed for %s:%s", host.c_str(), port.c_str());
        return nullptr;
    }

    // Normal HTTP (GET/POST/etc): need to parse host:port and send origin-form request to server
    std::string host, port, path;
    if (!determineHostPortAndPath(reqLine, headers, host, port, path)) {
        // cannot determine host
        std::string resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        sendAll(clientSock, resp.c_str(), resp.size());
        close(clientSock);
        return nullptr;
    }

    // Connect to server
    std::string resolvedIP;
    int serverSock = connectToHostPort(host, port, &resolvedIP);
    if (serverSock < 0) {
        std::string resp = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
        sendAll(clientSock, resp.c_str(), resp.size());
        close(clientSock);
        return nullptr;
    }

    logf("%s %s -> %s:%s (%s)", method.c_str(), path.c_str(), host.c_str(), port.c_str(), resolvedIP.c_str());

    // Build the request to send to origin server:
    // Replace request line's URI with path (origin-form).
    std::ostringstream outReq;
    outReq << method << " " << path << " " << version << "\r\n";

    // Filter headers: some proxies remove Proxy-Connection header and keep Connection
    // Also if request had absolute URI, Host header is already present in headers; keep it.
    // We will remove "Proxy-Connection" and downcase header keys for checking.
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // trim leading
        size_t a = line.find_first_not_of(" \t");
        if (a==std::string::npos) continue;
        std::string key = line.substr(0, line.find(':'));
        std::string lower = key;
        for (char &c : lower) c = tolower(c);
        if (lower == "proxy-connection") continue;
        if (lower == "connection") {
            // keep as-is if want persistent connections; here change to close for simplicity
            outReq << "Connection: close\r\n";
            continue;
        }
        outReq << line << "\r\n";
    }
    outReq << "\r\n";

    std::string toSend = outReq.str();
    if (sendAll(serverSock, toSend.c_str(), toSend.size()) < 0) {
        logf("Failed sending request to server %s:%s", host.c_str(), port.c_str());
        close(serverSock);
        close(clientSock);
        return nullptr;
    }

    // If client may have sent a request body (POST), we need to forward it.
    // For simplicity, we will check for Content-Length and forward exact bytes if present.
    // (Chunked / Transfer-Encoding: chunked is not fully implemented here)
    // Extract Content-Length header if present
    size_t contentLength = 0;
    {
        std::istringstream hs2(headers);
        while (std::getline(hs2, line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            size_t pos = line.find(':');
            if (pos==std::string::npos) continue;
            std::string k = line.substr(0,pos);
            std::string v = line.substr(pos+1);
            // trim
            auto trim = [](std::string &s){
                size_t a = s.find_first_not_of(" \t");
                size_t b = s.find_last_not_of(" \t\r\n");
                if (a==std::string::npos || b==std::string::npos) { s.clear(); return; }
                s = s.substr(a, b-a+1);
            };
            trim(k); trim(v);
            for (char &c: k) c = tolower(c);
            if (k == "content-length") {
                contentLength = std::stoul(v);
            }
        }
    }
    // If there's a body, read from client and forward to server
    if (contentLength > 0) {
        size_t remaining = contentLength;
        char buf[BUF_SIZE];
        while (remaining > 0) {
            size_t chunk = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
            ssize_t n = recv(clientSock, buf, chunk, 0);
            if (n <= 0) { break; }
            if (sendAll(serverSock, buf, (size_t)n) < 0) break;
            remaining -= (size_t)n;
        }
    }

    // Now relay the response from server back to client (simple streaming)
    char buffer[BUF_SIZE];
    ssize_t r;
    while ((r = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
        if (sendAll(clientSock, buffer, (size_t)r) < 0) break;
    }

    close(serverSock);
    close(clientSock);
    logf("Completed request for %s:%s %s", host.c_str(), port.c_str(), path.c_str());
    return nullptr;
}

int main(int argc, char *argv[]) {
    // ignore SIGPIPE to avoid dying if writing to closed sockets
    signal(SIGPIPE, SIG_IGN);

    const char *port = (argc >= 2) ? argv[1] : DEFAULT_PORT;

    // set up listening socket
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int rv = getaddrinfo(NULL, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }
    int listenSock = -1;
    struct addrinfo *p;
    for (p = res; p != NULL; p = p->ai_next) {
        listenSock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenSock < 0) continue;
        int yes = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listenSock, p->ai_addr, p->ai_addrlen) < 0) {
            close(listenSock);
            listenSock = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (listenSock < 0) {
        fprintf(stderr, "Failed to bind to port %s\n", port);
        return 1;
    }
    if (listen(listenSock, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }
    logf("Proxy listening on port %s", port);

    while (true) {
        struct sockaddr_storage clientAddr;
        socklen_t addrlen = sizeof(clientAddr);
        int clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &addrlen);
        if (clientSock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        // optionally set socket options (timeouts) here
        // spawn thread
        pthread_t tid;
        int *pclient = (int*)malloc(sizeof(int));
        *pclient = clientSock;
        if (pthread_create(&tid, NULL, client_thread, pclient) != 0) {
            logf("pthread_create failed");
            close(clientSock);
            free(pclient);
        } else {
            pthread_detach(tid);
        }
    }

    close(listenSock);
    return 0;
}
