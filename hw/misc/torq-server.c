// Copyright 2026 Synaptics Inc.
//
// Licensed under the Apache License v2.0
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hw/misc/torq-protocol.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include "cm/torq_cm.h"
#ifdef CONFIG_TORQ_AWS_FPGA
#include "hw/misc/torq-fpga.h"
#endif

#ifdef CONFIG_TORQ_MPACT
#include "hw/misc/torq_mpact.h"
#endif

typedef struct ServerRam {
    uint8_t *data;
    size_t size;
} ServerRam;

typedef struct DirtySpan {
    size_t mapping;
    size_t address;
    size_t size;
} DirtySpan;

typedef struct TorqServer {
    int socket_fd;
    void *model;
#ifdef CONFIG_TORQ_AWS_FPGA
    TorqFpga *fpga;
#endif
    uint8_t *xram;
    ServerRam ram[TORQ_PROTOCOL_MAX_REGIONS];
    size_t ram_count;
    TorqMapping *mappings;
    DirtySpan *dirty;
    size_t dirty_count;
    size_t dirty_capacity;
    size_t mapping_count;
    uint64_t generation;
    bool prepared;
    bool progress;
    bool memory_error;
    bool results_pending;
    uint32_t irq;
} TorqServer;

static int cmodel_consume_start(TorqServer *server)
{
    uint32_t start;
    int status = torq_cm_read32(server->model, TORQ_NSS_START, &start);

    if (status < 0 || !(start & 1)) {
        return status;
    }
    start &= ~1U;
    return torq_cm_write32(server->model, TORQ_NSS_START, &start);
}

static int backend_read(TorqServer *server, uint32_t address, uint32_t *value)
{
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server->fpga) {
        return torq_fpga_read32(server->fpga, address, value);
    }
#endif
    int status = torq_cm_read32(server->model, address, value);

    if (status >= 0 && address == TORQ_NSS_STATUS) {
        status = cmodel_consume_start(server);
    }
    return status;
}

static int backend_write(TorqServer *server, uint32_t address, uint32_t value)
{
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server->fpga) {
        return torq_fpga_write32(server->fpga, address, value);
    }
#endif
    return torq_cm_write32(server->model, address, &value);
}

static int backend_reset(TorqServer *server)
{
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server->fpga) {
        return torq_fpga_reset(server->fpga);
    }
#endif
    return torq_cm__reset(server->model);
}

static int local_memory(TorqServer *server, size_t address, size_t size,
                        uint8_t *data, bool writing)
{
    uint32_t permission = writing ? TORQ_MEM_WRITE : TORQ_MEM_READ;

    if (address >= TORQ_IOVA_LIMIT || size > TORQ_IOVA_LIMIT - address) {
        goto fail;
    }
    while (size) {
        size_t index;

        for (index = 0; index < server->mapping_count; index++) {
            TorqMapping *mapping = &server->mappings[index];
            if (address >= mapping->iova &&
                address - mapping->iova < mapping->size) {
                break;
            }
        }
        if (index == server->mapping_count) {
            goto fail;
        }
        TorqMapping *mapping = &server->mappings[index];
        if (!(mapping->permissions & permission)) {
            goto fail;
        }
        size_t count = mapping->size - (address - mapping->iova);
        if (count > size) {
            count = size;
        }
        if (writing) {
            DirtySpan *previous = server->dirty_count ?
                &server->dirty[server->dirty_count - 1] : NULL;
            if (previous && previous->mapping == index &&
                previous->address + previous->size == address) {
                previous->size += count;
            } else {
                if (server->dirty_count == server->dirty_capacity) {
                    size_t capacity = server->dirty_capacity ?
                        server->dirty_capacity * 2 : 64;
                    DirtySpan *spans = realloc(server->dirty,
                                               capacity * sizeof(*spans));
                    if (!spans) {
                        goto fail;
                    }
                    server->dirty = spans;
                    server->dirty_capacity = capacity;
                }
                server->dirty[server->dirty_count++] = (DirtySpan) {
                    .mapping = index,
                    .address = address,
                    .size = count,
                };
            }
            memcpy(server->xram + address, data, count);
        } else {
            memcpy(data, server->xram + address, count);
        }
        size -= count;
        address += count;
        data += count;
    }
    return 0;

fail:
    server->memory_error = true;
    fprintf(stderr, "TORQ: invalid %s at IOVA 0x%zx, size 0x%zx\n",
            writing ? "write" : "read", address, size);
    if (!writing) {
        memset(data, 0, size);
    }
    return -EFAULT;
}

