/* proxy_http.cpp
   Completed HTTP/HTTPS proxy that reads a single forbidden list file (forbidden.txt)
   and blocks requests/responses accordingly.

   forbidden.txt rules:
     - Lines starting with "site:" (case-insensitive) are forbidden hostnames.
     - Other non-empty, non-# lines are forbidden keywords.
*/

#include "proxy_http.h"

// Detect forbidden word inside body text
static bool containsForbidden(const std::string &body,
                              const std::vector<std::string> &forbiddenWords)
{
    for (const auto &w : forbiddenWords) {
        if (body.find(w) != std::string::npos)
            return true;
    }
    return false;
}

static void send503(int clientSock)
{
    const char *resp =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<html><body><h2>Forbidden content detected</h2></body></html>";
    send(clientSock, resp, strlen(resp), 0);
}


// Forward declarations
void *client_thread(void *arg);

// Single-file forbidden lists
static std::vector<std::string> forbiddenWords;
static std::vector<std::string> forbiddenSites;

// Utility: print timestamped log
void logf(const char *fmt, ...) 
{
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
ssize_t sendAll(int sock, const void *buf, size_t len) 
{
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

// recvExact: read exactly len bytes (unless EOF/error), returns bytes read or -1 on error
ssize_t recvExact(int sock, void *buf, size_t len)
{
	size_t total = 0;
	char *p = (char*)buf;
	while (total < len) {
		ssize_t n = recv(sock, p + total, len - total, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return (ssize_t)total; // EOF
		total += (size_t)n;
	}
	return (ssize_t)total;
}

// recvLine: read until '\n' (returns line without trailing CRLF), or false on EOF/error
bool recvLine(int sock, std::string &out, int maxlen = 8*1024)
{
	out.clear();
	char ch;
	while (true)
	{
		ssize_t n = recv(sock, &ch, 1, 0);
		if (n <= 0)
			return false;

		if (ch == '\r')
		{
			// peek next; if '\n' consume it
			ssize_t m = recv(sock, &ch, 1, MSG_PEEK);
			if (m > 0 && ch == '\n') {
				recv(sock, &ch, 1, 0);
			}
			break;
		}

		if (ch == '\n')
			break;

		out.push_back(ch);
		if ((int)out.size() >= maxlen)
			return false;
	}

	return true;
}

// readHeaders: read header lines until blank line; returns full headers as one string (trailing CRLFCRLF included)
bool readHeaders(int sock, std::string &headersOut)
{
	headersOut.clear();
	std::string line;
	while (true)
	{
		if (!recvLine(sock, line))
			return false;

		if (line.empty())
			break; // end of headers

		headersOut += line;
		headersOut += "\r\n";
		// safety: extremely large headers should abort
		if (headersOut.size() > 64*1024)
			return false;
	}
	headersOut += "\r\n";
	return true;
}

// read a file of simple lines into vector<string>, trimming whitespace and ignoring empty or # comments
static std::vector<std::string> readListFile(const char *fname)
{
	std::vector<std::string> out;
	FILE *f = fopen(fname, "r");
	if (!f) return out;
	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		std::string s(buf);
		// trim
		size_t a = s.find_first_not_of(" \t\r\n");
		size_t b = s.find_last_not_of(" \t\r\n");
		if (a==std::string::npos || b==std::string::npos) continue;
		s = s.substr(a, b-a+1);
		if (s.size() == 0) continue;
		if (s[0] == '#') continue;
		// to lower
		for (char &c : s) c = (char)tolower((unsigned char)c);
		out.push_back(s);
	}
	fclose(f);
	return out;
}

// Load single forbidden file "forbidden.txt" and split into words and sites.
// Lines starting with "site:" (case-insensitive) are pushed to forbiddenSites (the remainder).
// Other lines go to forbiddenWords.
static void loadForbiddenSingleFile(const char *filename)
{
	forbiddenWords.clear();
	forbiddenSites.clear();

	FILE *f = fopen(filename, "r");
	if (!f) {
		logf("Warning: %s not found — no filtering will apply", filename);
		return;
	}
	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		std::string s(buf);
		// trim
		size_t a = s.find_first_not_of(" \t\r\n");
		size_t b = s.find_last_not_of(" \t\r\n");
		if (a==std::string::npos || b==std::string::npos) continue;
		s = s.substr(a, b-a+1);
		if (s.empty()) continue;
		if (s[0] == '#') continue;
		// to lower
		std::string lower = s;
		for (char &c : lower) c = (char)tolower((unsigned char)c);
		if (lower.rfind("site:", 0) == 0) {
			std::string host = lower.substr(5);
			// trim again
			size_t aa = host.find_first_not_of(" \t");
			size_t bb = host.find_last_not_of(" \t");
			if (aa!=std::string::npos && bb!=std::string::npos) {
				host = host.substr(aa, bb-aa+1);
				forbiddenSites.push_back(host);
			}
		} else {
			forbiddenWords.push_back(lower);
		}
	}
	fclose(f);
	logf("Loaded %zu forbidden words and %zu forbidden sites from %s",
	     forbiddenWords.size(), forbiddenSites.size(), filename);
}

static std::string toLowerCopy(const std::string &s) {
	std::string r = s;
	for (char &c : r) c = (char)tolower((unsigned char)c);
	return r;
}

// check substring match case-insensitive between haystackLower and any forbidden word
//static bool containsForbidden(const std::string &haystackLower, const std::vector<std::string> &forbidden)
//{
	//for (const auto &w : forbidden) {
	//	if (w.empty()) continue;
	//	if (haystackLower.find(w) != std::string::npos) return true;
	//}
	//return false;
//}

// send a simple HTML error response (code like "403", reason like "Forbidden")
static void sendErrorHtml(int clientSock, const char *code, const char *reason, const char *bodytext)
{
	std::ostringstream resp;
	resp << "HTTP/1.1 " << code << " " << reason << "\r\n";
	resp << "Content-Type: text/html; charset=utf-8\r\n";
	resp << "Connection: close\r\n";
	std::string body = "<html><head><title>" + std::string(code) + " " + reason + "</title></head><body><h1>" +
		std::string(code) + " " + reason + "</h1><p>" + std::string(bodytext) + "</p></body></html>";
	resp << "Content-Length: " << body.size() << "\r\n";
	resp << "\r\n";
	resp << body;
	std::string s = resp.str();
	sendAll(clientSock, s.c_str(), s.size());
}

// parse host:port from an URL (absolute URI) or Host header fallback
// Outputs host (string), port (string), and pathToSend (string) which is the origin-form path to send to server.
bool determineHostPortAndPath(const std::string &requestLine, const std::string &headers, std::string &hostOut, std::string &portOut, std::string &pathOut) 
{
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
		} 
        else {
			hostOut = authority;
			// default port
			if (uri.rfind("https://", 0) == 0) portOut = "443";
			else portOut = "80";
		}
		return true;
	} 
    else {
		// origin-form: use Host header
		// extract Host header from headers string
		std::istringstream hs(headers);
		std::string line;
		std::string hostHeader;
		while (std::getline(hs, line)) {
			if (line.size() > 5 && (line[0]=='H' || line[0]=='h')) {
				// transform to lower for safe compare
				std::string lower=line;
				for(char &c: lower) c=tolower((unsigned char)c);
				if (lower.rfind("host:", 0) == 0) {
					hostHeader = line.substr(5);
					break;
				}
			}
		}
		// trim whitespace
		auto trim = [](std::string &s) {
			size_t a = s.find_first_not_of(" \t");
			size_t b = s.find_last_not_of(" \t\r\n");
			if (a==std::string::npos || b==std::string::npos) {
				s.clear();
				return;
			}
			s = s.substr(a, b-a+1);
		};
		trim(hostHeader);
		if (hostHeader.empty()) return false;
		size_t colon = hostHeader.find(':');
		if (colon != std::string::npos) {
			hostOut = hostHeader.substr(0, colon);
			portOut = hostHeader.substr(colon + 1);
		} 
        else {
			hostOut = hostHeader;
			portOut = "80";
		}
		// path is the uri itself (origin-form)
		pathOut = uri;
		return true;
	}
}

