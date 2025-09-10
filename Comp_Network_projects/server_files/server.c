#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <errno.h>

#define PORT 65432
#define BUFSIZE 4096

static ssize_t send_all(int sock, const void *buf, size_t len) 
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return n;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recv_all(int sock, void *buf, size_t len) 
{
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(sock, (char*)buf + recvd, len - recvd, 0);
        if (n <= 0) return n;
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}

// Read a line ending with '\n'.
static ssize_t recv_line(int sock, char *out, size_t maxlen) 
{
    size_t i = 0;
    while (i + 1 < maxlen) {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return n;
        out[i++] = c;
        if (c == '\n') break;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

static int handle_push(int client, const char *filename) 
{
    // 1) Ack command so client can send SIZE
    if (send_all(client, "OK\n", 3) <= 0) return -1;

    // 2) Receive "SIZE <n>\n"
    char line[BUFSIZE];
    ssize_t ln = recv_line(client, line, sizeof(line));
    if (ln <= 0) return -1;

    unsigned long long sz = 0;
    if (sscanf(line, "SIZE %llu", &sz) != 1) 
    {
        send_all(client, "ERR badsize\n", 12);
        return -1;
    }

    // 3) Ack size so client can stream file
    if (send_all(client, "OK\n", 3) <= 0) return -1;

    FILE *fp = fopen(filename, "wb");
    if (!fp) 
    {
        send_all(client, "ERR cannotopen\n", 15);
        return -1;
    }

    // 4) Receive file payload (exactly sz bytes)
    char buffer[BUFSIZE];
    unsigned long long remaining = sz;
    while (remaining > 0) 
    {
        size_t chunk = (remaining > BUFSIZE) ? BUFSIZE : (size_t)remaining;
        ssize_t n = recv_all(client, buffer, chunk);

        if (n <= 0) 
        { 
            fclose(fp); return -1; 
        }

        fwrite(buffer, 1, (size_t)n, fp);

        remaining -= (unsigned long long)n;
    }

    fclose(fp);

    // 5) Done
    send_all(client, "DONE\n", 5);
    return 0;
}

static int handle_get(int client, const char *filename) 
{
    // 1) Open file
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        send_all(client, "ERR notfound\n", 13);
        return -1;
    }

    // 2) Compute size
    struct stat st;
    if (stat(filename, &st) != 0) 
    {
        fclose(fp);
        send_all(client, "ERR stat\n", 9);
        return -1;
    }
    unsigned long long sz = (unsigned long long)st.st_size;

    // 3) Send "SIZE <n>\n"
    char header[128];
    int hl = snprintf(header, sizeof(header), "SIZE %llu\n", sz);
    if (send_all(client, header, (size_t)hl) <= 0) { fclose(fp); return -1; }

    // 4) Wait for "OK\n"
    char line[BUFSIZE];
    ssize_t ln = recv_line(client, line, sizeof(line));
    if (ln <= 0 || strcmp(line, "OK\n") != 0) 
    { 
        fclose(fp); 
        return -1; 
    }

    // 5) Stream file
    char buffer[BUFSIZE];
    size_t nread;
    while ((nread = fread(buffer, 1, sizeof(buffer), fp)) > 0) 
    {
        if (send_all(client, buffer, nread) <= 0) { fclose(fp); return -1; }
    }

    fclose(fp);

    // 6) Done
    send_all(client, "DONE\n", 5);
    return 0;
}

int main(void) 
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) 
    { 
        perror("socket"); exit(1);
    }

    // Allow quick restart
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    {
        perror("bind"); close(server_fd); exit(1);
    }

    if (listen(server_fd, 8) < 0) 
    {
        perror("listen"); close(server_fd); exit(1);
    }

    printf("Server listening on %d...\n", PORT);

    int client_fd = accept(server_fd, (struct sockaddr *)&addr, &addrlen);
    if (client_fd < 0) 
    { 
        perror("accept"); close(server_fd); exit(1); 
    }

    printf("Client connected.\n");

    char line[BUFSIZE];
    for (;;) 
    {
        ssize_t ln = recv_line(client_fd, line, sizeof(line));
        if (ln <= 0) break; // client disconnected
        // Expect: "push <file>\n", "get <file>\n", or "quit\n"

        if (strncmp(line, "push ", 5) == 0) 
        {
            // strip newline
            char *filename = line + 5;
            char *nl = strchr(filename, '\n');
            if (nl) *nl = '\0';
            handle_push(client_fd, filename);
        } 
        
        else if (strncmp(line, "get ", 4) == 0) 
        {
            char *filename = line + 4;
            char *nl = strchr(filename, '\n');
            if (nl) *nl = '\0';
            handle_get(client_fd, filename);
        } 
        
        else if (strcmp(line, "quit\n") == 0) 
        {
            send_all(client_fd, "Goodbye\n", 8);
            break;
        } 
        
        else 
        {
            send_all(client_fd, "ERR unknown\n", 12);
        }
    }

    close(client_fd);
    close(server_fd);
    return 0;
}

