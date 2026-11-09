#include "hw/misc/torq-protocol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int sockets[2];
    int received_fd;
    int shared_fd = open("/dev/null", O_RDONLY);
    const char contents[] = "torq protocol";
    TorqMessage sent = {
        .type = TORQ_MSG_RAM,
        .id = 17,
        .size = sizeof(contents),
        .generation = 42,
    };
    TorqMessage received;
    void *payload;

    assert(shared_fd >= 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(torq_message_send(sockets[0], &sent, contents, shared_fd) == 0);
    assert(torq_message_receive(sockets[1], &received, &payload,
                               &received_fd) == 0);
    assert(memcmp(&sent, &received, sizeof(sent)) == 0);
    assert(memcmp(contents, payload, sizeof(contents)) == 0);
    assert(received_fd >= 0);
    assert(fcntl(received_fd, F_GETFD) & FD_CLOEXEC);
    free(payload);
    close(received_fd);
    close(shared_fd);

    sent.size = TORQ_PROTOCOL_MAX_PAYLOAD + 1;
    assert(torq_message_send(sockets[0], &sent, contents, -1) == -1);
    close(sockets[0]);
    assert(torq_message_receive(sockets[1], &received, &payload,
                               &received_fd) == -1);
    assert(payload == NULL && received_fd == -1);
    close(sockets[1]);
    puts("1..1\nok 1 - TORQ transport and descriptor passing");
    return 0;
}