// connect to server by host:port; returns socket fd or -1
int connectToHostPort(const std::string &host, const std::string &port, std::string *resolvedIP = nullptr) 
{
	struct addrinfo hints {
	}, *res = nullptr;
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
void tunnelRelay(int clientSock, int serverSock) 
{
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
			if (n <= 0) {
				clientOpen = false;
				shutdown(serverSock, SHUT_WR);
			}
			else {
				if (sendAll(serverSock, buf, (size_t)n) <= 0) {
					serverOpen = false;
					shutdown(clientSock, SHUT_RD);
				}
			}
		}
		if (FD_ISSET(serverSock, &readfds)) {
			ssize_t n = recv(serverSock, buf, sizeof(buf), 0);
			if (n <= 0) {
				serverOpen = false;
				shutdown(clientSock, SHUT_WR);
			}
			else {
				if (sendAll(clientSock, buf, (size_t)n) <= 0) {
					clientOpen = false;
					shutdown(serverSock, SHUT_RD);
				}
			}
		}
	}
}

// Helper: parse headers into map<string,string> (lowercased keys)
static std::map<std::string,std::string> parseHeadersToMap(const std::string &headers)
{
	std::map<std::string,std::string> m;
	std::istringstream hs(headers);
	std::string line;
	while (std::getline(hs, line)) {
		if (!line.empty() && line.back()=='\r') line.pop_back();
		if (line.empty()) continue;
		size_t pos = line.find(':');
		if (pos==std::string::npos) continue;
		std::string k = line.substr(0,pos);
		std::string v = line.substr(pos+1);
		// trim
		auto trim = [](std::string &s) {
			size_t a = s.find_first_not_of(" \t");
			size_t b = s.find_last_not_of(" \t\r\n");
			if (a==std::string::npos || b==std::string::npos) {
				s.clear();
				return;
			}
			s = s.substr(a, b-a+1);
		};
		trim(k); trim(v);
		for (char &c : k) c = (char)tolower((unsigned char)c);
		m[k] = v;
	}
	return m;
}