static int read_memory(void *context, uint32_t tag, size_t address,
                       size_t size, uint8_t *data)
{
    (void)tag;
    return local_memory(context, address, size, data, false);
}

static int write_memory(void *context, uint32_t tag, size_t address,
                        size_t size, uint8_t *data)
{
    (void)tag;
    return local_memory(context, address, size, data, true);
}

static int register_ram(TorqServer *server, const TorqMessage *message,
                         const void *payload, int shared_fd)
{
    const TorqRamRegion *region = payload;
    struct stat status;
    void *data;

    if (server->prepared || message->size != sizeof(*region) ||
        shared_fd < 0 || message->value != server->ram_count ||
        server->ram_count == TORQ_PROTOCOL_MAX_REGIONS ||
        fstat(shared_fd, &status) || !region->size ||
        region->fd_offset > (uint64_t)status.st_size ||
        region->size > (uint64_t)status.st_size - region->fd_offset ||
        region->fd_offset % sysconf(_SC_PAGESIZE)) {
        return -EINVAL;
    }
    data = mmap(NULL, region->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                shared_fd, region->fd_offset);
    if (data == MAP_FAILED) {
        return -errno;
    }
    server->ram[server->ram_count++] = (ServerRam) {
        .data = data,
        .size = region->size,
    };
    return 0;
}

static int prepare(TorqServer *server, const TorqMessage *message,
                    void **payload)
{
    TorqMapping *mappings = *payload;
    size_t count = message->size / sizeof(*mappings);

    if (message->size % sizeof(*mappings) ||
        message->generation <= server->generation) {
        return -EINVAL;
    }
    for (size_t index = 0; index < count; index++) {
        TorqMapping *mapping = &mappings[index];
        if (mapping->region >= server->ram_count || !mapping->size ||
            mapping->iova >= TORQ_IOVA_LIMIT ||
            mapping->size > TORQ_IOVA_LIMIT - mapping->iova ||
            mapping->offset > server->ram[mapping->region].size ||
            mapping->size > server->ram[mapping->region].size -
                            mapping->offset ||
            !mapping->permissions ||
            (mapping->permissions & ~(TORQ_MEM_READ | TORQ_MEM_WRITE)) ||
            (index && mappings[index - 1].iova + mappings[index - 1].size >
                      mapping->iova)) {
            return -EINVAL;
        }
    }
    free(server->mappings);
    server->mappings = mappings;
    *payload = NULL;
    server->mapping_count = count;
    server->dirty_count = 0;
    for (size_t index = 0; index < count; index++) {
        TorqMapping *mapping = &mappings[index];
        memcpy(server->xram + mapping->iova,
               server->ram[mapping->region].data + mapping->offset,
               mapping->size);
#ifdef CONFIG_TORQ_AWS_FPGA
        if (server->fpga &&
            torq_fpga_memory(server->fpga, mapping->iova, mapping->size,
                             server->xram + mapping->iova, true)) {
            return -EIO;
        }
#endif
    }
    server->generation = message->generation;
    server->prepared = true;
    server->memory_error = false;
    return 0;
}

static int publish(TorqServer *server, uint32_t irq)
{
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server->fpga && server->results_pending &&
        !torq_fpga_busy(server->fpga)) {
        for (size_t index = 0; index < server->mapping_count; index++) {
            TorqMapping *mapping = &server->mappings[index];
            if ((mapping->permissions & TORQ_MEM_WRITE) &&
                torq_fpga_memory(server->fpga, mapping->iova, mapping->size,
                                 server->ram[mapping->region].data +
                                 mapping->offset, false)) {
                return -1;
            }
        }
        server->results_pending = false;
    }
