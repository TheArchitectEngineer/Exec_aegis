/* httpd_bin.c — minimal HTTP server for socket API testing */
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

static const char RESPONSE[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 17\r\n"
    "\r\n"
    "Hello from Aegis\n";

int main(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return 1;

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = __builtin_bswap16(80);
    addr.sin_addr.s_addr = 0;  /* INADDR_ANY */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    if (listen(srv, 4) < 0) return 1;

    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        char buf[256];
        read(cli, buf, sizeof(buf) - 1);  /* drain request */
        write(cli, RESPONSE, sizeof(RESPONSE) - 1);
        close(cli);
    }
}