// Read chunked response from serverSock. We will collect both raw bytes (including chunk headers and CRLFs)
// into rawOut (so we can forward the response unchanged) and collect decoded data bytes into decodedOut (for searching).
// Returns true on success, false on error.
static bool readChunkedResponse(int serverSock, int clientSock, std::string &rawOut, std::string &decodedOut)
{
	std::string line;
	while (true) {
		// read chunk-size line
		if (!recvLine(serverSock, line)) return false;
		// line does not contain CRLF; add it to rawOut with CRLF
		rawOut += line;
		rawOut += "\r\n";
		// parse chunk size (hex)
		size_t semi = line.find(';'); // ignore chunk extensions
		std::string hex = (semi==std::string::npos) ? line : line.substr(0, semi);
		// trim
		size_t a = hex.find_first_not_of(" \t");
		size_t b = hex.find_last_not_of(" \t");
		if (a==std::string::npos || b==std::string::npos) return false;
		hex = hex.substr(a, b-a+1);
		unsigned long chunkSize = 0;
		try {
			chunkSize = std::stoul(hex, nullptr, 16);
		} catch (...) {
			return false;
		}
		if (chunkSize == 0) {
			// final chunk: read trailers until blank line
			std::string hdr;
			while (true) {
				if (!recvLine(serverSock, hdr)) return false;
				if (hdr.empty()) {
					rawOut += "\r\n";
					break;
				}
				rawOut += hdr;
				rawOut += "\r\n";
			}
			return true;
		}
		// Read exactly chunkSize bytes, append to rawOut and decodedOut
		std::string chunkData;
		chunkData.resize(chunkSize);
		ssize_t got = recvExact(serverSock, &chunkData[0], chunkSize);
		if (got < 0 || (size_t)got != chunkSize) return false;
		rawOut.append(chunkData);
		decodedOut.append(chunkData);
        // check forbidden content immediately
        if (containsForbidden(decodedOut, forbiddenWords)) {
            send503(clientSock);
            return false;
        }

		// after chunk data there is a CRLF we must consume
		char crlf[2];
		ssize_t c = recvExact(serverSock, crlf, 2);
		if (c != 2) return false;
		rawOut.append("\r\n");
	}
	// unreachable
	return false;
}