#endif
    for (size_t index = 0; index < server->dirty_count; index++) {
        DirtySpan *span = &server->dirty[index];
        TorqMapping *mapping = &server->mappings[span->mapping];
        memcpy(server->ram[mapping->region].data + mapping->offset +
               span->address - mapping->iova,
               server->xram + span->address, span->size);
    }
    server->dirty_count = 0;
    TorqMessage event = {
        .type = TORQ_MSG_IRQ,
        .value = irq,
        .generation = server->generation,
        .status = server->memory_error ? -EFAULT : 0,
    };
    server->irq = irq;
    return torq_message_send(server->socket_fd, &event, NULL, -1);
}

static int advance(TorqServer *server)
{
    uint32_t irq = 0;

    server->progress = false;
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server->fpga) {
        if (torq_fpga_get_irq(server->fpga, &irq)) {
            return -1;
        }
        server->progress = torq_fpga_busy(server->fpga);
        if (server->progress) {
            return 0;
        }
        return publish(server, irq);
    }
#endif
    uint32_t control, enabled;

    if (torq_cm_read32(server->model, TORQ_NSS_CONTROL, &control) < 0 ||
        torq_cm_read32(server->model, TORQ_HOST_IEN, &enabled) < 0 ||
        torq_cm__get_irq(server->model, 0, 1, &irq) < 0) {
        return -1;
    }
    if ((control & enabled & 0xff) && cmodel_consume_start(server) < 0) {
        return -1;
    }
    return publish(server, irq);
}

static int process(TorqServer *server, TorqMessage *message, void **payload,
                    int shared_fd)
{
    uint32_t address = message->value >> 32;
    uint32_t value = message->value;
    TorqMessage reply = { .type = TORQ_MSG_REPLY, .id = message->id };

    if ((message->type != TORQ_MSG_RAM && shared_fd >= 0) ||
        (message->type != TORQ_MSG_RAM &&
         message->type != TORQ_MSG_PREPARE && message->size)) {
        return -1;
    }
    switch (message->type) {
    case TORQ_MSG_RAM:
        reply.status = register_ram(server, message, *payload, shared_fd);
        break;
    case TORQ_MSG_PREPARE:
        reply.status = prepare(server, message, payload);
        break;
    case TORQ_MSG_READ:
        if (address >= 0x200000 || (address & 3) ||
            (torq_execution_read(address) && !server->prepared)) {
            reply.status = -EINVAL;
            break;
        }
        reply.status = backend_read(server, address, &value);
        reply.value = value;
        if (torq_execution_read(address)) {
            if (advance(server)) {
                return -1;
            }
        }
        break;
    case TORQ_MSG_WRITE:
        if (address >= 0x200000 || (address & 3) ||
            (torq_execution_write(address, value) && !server->prepared)) {
            reply.status = -EINVAL;
            break;
        }
        reply.status = backend_write(server, address, value);
        if (!reply.status && torq_execution_write(address, value)) {
            server->results_pending = true;
        }
        server->progress = server->prepared;
        /* Lower the IRQ before the reply, else the guest re-enters its handler on a stale level. */
        if (!reply.status && address == TORQ_NSS_STATUS && server->prepared &&
            advance(server)) {
            return -1;
        }
        /* Run the C model to completion before replying so the guest's timeout starts after the job. */
        if (!reply.status && server->model && torq_execution_write(address, value) &&
            advance(server)) {
            return -1;
        }
        break;
    case TORQ_MSG_RESET:
        reply.status = backend_reset(server);
        server->prepared = false;
        server->progress = false;
        server->memory_error = false;
        if (publish(server, 0)) {
            return -1;
        }
        break;
    case TORQ_MSG_INVALIDATE:
        server->prepared = false;
        server->progress = false;
        break;
    case TORQ_MSG_STOP:
        torq_message_send(server->socket_fd, &reply, NULL, -1);
        return 1;
    default:
        reply.status = -ENOTSUP;
        break;
    }
    if (server->memory_error) {
        reply.status = -EFAULT;
    }
    reply.generation = server->generation;
    return torq_message_send(server->socket_fd, &reply, NULL, -1);
}

