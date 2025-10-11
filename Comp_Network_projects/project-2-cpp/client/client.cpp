#include "client.h"
#include <vector>
#include <sstream>
#include <sys/stat.h>
#include <errno.h>

static const size_t IO_BUFFER_SIZE = 64 * 1024;


/*
GPT-5:

Prompt:Formulate a plan to implement remote file copy between the server and the client but don't change any code yet.

Used plan to implement by hand

*/
// Helper: send exactly len bytes
// Returns false on error/EOF; loops until all bytes are sent
static bool sendAll(int sock, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Helper: receive exactly len bytes
// Returns false on error/EOF; loops until all bytes are read
static bool recvExact(int sock, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, p + total, len - total, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Helper: receive a line ending with \n (not including the \n)
// Bounded to 4096 bytes to avoid unbounded growth
static bool recvLine(int sock, std::string &out) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ch == '\n') break;
        out.push_back(ch);
        if (out.size() > 4096) return false;
    }
    return true;
}

Client::Client(int argc, char* argv[]) {
    if (argc == 2) {
        host = argv[1];
    }
    else {
        std::cerr << "usage: simplex-talk host" << std::endl;
        exit(1);
    }
}

void Client::connectToServer() {
    this->hp = gethostbyname(this->host);
    if (!this->hp) {
        std::cerr << "simplex-talk: unknown host: " << this->host << std::endl;
        exit(1);
    }
    bzero((char *)&this->sin, sizeof(this->sin));
    this->sin.sin_family = AF_INET;
    bcopy(this->hp->h_addr, (char *)&this->sin.sin_addr, this->hp->h_length);
	
	//changed to from SERVER_PORT to PROXY_PORT
    this->sin.sin_port = htons(PROXY_PORT);
    if ((this->s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("simplex-talk: socket");
        exit(1);
        }
        if (connect(this->s, (struct sockaddr *)&this->sin, sizeof(this->sin)) < 0) {
        perror("simplex-talk: connect");
        close(this->s);
        exit(1);
    }
    std::cout << "Client: Connected to server" << std::endl;
	
	// Send server info from client to proxy
    std::string serInfo = std::string(this->host) + " " + std::to_string(SERVER_PORT) + "\n";
    sendAll(this->s, serInfo.data(), serInfo.size());
}


void Client::builtin_put(int argc, char* argv[]) {
    // CLI parsing group: local_path and optional remote_path
    if (argc < 2) {
        std::cerr << "usage: put <local_path> [remote_path]" << std::endl;
        return;
    }

    std::string srcPath = std::string("client_storage/") + argv[1];
    const char* remotePath = (argc >= 3 ? argv[2] : argv[1]);

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) {
        std::cerr << "Failed to open local file: " << srcPath << std::endl;
        return;
    }
    in.seekg(0, std::ios::end);
    std::streampos endPos = in.tellg();
    if (endPos < 0) {
        std::cerr << "Failed to determine file size" << std::endl;
        return;
    }
    size_t fileSize = static_cast<size_t>(endPos);
    in.seekg(0, std::ios::beg);

    // Header group: send protocol header with path length and file size
    std::string header = std::string("put ") + std::to_string(strlen(remotePath)) + " " + std::to_string(fileSize) + "\n";\
    std::cout << "header: " << header << std::endl;
    if (!sendAll(this->s, header.data(), header.size())) {
        std::cerr << "Failed to send PUT header" << std::endl;
        return;
    }
    // Path group: send the raw path bytes
    if (!sendAll(this->s, remotePath, strlen(remotePath))) {
        std::cerr << "Failed to send remote path" << std::endl;
        return;
    }

    // File transfer group: stream local file to server
    std::vector<char> buffer(IO_BUFFER_SIZE);
    size_t remaining = fileSize;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, buffer.size());
        in.read(buffer.data(), static_cast<std::streamsize>(chunk));
        std::streamsize got = in.gcount();
        if (got <= 0) {
            std::cerr << "Unexpected EOF or read error" << std::endl;
            return;
        }
        if (!sendAll(this->s, buffer.data(), static_cast<size_t>(got))) {
            std::cerr << "Failed to send file data" << std::endl;
            return;
        }
        remaining -= static_cast<size_t>(got);
    }

    std::string resp;
    if (!recvLine(this->s, resp)) {
        std::cerr << "Failed to receive response" << std::endl;
        return;
    }
    if (resp.rfind("OK", 0) == 0) {
        std::cout << "Upload succeeded" << std::endl;
    } else {
        std::cerr << "Server error: " << resp << std::endl;
    }
}

