int torq_server_main(int argc, char **argv);
#undef CONFIG_TORQ_MPACT
#define main torq_server_main
#include "../../hw/misc/torq-server.c"
#undef main

#include <assert.h>
#include <sys/socket.h>

int main(void)
{
    TorqServer server = { 0 };
    int sockets[2];
    uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t readback[8];
    TorqMessage message = { .size = 2 * sizeof(TorqMapping), .generation = 1 };
    TorqMapping *mappings = calloc(2, sizeof(*mappings));
    void *payload = mappings;
    int shared_fd;

    assert(mappings);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server.socket_fd = sockets[0];
    server.xram = mmap(NULL, TORQ_IOVA_LIMIT, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(server.xram != MAP_FAILED);
    server.ram[0].size = 8192;
    server.ram[0].data = malloc(server.ram[0].size);
    assert(server.ram[0].data);
    memset(server.ram[0].data, 0x55, server.ram[0].size);
    server.ram_count = 1;
    mappings[0] = (TorqMapping) {
        .iova = 0x1000, .size = 4096, .permissions = 3,
    };
    mappings[1] = (TorqMapping) {
        .iova = 0x2000, .offset = 4096, .size = 4096, .permissions = 3,
    };
    assert(prepare(&server, &message, &payload) == 0);
    assert(payload == NULL);
    server.ram[0].data[100] = 0xaa;
    assert(local_memory(&server, 0x1ffc, sizeof(bytes), bytes, true) == 0);
    assert(local_memory(&server, 0x1ffc, sizeof(readback),
                         readback, false) == 0);
    assert(memcmp(bytes, readback, sizeof(bytes)) == 0);
    assert(publish(&server, 1) == 0);
    assert(torq_message_receive(sockets[1], &message,
                                 &payload, &shared_fd) == 0);
    assert(message.type == TORQ_MSG_IRQ && message.value == 1);
    assert(message.generation == 1 && message.status == 0);
    assert(memcmp(server.ram[0].data + 4092, bytes, sizeof(bytes)) == 0);
    assert(server.ram[0].data[100] == 0xaa);
    server.mappings[1].permissions = TORQ_MEM_READ;
    assert(local_memory(&server, 0x2000, sizeof(bytes),
                         bytes, true) == -EFAULT);
    assert(local_memory(&server, 0x3000, sizeof(readback), readback, false) ==
           -EFAULT);
    for (size_t index = 0; index < sizeof(readback); index++) {
        assert(readback[index] == 0);
    }
    close(sockets[0]);
    close(sockets[1]);
    free(server.ram[0].data);
    free(server.mappings);
    free(server.dirty);
    munmap(server.xram, TORQ_IOVA_LIMIT);
    puts("1..1\nok 1 - staged memory permissions and exact writeback");
    return 0;
}
