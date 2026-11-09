/*
 * TORQ device
 *
 * Copyright (c) 2026 Synaptics Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/torq.h"
#include "hw/misc/berlin_reset.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "hw/arm/smmuv3.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"
#include "system/ramblock.h"
#include "system/runstate.h"
#include "system/system.h"
#include <spawn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>

static void torq_disconnect(TORQState *s)
{
    if (s->socket_fd >= 0) {
        qemu_set_fd_handler(s->socket_fd, NULL, NULL, NULL);
        close(s->socket_fd);
        s->socket_fd = -1;
    }
    if (s->server_pid > 0) {
        pid_t waited;

        kill(s->server_pid, SIGKILL);
        do {
            waited = waitpid(s->server_pid, NULL, 0);
        } while (waited < 0 && errno == EINTR);
        s->server_pid = 0;
    }
}

static void torq_fail(TORQState *s, const char *reason)
{
    if (s->watchdog) {
        timer_del(s->watchdog);
    }
    error_report("TORQ server: %s", reason);
    torq_disconnect(s);
    s->busy = false;
    s->prepared = false;
    qemu_set_irq(s->irq, 0);
    qemu_system_vmstop_request(RUN_STATE_IO_ERROR);
}

static void torq_timeout(void *opaque)
{
    torq_fail(opaque, "job timed out");
}

static bool torq_event(TORQState *s, const TorqMessage *message)
{
    if (message->type != TORQ_MSG_IRQ || message->size ||
        message->value > 1 || message->generation > s->generation ||
        message->status < 0) {
        return false;
    }
    if (message->generation != s->generation) {
        return true;
    }
    if (s->mappings) {
        for (size_t index = 0; index < s->mappings->len; index++) {
            TorqMapping *mapping = &g_array_index(s->mappings,
                                                  TorqMapping, index);
            if (mapping->permissions & TORQ_MEM_WRITE) {
                memory_region_set_dirty(s->ram, mapping->offset, mapping->size);
            }
        }
    }
    s->busy = false;
    if (s->watchdog) {
        timer_del(s->watchdog);
    }
    qemu_set_irq(s->irq, message->value);
    return true;
}

static bool torq_receive(TORQState *s, TorqMessage *message)
{
    void *payload = NULL;
    int shared_fd = -1;
    int status = torq_message_receive(s->socket_fd, message,
                                       &payload, &shared_fd);
    free(payload);
    if (shared_fd >= 0) {
        close(shared_fd);
    }
    return !status && !message->size && shared_fd < 0;
}

static bool torq_request(TORQState *s, uint32_t type, uint64_t value,
                          const void *payload, uint32_t size, int shared_fd,
                          uint64_t *result, Error **errp)
{
    TorqMessage message = {
        .type = type,
        .id = ++s->sequence,
        .size = size,
        .value = value,
        .generation = s->generation,
    };
    uint32_t id = message.id;

    if (s->socket_fd < 0 ||
        torq_message_send(s->socket_fd, &message, payload, shared_fd)) {
        error_setg(errp, "TORQ server connection failed");
        return false;
    }
    while (torq_receive(s, &message)) {
        if (message.type == TORQ_MSG_IRQ) {
            if (!torq_event(s, &message)) {
                break;
            }
            continue;
        }
        if (message.type != TORQ_MSG_REPLY || message.id != id) {
            break;
        }
        if (message.status < 0) {
            error_setg(errp, "TORQ server rejected operation %u: %s",
                       type, strerror(-message.status));
            return false;
        }
        if (result) {
            *result = message.value;
        }
        return true;
    }
    error_setg(errp, "TORQ server disconnected or sent an invalid reply");
    return false;
}

static void torq_irq_ready(void *opaque)
{
    TORQState *s = opaque;
    TorqMessage message;
    struct pollfd descriptor = { .fd = s->socket_fd, .events = POLLIN };

    if (s->socket_fd < 0 || poll(&descriptor, 1, 0) <= 0) {
        return;
    }

    if (!torq_receive(s, &message) || !torq_event(s, &message)) {
        torq_fail(s, "disconnected or invalid asynchronous event");
    }
}

static gint torq_mapping_offset_compare(gconstpointer first,
                                         gconstpointer second)
{
    const TorqMapping *left = first;
    const TorqMapping *right = second;

    return (left->offset > right->offset) - (left->offset < right->offset);
}

static bool torq_prepare(TORQState *s, Error **errp)
{
    g_autoptr(GArray) entries = NULL;
    g_autoptr(GArray) mappings = g_array_new(false, false, sizeof(TorqMapping));
    g_autoptr(GArray) physical = g_array_new(false, false, sizeof(TorqMapping));

    struct pollfd descriptor = { .fd = s->socket_fd, .events = POLLIN };
    while (s->socket_fd >= 0 && poll(&descriptor, 1, 0) > 0) {
        torq_irq_ready(s);
    }
    if (s->busy) {
        error_setg(errp, "TORQ does not support overlapping jobs");
        return false;
    }
    entries = smmuv3_snapshot(s->dma_as, TORQ_IOVA_LIMIT, errp);
    if (!entries) {
        return false;
    }
    RCU_READ_LOCK_GUARD();
    for (size_t index = 0; index < entries->len; index++) {
        IOMMUTLBEntry *entry = &g_array_index(entries, IOMMUTLBEntry, index);
        uint64_t remaining = entry->addr_mask + 1;
        uint64_t iova = entry->iova;
        uint64_t address = entry->translated_addr;

        while (remaining) {
            MemoryRegionSection section = memory_region_find(
                get_system_memory(), address, remaining);
            if (!section.mr) {
                break;
            }
            uint64_t gap = section.offset_within_address_space - address;
            hwaddr length = int128_get64(section.size);
            hwaddr offset = section.offset_within_region;

            iova += gap;
            address += gap;
            remaining -= gap;
            if (!length) {
                memory_region_unref(section.mr);
                error_setg(errp, "TORQ snapshot contains an empty RAM extent");
                return false;
            }
            if (section.mr == s->ram) {
                TorqMapping mapping = {
                    .iova = iova,
                    .offset = offset,
                    .size = length,
                    .permissions = entry->perm,
                };
                TorqMapping *previous = mappings->len ?
                    &g_array_index(mappings, TorqMapping, mappings->len - 1) :
                    NULL;
                if (previous && previous->iova + previous->size == iova &&
                    previous->offset + previous->size == offset &&
                    previous->permissions == mapping.permissions) {
                    previous->size += length;
                } else {
                    g_array_append_val(mappings, mapping);
                }
            }
            memory_region_unref(section.mr);
            iova += length;
            address += length;
            remaining -= length;
        }
    }
    if (mappings->len > TORQ_PROTOCOL_MAX_PAYLOAD / sizeof(TorqMapping)) {
        error_setg(errp, "TORQ has too many fragmented mappings");
        return false;
    }
    g_array_append_vals(physical, mappings->data, mappings->len);
    g_array_sort(physical, torq_mapping_offset_compare);
    for (size_t index = 1; index < physical->len; index++) {
        TorqMapping *previous = &g_array_index(physical, TorqMapping,
                               index - 1);
        TorqMapping *current = &g_array_index(physical, TorqMapping, index);
        if (previous->offset + previous->size > current->offset) {
            error_setg(errp, "TORQ IOVAs 0x%"PRIx64" and 0x%"PRIx64
                       " alias RAM offset 0x%"PRIx64,
                       previous->iova, current->iova, current->offset);
            return false;
        }
    }
    s->generation++;
    if (!torq_request(s, TORQ_MSG_PREPARE, 0, mappings->data,
                       mappings->len * sizeof(TorqMapping), -1, NULL, errp)) {
        return false;
    }
    if (s->mappings) {
        g_array_unref(s->mappings);
    }
    s->mappings = g_steal_pointer(&mappings);
    s->prepared = true;
    return true;
}

static uint64_t torq_read(void *opaque, hwaddr address, unsigned int size)
{
    TORQState *s = TORQ(opaque);
    uint64_t result = 0;
    Error *error = NULL;

    for (unsigned int offset = 0; offset < size; offset += 4) {
        uint64_t value = 0;
        uint32_t register_address = address + offset;
        if ((torq_execution_read(register_address) && !s->prepared &&
             !torq_prepare(s, &error)) ||
            !torq_request(s, TORQ_MSG_READ,
                           (uint64_t)register_address << 32,
                           NULL, 0, -1, &value, &error)) {
            torq_fail(s, error_get_pretty(error));
            error_free(error);
            return UINT64_MAX;
        }
        result |= (uint64_t)(uint32_t)value << (offset * 8);
    }
    return result;
}

static void torq_write(void *opaque, hwaddr address,
                        uint64_t value, unsigned int size)
{
    TORQState *s = TORQ(opaque);
    Error *error = NULL;

    for (unsigned int offset = 0; offset < size; offset += 4) {
        uint32_t register_address = address + offset;
        uint32_t word = value >> (offset * 8);
        bool launch = torq_execution_write(register_address, word);
        if (launch && !torq_prepare(s, &error)) {
            goto fail;
        }
        if (launch) {
            s->busy = true;
            timer_mod(s->watchdog,
                        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + s->timeout_ms);
        }
        if (!torq_request(s, TORQ_MSG_WRITE,
                           ((uint64_t)register_address << 32) | word,
                           NULL, 0, -1, NULL, &error)) {
            goto fail;
        }
    }
    return;

fail:
    torq_fail(s, error_get_pretty(error));
    error_free(error);
}

static const MemoryRegionOps torq_ops = {
    .read = torq_read,
    .write = torq_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void torq_init(Object *obj)
{
    TORQState *s = TORQ(obj);

    memory_region_init_io(&s->mmio, obj, &torq_ops, s, TYPE_TORQ, TORQ_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->mmio_base = 0;
    s->dma_as = &address_space_memory;

    s->socket_fd = -1;
}

void torq_set_dma_as(DeviceState *dev, AddressSpace *as)
{
    TORQState *s = TORQ(dev);

    if (as) {
        s->dma_as = as;
    }
}

static void torq_iommu_notify(IOMMUNotifier *notifier, IOMMUTLBEntry *entry)
{
    TORQState *s = container_of(notifier, TORQState, iommu_notifier);

    if (!s->prepared) {
        return;
    }
    if (s->busy) {
        torq_fail(s, "IOMMU invalidation during a job is unsupported");
    } else {
        Error *error = NULL;
        if (!torq_request(s, TORQ_MSG_INVALIDATE, 0, NULL, 0, -1,
                    NULL, &error)) {
            torq_fail(s, error_get_pretty(error));
            error_free(error);
        }
    }
    s->prepared = false;
}

static void torq_exit(Notifier *notifier, void *data)
{
    TORQState *s = container_of(notifier, TORQState, exit_notifier);

    torq_disconnect(s);
}

static void torq_unrealize(DeviceState *dev)
{
    TORQState *s = TORQ(dev);

    torq_disconnect(s);
    if (s->watchdog) {
        timer_free(s->watchdog);
        s->watchdog = NULL;
    }
    if (s->notifier_registered) {
        memory_region_unregister_iommu_notifier(s->dma_as->root,
                                                 &s->iommu_notifier);
        s->notifier_registered = false;
    }
    if (s->exit_registered) {
        qemu_remove_exit_notifier(&s->exit_notifier);
        s->exit_registered = false;
    }
    migrate_del_blocker(&s->migration_blocker);
    if (s->mappings) {
        g_array_unref(s->mappings);
        s->mappings = NULL;
    }
}

static void torq_realize(DeviceState *dev, Error **errp)
{
    TORQState *s = TORQ(dev);
    posix_spawn_file_actions_t actions;
    int sockets[2];
    int status;
    TorqMessage hello;
    g_autofree char *default_path = NULL;
    g_autofree char *executable = NULL;
    g_autofree char *directory = NULL;
    struct timeval timeout = {
        .tv_sec = s->timeout_ms / 1000,
        .tv_usec = (s->timeout_ms % 1000) * 1000,
    };

    if (!s->ram || !memory_region_is_ram(s->ram) ||
        memory_region_get_fd(s->ram) < 0 ||
        !qemu_ram_is_shared(s->ram->ram_block)) {
        error_setg(errp,
                "TORQ requires memory-backend-memfd,share=on guest RAM");
        return;
    }
    if (!s->timeout_ms ||
        !object_dynamic_cast(OBJECT(s->dma_as->root),
                              TYPE_SMMUV3_IOMMU_MEMORY_REGION)) {
        error_setg(errp, "TORQ requires SMMUv3 DMA and a nonzero timeout-ms");
        return;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        error_setg_errno(errp, errno, "TORQ socketpair failed");
        return;
    }
    s->socket_fd = sockets[0];
    setsockopt(s->socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout));
    setsockopt(s->socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                &timeout, sizeof(timeout));
    executable = g_file_read_link("/proc/self/exe", NULL);
    if (!executable) {
        error_setg_errno(errp, errno, "Cannot locate the QEMU executable");
        goto fail;
    }
    directory = g_path_get_dirname(executable);
    default_path = g_build_filename(directory, "torq-cmodel-server", NULL);
    if (!s->server_path && access(default_path, X_OK)) {
        g_free(default_path);
        default_path = g_build_filename(directory, "hw", "misc",
                                         "torq-cmodel-server", NULL);
    }
    const char *path = s->server_path ? s->server_path : default_path;
    char fd_option[] = "--fd";
    char fd_number[] = "3";
    char backend_option[] = "--backend";
    char default_backend[] = "cmodel";
    char *arguments[] = { (char *)path, fd_option, fd_number, backend_option,
                          s->backend ? s->backend : default_backend, NULL };
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    status = posix_spawn(&s->server_pid, path, &actions, NULL,
                          arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (status) {
        error_setg_errno(errp, status, "Cannot spawn TORQ server %s", path);
        goto fail;
    }
    if (!torq_receive(s, &hello) || hello.type != TORQ_MSG_HELLO ||
        hello.value != TORQ_PROTOCOL_VERSION) {
        error_setg(errp, "TORQ server startup or protocol negotiation failed");
        goto fail;
    }
    TorqRamRegion region = {
        .size = memory_region_size(s->ram),
        .fd_offset = qemu_ram_get_fd_offset(s->ram->ram_block),
    };
    if (!torq_request(s, TORQ_MSG_RAM, 0, &region, sizeof(region),
                       memory_region_get_fd(s->ram), NULL, errp)) {
        goto fail;
    }
    iommu_notifier_init(&s->iommu_notifier, torq_iommu_notify,
                        IOMMU_NOTIFIER_UNMAP, 0, TORQ_IOVA_LIMIT - 1, 0);
    if (memory_region_register_iommu_notifier(s->dma_as->root,
                                              &s->iommu_notifier, errp)) {
        goto fail;
    }
    s->notifier_registered = true;
    error_setg(&s->migration_blocker,
                "TORQ external server state cannot be migrated");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        goto fail;
    }
    s->exit_notifier.notify = torq_exit;
    qemu_add_exit_notifier(&s->exit_notifier);
    s->exit_registered = true;
    s->watchdog = timer_new_ms(QEMU_CLOCK_REALTIME, torq_timeout, s);
    qemu_set_fd_handler(s->socket_fd, torq_irq_ready, NULL, s);
    return;

fail:
    torq_unrealize(dev);
}

static const Property torq_properties[] = {
    DEFINE_PROP_STRING("server", TORQState, server_path),
    DEFINE_PROP_STRING("backend", TORQState, backend),
    DEFINE_PROP_UINT32("timeout-ms", TORQState, timeout_ms, 300000),
};

static void torq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->realize = torq_realize;
    device_class->unrealize = torq_unrealize;
    device_class_set_props(device_class, torq_properties);
}

static const TypeInfo torq_info = {
    .name = TYPE_TORQ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TORQState),
    .instance_init = torq_init,
    .class_init = torq_class_init,
};

static void torq_register_types(void)
{
    type_register_static(&torq_info);
}

type_init(torq_register_types)

/* Reset-line registration is done by the board (see torq_core_rst and
 * torq_reset_line_cb in hw/misc/torq.h). */
const BerlinResetSpec torq_core_rst = {
    .offset = 0x378,
    .bit = 1,
    .inverted = false,
};

/*
 * The guest asserts the NPU core reset by clearing the (active-low) bit,
 * after which the kernel deasserts it and reprograms the NPU from scratch,
 * so resetting the C model on the assert edge matches the hardware.
 * @opaque must be a TORQState.
 */
void torq_reset_line_cb(void *opaque, bool asserted)
{
    TORQState *s = TORQ(opaque);

    if (asserted) {
        Error *error = NULL;

        if (!torq_request(s, TORQ_MSG_RESET, 0, NULL, 0, -1, NULL, &error)) {
            torq_fail(s, error_get_pretty(error));
            error_free(error);
        }
        s->prepared = false;
        s->busy = false;
        qemu_set_irq(s->irq, 0);
    }
}

DeviceState *torq_create(hwaddr addr, qemu_irq irq, AddressSpace *dma_as,
                          MemoryRegion *ram)
{
    SysBusDevice *sbd;
    DeviceState *dev;
    TORQState *s;

    dev = qdev_new(TYPE_TORQ);
    s = TORQ(dev);
    s->mmio_base = addr;
    s->dma_as = dma_as;
    s->ram = ram;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    return dev;
}
