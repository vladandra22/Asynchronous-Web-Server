# Asynchronous Web Server

Web servers which support advanced I/O operations such as: asyncronous operations, non-blocking socket operations, zero-copying and multiplexing I/O operations. It uses the I/O Linux API for sendfile, io_setup and epoll. Client-server communication is done through parsing HTTP requests. It has support for both static (for zero-copying through sendfile) and dynamic (non-blocking sockets) operations. Post-processing is also handled in the dynamic case. For each connection it uses an eventfd file descriptor, and for closing asynchronous operations it uses io_getevents.