void Client::builtin_get(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: get <remote_path> [local_path]" << std::endl;
        return;
    }
    const char* remotePath = argv[1];
    const char* localPath = (argc >= 3 ? argv[2] : argv[1]);

    // Ensure client_storage directory exists
    if (mkdir("client_storage", 0755) != 0 && errno != EEXIST) {
        std::cerr << "Failed to create client_storage directory" << std::endl;
        return;
    }
    std::string finalLocalPath = std::string("client_storage/") + localPath;

    // Header group: send GET header with remote path length
    std::string header = std::string("get ") + std::to_string(strlen(remotePath)) + "\n";
    if (!sendAll(this->s, header.data(), header.size())) {
        std::cerr << "Failed to send GET header" << std::endl;
        return;
    }
    // Path group: send the raw remote path bytes
    if (!sendAll(this->s, remotePath, strlen(remotePath))) {
        std::cerr << "Failed to send remote path" << std::endl;
        return;
    }

    // Response group: read OK <size> line then stream file to disk
    std::string resp;
    if (!recvLine(this->s, resp)) {
        std::cerr << "Failed to receive response" << std::endl;
        return;
    }
    if (resp.rfind("OK ", 0) != 0) {
        std::cerr << "Server error: " << resp << std::endl;
        return;
    }

    size_t size = 0;
    {
        std::istringstream iss(resp.substr(3));
        iss >> size;
        if (!iss) {
            std::cerr << "Malformed OK header: " << resp << std::endl;
            return;
        }
    }

    std::ofstream out(finalLocalPath, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open local file for writing: " << finalLocalPath << std::endl;
        return;
    }

    std::vector<char> buffer(IO_BUFFER_SIZE);
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, buffer.size());
        if (!recvExact(this->s, buffer.data(), chunk)) {
            std::cerr << "Failed to receive file data" << std::endl;
            return;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(chunk));
        if (!out) {
            std::cerr << "Failed to write local file" << std::endl;
            return;
        }
        remaining -= chunk;
    }

    std::cout << "Download succeeded: " << finalLocalPath << std::endl;
}

void Client::registerCommands() {
    // Register "put" command with a lambda that calls the member function

    /*
    Formatting for group members:

    client.h:
    class Client {
        ...
        public:
            
            void builtin_put(int argc, char* argv[]);
        
    };

    client.cpp:
    void Client::builtin_put(int argc, char* argv[]) {
        // your function code here
    }

    in Client::registerCommands():
    this->commandHandler.registerCommand("put", [this](int argc, char* argv[]) {
        this->builtin_put(argc, argv);
        return 0;
    });

    */
    this->commandHandler.registerCommand("put", [this](int argc, char* argv[]) {
        this->builtin_put(argc, argv);
        return 0;
    });
    this->commandHandler.registerCommand("get", [this](int argc, char* argv[]) {
        this->builtin_get(argc, argv);
        return 0;
    });
}

void Client::mainloop() {
    std::cout << "$ ";
    while (fgets(this->buf, sizeof(this->buf), stdin)) {
        this->buf[MAX_LINE-1] = '\0';
        this->len = strlen(this->buf) + 1;
        this->commandHandler.executeCommand(this->buf);
        std::cout << "$ ";
    }
}

int main(int argc, char* argv[]) {
    Client client(argc, argv);
    client.connectToServer();
    client.registerCommands();
    client.mainloop();
    return 0;
}