// Read a response body where Content-Length is used. rawOut will get exact bytes, decodedOut gets same bytes.
static bool readContentLengthResponse(int serverSock, int clientSock, size_t contentLength, std::string &rawOut, std::string &decodedOut)
{
	size_t remaining = contentLength;
	std::vector<char> buffer(BUF_SIZE);
	while (remaining > 0) {
		size_t toRead = (remaining < buffer.size())?remaining:buffer.size();
		ssize_t n = recvExact(serverSock, buffer.data(), toRead);
		if (n <= 0) return false;
		rawOut.append(buffer.data(), (size_t)n);
		decodedOut.append(buffer.data(), (size_t)n);
        if (containsForbidden(decodedOut, forbiddenWords)) {
            send503(clientSock);
            return false;
        }

		remaining -= (size_t)n;
	}
	return true;
}

// Read until server closes connection (used for HTTP/1.0 or when no length given). Store all bytes into rawOut and decodedOut.
static bool readUntilCloseResponse(int serverSock,int clientSock, std::string &rawOut, std::string &decodedOut)
{
	std::vector<char> buffer(BUF_SIZE);
	ssize_t n;
	while ((n = recv(serverSock, buffer.data(), buffer.size(), 0)) > 0) {
		rawOut.append(buffer.data(), (size_t)n);
		decodedOut.append(buffer.data(), (size_t)n);
        if (containsForbidden(decodedOut, forbiddenWords)) {
            send503(clientSock);
            return false;
        }
	}
	// n==0 is normal EOF
	return true;
}

