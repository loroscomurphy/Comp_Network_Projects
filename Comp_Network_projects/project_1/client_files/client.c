//C code for client

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#define SERVER_IP "127.0.0.1"
#define PORT 65432
#define BUFSIZE 4096

static ssize_t send_all(int sock, const void *buf, size_t len) 
{
    size_t sent = 0;
    while (sent < len) 
    {
        ssize_t n = send(sock, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return n;
        sent += (size_t)n;
    }

    return (ssize_t)sent;
}

static ssize_t recv_all(int sock, void *buf, size_t len) 
{
    size_t recvd = 0;
    while (recvd < len) 
    {
        ssize_t n = recv(sock, (char*)buf + recvd, len - recvd, 0);
        if (n <= 0) return n;
        recvd += (size_t)n;
    }

    return (ssize_t)recvd;
}

static ssize_t recv_line(int sock, char *out, size_t maxlen) 
{
    size_t i = 0;
    while (i + 1 < maxlen) 
    {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return n;
        out[i++] = c;
        if (c == '\n') break;
    }

    out[i] = '\0';
    return (ssize_t)i;
}

static int do_push(int sock, const char *filename) 
{
    // Open file
    FILE *fp = fopen(filename, "rb");
    if (!fp) 
    {
        printf("File not found: %s\n", filename);
        return -1;
    }

    // Get size
    struct stat st;
    if (stat(filename, &st) != 0)
    {
        printf("stat failed for %s\n", filename);
        fclose(fp);
        return -1;
    }
    unsigned long long sz = (unsigned long long)st.st_size;

    // 1) Send "push <file>\n"
    char cmd[BUFSIZE];
    int cl = snprintf(cmd, sizeof(cmd), "push %s\n", filename);

    if(send_all(sock, cmd, (size_t)cl) <= 0) 
    { 
        fclose(fp); return -1; 
    }

    // 2) Wait "OK\n"
    char line[BUFSIZE];
    ssize_t ln = recv_line(sock, line, sizeof(line));
    if(ln <= 0 || strcmp(line, "OK\n") != 0) 
    { 
        fclose(fp); 
        return -1; 
    }

    // 3) Send "SIZE <n>\n"
    int hl = snprintf(cmd, sizeof(cmd), "SIZE %llu\n", sz);
    if (send_all(sock, cmd, (size_t)hl) <= 0) 
    { 
        fclose(fp);
        return -1;
    }

    // 4) Wait "OK\n"
    ln = recv_line(sock, line, sizeof(line));
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
        if (send_all(sock, buffer, nread) <= 0) 
        { 
            fclose(fp); 
            return -1; 
        }
    }

    fclose(fp);

    // 6) Wait "DONE\n"
    ln = recv_line(sock, line, sizeof(line));
    if (ln <= 0 || strcmp(line, "DONE\n") != 0) 
    {    
        return -1;
    }

    printf("Pushed %s (%llu bytes)\n", filename, sz);
    return 0;
}

static int do_get(int sock, const char *filename) 
{
    // 1) Send "get <file>\n"
    char cmd[BUFSIZE];
    int cl = snprintf(cmd, sizeof(cmd), "get %s\n", filename);
    if(send_all(sock, cmd, (size_t)cl) <= 0) 
    {
        return -1;
    }

    // 2) Expect "SIZE <n>\n" or "ERR notfound\n"
    char line[BUFSIZE];
    ssize_t ln = recv_line(sock, line, sizeof(line));
    if (ln <= 0) return -1;

    if(strncmp(line, "ERR", 3) == 0) 
    {
        printf("%s", line); // show error from server
        return -1;
    }

    unsigned long long sz = 0;
    if(sscanf(line, "SIZE %llu", &sz) != 1) 
    {
        printf("Bad SIZE from server\n");
        return -1;
    }

    // 3) Send "OK\n"
    if(send_all(sock, "OK\n", 3) <= 0) return -1;

    // 4) Receive file
    FILE *fp = fopen(filename, "wb");
    if(!fp) 
    {
        printf("Could not create %s\n", filename);
        return -1;
    }

    char buffer[BUFSIZE];
    unsigned long long remaining = sz;
    while (remaining > 0) {
        size_t chunk = (remaining > BUFSIZE) ? BUFSIZE : (size_t)remaining;
        ssize_t n = recv_all(sock, buffer, chunk);
        if (n <= 0) 
        { 
            fclose(fp); 
            return -1;
        }

        fwrite(buffer, 1, (size_t)n, fp);
        remaining -= (unsigned long long)n;
    }

    fclose(fp);

    // 5) Wait "DONE\n"
    ln = recv_line(sock, line, sizeof(line));
    if (ln <= 0 || strcmp(line, "DONE\n") != 0) 
    {
        return -1;
    }

    printf("Got %s (%llu bytes)\n", filename, sz);
    return 0;
}

int main(void) 
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) 
    { 
        perror("socket"); 
        exit(1); 
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    srv.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) 
    {
        perror("connect");
        close(sock);
        exit(1);
    }

    printf("Connected to server.\n");

    char input[BUFSIZE];

    for(;;) 
    {
        printf("Enter command (push <file>, get <file>, quit): ");
        if (!fgets(input, sizeof(input), stdin))
        {
            break;
        }

        // trim newline
        input[strcspn(input, "\n")] = '\0';

        if (strncmp(input, "push ", 5) == 0) 
        {
            do_push(sock, input + 5);
        } 

        else if (strncmp(input, "get ", 4) == 0)
        {
            do_get(sock, input + 4);
        } 

        else if (strcmp(input, "quit") == 0) 
        {
            send_all(sock, "quit\n", 5);
            break;
        } 

        else 
        {
            printf("Unknown command.\n");
        }
    }

    close(sock);
    return 0;
}

