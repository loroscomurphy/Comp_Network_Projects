# Project #2

#### Due Date

Due: 10/13, 2025

## Description

This project implements a simple TCP/IP proxy server designed to sit between a client and a destination server. The proxy intercepts TCP connections from clients, forwards the requests to the target server, receives responses, and then relays those responses back to the client.
The primary purpose of this proxy is to act as a transparent intermediary, allowing observation or modification of traffic (for debugging, logging, filtering, or other processing purposes) while maintaining minimal overhead and latency.
A proxy server is functionally an intermediary system equipped with both a server and a client.  It basically plays as a server to a client, and plays as a client to a server as shown as: 
+--------+                  +----------------+              +--------+
| client | ---------------- |A    Proxy     B| ------------ | server |
+--------+                  +----------------+              +--------+


### Notes

* If you have a question about the project, you will post a topic to Brightspace/Discussions/Project. You are encouraged to answer to other’s questions in the forum.
* Each member must submit a Peer Rating Form individually and confidentially. Without the peer rating, you are considered not to submit the project even though your team does. You must be fair in the peer evaluation.
* The server in the previous project can be used a server in this project.  However, you need to modify the client in the previous project to add the server’s address and port number in the message (eg. at the first line). Once receiving the message, the A part of the proxy takes the first line as the server’s address, and the remaining will be process as usual thru the B part of the proxy.  