int main(int argc, char **argv)
{
    TorqServer server = { 0 };
    TorqMessage message;
    void *payload;
    int shared_fd;
    int result = EXIT_FAILURE;
    char *end;

    if ((argc != 3 && argc != 5) || strcmp(argv[1], "--fd") ||
        (argc == 5 && strcmp(argv[3], "--backend"))) {
        fprintf(stderr,
            "Usage: %s --fd SOCKET_FD [--backend cmodel|aws-fpga]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    errno = 0;
    long socket_fd = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || socket_fd < 0 ||
        socket_fd > INT32_MAX) {
        return EXIT_FAILURE;
    }
    server.socket_fd = socket_fd;
    signal(SIGPIPE, SIG_IGN);
    pid_t parent = getppid();
#ifdef __linux__
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) {
        return EXIT_FAILURE;
    }
#endif
    server.xram = mmap(NULL, TORQ_IOVA_LIMIT, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (server.xram == MAP_FAILED) {
        perror("TORQ XRAM allocation");
        return EXIT_FAILURE;
    }
    if (argc == 3 || !strcmp(argv[4], "cmodel")) {
        server.model = torq_cm_open(server.xram, 0, TORQ_IOVA_LIMIT);
        if (!server.model ||
            torq_cm__set_ext_mem_if(server.model, &server,
                                   read_memory, write_memory) < 0) {
            goto out;
        }
#ifdef CONFIG_TORQ_MPACT
        torq_cm__set_css_cpu_code(server.model, torq_mpact_run_cpu);
#endif
    } else {
#ifdef CONFIG_TORQ_AWS_FPGA
        if (!strcmp(argv[4], "aws-fpga")) {
            server.fpga = torq_fpga_open();
        }
        if (!server.fpga) {
            fprintf(stderr, "TORQ: cannot open AWS FPGA backend\n");
            goto out;
        }
#else
        fprintf(stderr,
            "TORQ: this server was built without AWS FPGA support\n");
        goto out;
#endif
    }
    message = (TorqMessage) {
        .type = TORQ_MSG_HELLO,
        .value = TORQ_PROTOCOL_VERSION,
    };
    if (torq_message_send(server.socket_fd, &message, NULL, -1)) {
        goto out;
    }
    for (;;) {
        if (server.progress) {
            if (advance(&server)) {
                break;
            }
        }
        struct pollfd descriptor = { .fd = server.socket_fd, .events = POLLIN };
        int ready = poll(&descriptor, 1, server.progress ? 1 : 1000);
        if (getppid() != parent) {
            break;
        }
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (!ready) {
            continue;
        }
        if (ready < 0) {
            break;
        }
        if (torq_message_receive(server.socket_fd, &message, &payload,
                                  &shared_fd)) {
            break;
        }
        int status = process(&server, &message, &payload, shared_fd);
        free(payload);
        if (shared_fd >= 0) {
            close(shared_fd);
        }
        if (status) {
            if (status > 0) {
                result = EXIT_SUCCESS;
            }
            break;
        }
    }

out:
    if (server.model) {
        torq_cm_close(server.model);
    }
#ifdef CONFIG_TORQ_AWS_FPGA
    if (server.fpga) {
        torq_fpga_close(server.fpga);
    }
#endif
    for (size_t index = 0; index < server.ram_count; index++) {
        munmap(server.ram[index].data, server.ram[index].size);
    }
    free(server.mappings);
    free(server.dirty);
    munmap(server.xram, TORQ_IOVA_LIMIT);
    close(server.socket_fd);
    return result;
}
