// Copyright 2026 Synaptics Inc.
//
// SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0


#include "hw/misc/torq-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

static int transfer(int socket_fd, void *buffer, size_t size, int sending)
{
    uint8_t *cursor = buffer;

    while (size) {
        ssize_t count = sending ? send(socket_fd, cursor, size, MSG_NOSIGNAL) :
                                 recv(socket_fd, cursor, size, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += count;
        size -= count;
    }
    return 0;
}

int torq_message_send(int socket_fd, const TorqMessage *message,
                      const void *payload, int shared_fd)
{
    struct iovec vector = { .iov_base = (void *)message, .iov_len = 1 };
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = { 0 };
    struct msghdr header = { .msg_iov = &vector, .msg_iovlen = 1 };
    ssize_t count;

    if (message->size > TORQ_PROTOCOL_MAX_PAYLOAD ||
        (message->size && !payload)) {
        errno = EINVAL;
        return -1;
    }
    if (shared_fd >= 0) {
        header.msg_control = control.bytes;
        header.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *descriptor = CMSG_FIRSTHDR(&header);
        descriptor->cmsg_level = SOL_SOCKET;
        descriptor->cmsg_type = SCM_RIGHTS;
        descriptor->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(descriptor), &shared_fd, sizeof(int));
    }
    do {
        count = sendmsg(socket_fd, &header, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count != 1 ||
        transfer(socket_fd, (uint8_t *)message + 1, sizeof(*message) - 1, 1)) {
        return -1;
    }
    return transfer(socket_fd, (void *)payload, message->size, 1);
}

int torq_message_receive(int socket_fd, TorqMessage *message,
                         void **payload, int *shared_fd)
{
    struct iovec vector = { .iov_base = message, .iov_len = 1 };
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = { 0 };
    struct msghdr header = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    ssize_t count;
    int malformed = 0;

    *payload = NULL;
    *shared_fd = -1;
    do {
        count = recvmsg(socket_fd, &header, MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    for (struct cmsghdr *descriptor = CMSG_FIRSTHDR(&header); descriptor;
         descriptor = CMSG_NXTHDR(&header, descriptor)) {
        if (descriptor->cmsg_level == SOL_SOCKET &&
            descriptor->cmsg_type == SCM_RIGHTS &&
            descriptor->cmsg_len >= CMSG_LEN(sizeof(int))) {
            size_t descriptors = (descriptor->cmsg_len - CMSG_LEN(0)) /
                                 sizeof(int);
            for (size_t index = 0; index < descriptors; index++) {
                int received_fd;
                memcpy(&received_fd, (uint8_t *)CMSG_DATA(descriptor) +
                       index * sizeof(int), sizeof(received_fd));
                if (*shared_fd < 0) {
                    *shared_fd = received_fd;
                    if (fcntl(received_fd, F_SETFD, FD_CLOEXEC) < 0) {
                        malformed = 1;
                    }
                } else {
                    close(received_fd);
                    malformed = 1;
                }
            }
        }
    }
    if (count != 1 || malformed || (header.msg_flags & MSG_CTRUNC) ||
        transfer(socket_fd, (uint8_t *)message + 1, sizeof(*message) - 1, 0) ||
        message->size > TORQ_PROTOCOL_MAX_PAYLOAD) {
        goto fail;
    }
    if (message->size) {
        *payload = malloc(message->size);
        if (!*payload || transfer(socket_fd, *payload, message->size, 0)) {
            goto fail;
        }
    }
    return 0;

fail:
    free(*payload);
    *payload = NULL;
    if (*shared_fd >= 0) {
        close(*shared_fd);
        *shared_fd = -1;
    }
    return -1;
}
