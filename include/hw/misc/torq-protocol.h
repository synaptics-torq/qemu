#ifndef HW_MISC_TORQ_PROTOCOL_H
#define HW_MISC_TORQ_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/*
 * TORQ proxy protocol: QEMU owns guest RAM, SMMU translation and the guest
 * IRQ line; the server owns registers, execution and backend-local XRAM
 * (C model or FPGA). Control uses an inherited Unix SOCK_STREAM socketpair.
 * Guest data moves through shared RAM, not through register-message payloads.
 *
 * High-level flow:
 *
 *   server -> QEMU: HELLO (value = protocol version)
 *   QEMU -> server: RAM + TorqRamRegion + backing FD via SCM_RIGHTS
 *   server -> QEMU: REPLY
 *   QEMU <-> server: ordinary register WRITE/READ requests and REPLY messages
 *   QEMU -> server: PREPARE + TorqMapping[] for a new mapping generation
 *   server: upload shared guest RAM into local XRAM at the supplied IOVAs
 *   server -> QEMU: REPLY (upload complete)
 *   QEMU -> server: execution-triggering WRITE or READ
 *   server: execute/progress, then copy results back into shared guest RAM
 *   server -> QEMU: IRQ (value = current level, 0 or 1)
 *   QEMU: mark guest RAM dirty, finish the job and update the guest IRQ line
 *
 * PREPARE precedes a launch write, or an execution-triggering status read
 * without a valid snapshot. Polling an existing job does not re-upload its
 * inputs. Mappings and job buffers must remain stable until completion.
 * Ordinary C-model writes update registers; subsequent server IRQ sampling
 * can execute pending work. Status reads can also execute work. Thus IRQ
 * delivery does not require the guest to poll. The FPGA starts on its launch
 * write, after upload, and the server polls its completion/IRQ sources.
 *
 * Each request receives a REPLY with the same id; status is zero on success
 * or a negative errno on failure. IRQ is unsolicited and may precede a REPLY,
 * so a waiting client must dispatch events until its matching reply arrives.
 * Results are published before an IRQ event or an execution-read reply.
 * Currently IRQ also implies completed execution, even for level zero; it
 * is not an intermediate progress notification. IRQ deassertion uses level 0.
 *
 * PREPARE generations increase monotonically. IRQ carries its generation;
 * QEMU ignores older events and rejects future ones. INVALIDATE discards
 * snapshot validity after an idle IOMMU invalidation; a fresh PREPARE is then
 * required before execution. Active-job invalidation is unsupported. RESET
 * resets the backend, invalidates preparation and publishes IRQ level zero.
 * STOP requests an orderly reply and server exit.
 *
 * Every frame is a TorqMessage followed by size payload bytes. RAM carries
 * one TorqRamRegion and one FD; value selects the RAM region index. PREPARE
 * carries an array of TorqMapping entries referencing those regions. READ
 * and WRITE encode the register offset in value[63:32]; WRITE data and READ
 * reply data use value[31:0]. QEMU splits 64-bit MMIO into two ordered 32-bit
 * requests. Other messages have no payload or FD.
 *
 * Version 1 uses the fixed-size native-endian structures below. Both peers
 * must share the same ABI/endianness; this is a trusted same-host transport.
 */

#define TORQ_PROTOCOL_VERSION 1
#define TORQ_PROTOCOL_MAX_PAYLOAD (32U * 1024U * 1024U)
#define TORQ_PROTOCOL_MAX_REGIONS 64
#define TORQ_IOVA_LIMIT (UINT64_C(1) << 32)

#define TORQ_NSS_STATUS 0x001f000c
#define TORQ_NSS_CONTROL 0x001f0008
#define TORQ_NSS_START 0x001f0010
#define TORQ_HOST_IEN 0x001fc004
#define TORQ_CPU_CONTROL 0x001fd000
#define TORQ_CDMA_STATUS 0x001ff004
#define TORQ_CDMA_COMMAND 0x001ff000
#define TORQ_CDMA_NSEC_STATUS 0x001fe208

enum TorqMessageType {
    TORQ_MSG_HELLO = 1,
    TORQ_MSG_RAM,
    TORQ_MSG_PREPARE,
    TORQ_MSG_READ,
    TORQ_MSG_WRITE,
    TORQ_MSG_RESET,
    TORQ_MSG_STOP,
    TORQ_MSG_INVALIDATE,
    TORQ_MSG_REPLY = 0x100,
    TORQ_MSG_IRQ,
};

enum TorqMemoryPermission {
    TORQ_MEM_READ = 1,
    TORQ_MEM_WRITE = 2,
};

typedef struct TorqMessage {
    uint32_t type;
    uint32_t id;
    uint32_t size;
    int32_t status;
    uint64_t value;
    uint64_t generation;
} TorqMessage;

typedef struct TorqRamRegion {
    uint64_t size;
    uint64_t fd_offset;
} TorqRamRegion;

typedef struct TorqMapping {
    uint64_t iova;
    uint64_t offset;
    uint64_t size;
    uint32_t region;
    uint32_t permissions;
} TorqMapping;

_Static_assert(sizeof(TorqMessage) == 32, "TORQ message ABI size");
_Static_assert(sizeof(TorqRamRegion) == 16, "TORQ RAM ABI size");
_Static_assert(sizeof(TorqMapping) == 32, "TORQ mapping ABI size");

int torq_message_send(int socket_fd, const TorqMessage *message,
                      const void *payload, int shared_fd);
int torq_message_receive(int socket_fd, TorqMessage *message,
                         void **payload, int *shared_fd);

static inline int torq_execution_read(uint32_t address)
{
    return address == TORQ_NSS_STATUS || address == TORQ_CDMA_STATUS ||
           address == TORQ_CDMA_NSEC_STATUS;
}

static inline int torq_execution_write(uint32_t address, uint32_t value)
{
    return (address == TORQ_NSS_START && value) ||
           (address == TORQ_CPU_CONTROL && !value) ||
           (address == TORQ_CDMA_COMMAND && (value & 1));
}

#endif