// Core per-client handler
void *client_thread(void *arg) 
{
	int clientSock = *((int*)arg);
	free(arg);

	// Set a generous recv timeout to avoid hanging forever (optional)
	struct timeval tv;
	tv.tv_sec = 300;
	tv.tv_usec = 0;
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

	// read possible request body (Content-Length) into local buffer BEFORE connecting so we can inspect it
	size_t reqContentLength = 0;
	std::string reqBody;
	{
		std::istringstream hs2(headers);
		std::string line;
		while (std::getline(hs2, line)) {
			if (!line.empty() && line.back()=='\r') line.pop_back();
			size_t pos = line.find(':');
			if (pos==std::string::npos) continue;
			std::string k = line.substr(0,pos);
			std::string v = line.substr(pos+1);
			// trim
			auto trim = [](std::string &s) {
				size_t a = s.find_first_not_of(" \t");
				size_t b = s.find_last_not_of(" \t\r\n");
				if (a==std::string::npos || b==std::string::npos) {
					s.clear();
					return;
				}
				s = s.substr(a, b-a+1);
			};
			trim(k);
			trim(v);
			for (char &c: k) c = (char)tolower((unsigned char)c);
			if (k == "content-length") {
				try {
					reqContentLength = std::stoul(v);
				} catch (...) { reqContentLength = 0; }
			}
		}
	}
	if (reqContentLength > 0) {
		reqBody.reserve(reqContentLength);
		size_t remaining = reqContentLength;
		std::vector<char> buf(BUF_SIZE);
		while (remaining > 0) {
			size_t chunk = (remaining < buf.size()) ? remaining : buf.size();
			ssize_t n = recv(clientSock, buf.data(), chunk, 0);
			if (n <= 0) break;
			reqBody.append(buf.data(), (size_t)n);
			remaining -= (size_t)n;
		}
	}

	// Quick check: if request-line or headers or body contain forbidden words -> 403
	std::string combinedReqLower = toLowerCopy(reqLine + "\r\n" + headers + reqBody);
	if (containsForbidden(combinedReqLower, forbiddenWords)) {
		logf("Blocking request from client: forbidden word in request");
		sendErrorHtml(clientSock, "403", "Forbidden", "Your request contains forbidden words and was blocked by the proxy.");
		close(clientSock);
		return nullptr;
	}

	// Parse method
	std::istringstream iss(reqLine);
	std::string method, uri, version;
	if (!(iss >> method >> uri >> version)) {
		sendErrorHtml(clientSock, "400", "Bad Request", "Malformed request line.");
		close(clientSock);
		return nullptr;
	}

	// For CONNECT requests (HTTPS tunneling): check forbidden sites, then tunnel
	if (method == "CONNECT") {
		// uri is host:port
		size_t col = uri.find(':');
		std::string host = (col==std::string::npos)?uri:uri.substr(0,col);
		std::string port = (col==std::string::npos)?"443":uri.substr(col+1);
		std::string hostLower = toLowerCopy(host);
		for (const auto &s : forbiddenSites) {
			if (hostLower.find(s) != std::string::npos) {
				logf("Blocking CONNECT to forbidden site %s", host.c_str());
				sendErrorHtml(clientSock, "403", "Forbidden", "CONNECT to this site is blocked by the proxy.");
				close(clientSock);
				return nullptr;
			}
		}

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

	// check host against forbiddenSites
	std::string hostLower = toLowerCopy(host);
	for (const auto &s : forbiddenSites) {
		if (hostLower.find(s) != std::string::npos) {
			logf("Blocking request to forbidden host %s", host.c_str());
			sendErrorHtml(clientSock, "403", "Forbidden", "Access to this host is blocked by the proxy.");
			close(clientSock);
			return nullptr;
		}
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

	// Filter headers: remove Proxy-Connection and replace Connection with close
	std::istringstream hs(headers);
	std::string line;
	bool hasHostHeader = false;
	while (std::getline(hs, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		// trim leading
		size_t a = line.find_first_not_of(" \t");
		if (a==std::string::npos) continue;
		size_t colonPos = line.find(':');
		std::string key = (colonPos==std::string::npos) ? line : line.substr(0, colonPos);
		std::string lower = key;
		for (char &c : lower) c = (char)tolower((unsigned char)c);
		if (lower == "proxy-connection") continue;
		if (lower == "connection") {
			outReq << "Connection: close\r\n";
			continue;
		}
		if (lower == "host") hasHostHeader = true;
		outReq << line << "\r\n";
	}
	if (!hasHostHeader) {
		outReq << "Host: " << host << "\r\n";
	}
	outReq << "\r\n";

	std::string toSend = outReq.str();
	if (sendAll(serverSock, toSend.c_str(), toSend.size()) < 0) {
		logf("Failed sending request to server %s:%s", host.c_str(), port.c_str());
		close(serverSock);
		close(clientSock);
		return nullptr;
	}

	// forward request body (we already read it from client and inspected it)
	if (!reqBody.empty()) {
		if (sendAll(serverSock, reqBody.data(), reqBody.size()) < 0) {
			logf("Failed forwarding request body to server");
			close(serverSock); close(clientSock);
			return nullptr;
		}
	}

	// Now we must read the SERVER's response & inspect it for forbidden words
	// We will buffer the full response (headers + raw body) in per-thread local buffers.
	// If any forbidden word is found in the response body -> return 503 to client instead of forwarding original.
	// Steps:
	//  1) read status-line
	//  2) read headers
	//  3) parse headers to determine body-length type (chunked, content-length, or connection-close)
	//  4) read body accordingly into rawBody and decodedBody (decodedBody is the textual data for searching)
	//  5) inspect decodedBody for forbidden words
	//  6) send either original response (status+headers+rawBody) or 503 HTML

	// 1) status line
	std::string statusLine;
	if (!recvLine(serverSock, statusLine)) {
		logf("Failed reading status line from server");
		close(serverSock); close(clientSock);
		return nullptr;
	}

	// 2) server headers
	std::string serverHeaders;
	if (!readHeaders(serverSock, serverHeaders)) {
		logf("Failed reading headers from server");
		close(serverSock); close(clientSock);
		return nullptr;
	}

	// Build full headers block to send later
	std::string responseHeaderBlock = statusLine + "\r\n" + serverHeaders;

	// parse headers into map
	auto hdrMap = parseHeadersToMap(serverHeaders);

	// Determine body reading strategy
	bool isChunked = false;
	size_t contentLength = 0;
	bool hasContentLength = false;
	bool shouldCloseAfter = false; // if server will close after body (HTTP/1.0 or missing length)
	if (hdrMap.find("transfer-encoding") != hdrMap.end()) {
		std::string te = toLowerCopy(hdrMap["transfer-encoding"]);
		if (te.find("chunked") != std::string::npos) isChunked = true;
	}
	if (hdrMap.find("content-length") != hdrMap.end()) {
		try {
			contentLength = std::stoul(hdrMap["content-length"]);
			hasContentLength = true;
		} catch (...) { hasContentLength = false; }
	}
	// If no content-length and not chunked and protocol is HTTP/1.0 (or older), server might close after sending body.
	std::string versionLower = toLowerCopy(version);
	if (!isChunked && !hasContentLength) {
		// Treat as connection-close delimited body
		shouldCloseAfter = true;
	}

	// local per-thread buffers (raw body as sent to client, and decoded text for scanning)
	std::string rawBody; rawBody.reserve(1024*16);
	std::string decodedBody; decodedBody.reserve(1024*16);

	bool readOk = true;
	if (isChunked) {
		// read chunked response but preserve raw chunk-stream in rawBody
		readOk = readChunkedResponse(serverSock,clientSock, rawBody, decodedBody);
	} else if (hasContentLength) {
		readOk = readContentLengthResponse(serverSock, clientSock, contentLength, rawBody, decodedBody);
	} else {
		// read until close
		readOk = readUntilCloseResponse(serverSock, clientSock, rawBody, decodedBody);
	}

	if (!readOk) {
		logf("Failed reading response body from server");
		close(serverSock); close(clientSock);
		return nullptr;
	}

	// Prepare text to search. We will lowercase decodedBody and search forbidden words
	std::string decodedLower = toLowerCopy(decodedBody);

	bool forbiddenInResponse = containsForbidden(decodedLower, forbiddenWords);
	if (forbiddenInResponse) {
		logf("Blocking response from server: forbidden content detected");
		sendErrorHtml(clientSock, "503", "Service Unavailable", "The server response contains forbidden content and was blocked by the proxy.");
		close(serverSock);
		close(clientSock);
		return nullptr;
	}

	// No forbidden words — forward original response to client.
	// We forward the header block (status line + headers) exactly as received and then rawBody.
	if (sendAll(clientSock, responseHeaderBlock.c_str(), responseHeaderBlock.size()) < 0) {
		logf("Failed sending response headers to client");
		close(serverSock); close(clientSock);
		return nullptr;
	}
	// send raw body (which for chunked includes the chunk size lines and final trailers)
	if (!rawBody.empty()) {
		if (sendAll(clientSock, rawBody.data(), rawBody.size()) < 0) {
			logf("Failed sending response body to client");
		}
	}

	// close connections
	close(serverSock);
	close(clientSock);
	logf("Completed request for %s:%s %s", host.c_str(), port.c_str(), path.c_str());
	return nullptr;
}

int main(int argc, char *argv[]) 
{
	// ignore SIGPIPE to avoid dying if writing to closed sockets
	signal(SIGPIPE, SIG_IGN);

	// load single forbidden list file
	loadForbiddenSingleFile("forbidden.txt");

	const char *port = (argc >= 2) ? argv[1] : DEFAULT_PORT;

	// set up listening socket
	struct addrinfo hints {
	}, *res;
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
