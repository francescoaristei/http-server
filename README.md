## HTTP Server

[HTTP](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol) is the
protocol that powers the web.


Basic multithreaded HTTP server handling simple GET and POST requests.
GET:
   - GET /files/: Serves files from the specified directory.
   - GET /echo/: Echoes the message back to the client. If request header contains: Accept-Encoding: gzip the echoed message is compressed using the gzip compression algorithm from the  [zlib library](https://www.zlib.net/).
   - GET /user-agent: Returns the user-agent string sent by the client.

POST:
   - POST /files/: Saves the request body to a file in the specified directory.

Each client connection is handled by a different thread from a threads pool using the Producer-Consumer pattern.

### Compiling and Testing

To compile the server: gcc -o server app/server.c -pthread -lz

Some example of testing:

- GET Request to Echo Message: curl http://localhost:4221/echo/Hello

- POST Request to Save Data to data.txt: curl -X POST -d "here-the-data" http://localhost:4221/files/data.txt

- GET User-Agent: curl http://localhost:4221/user-agent
