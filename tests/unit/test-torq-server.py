#!/usr/bin/env python3

import array
import mmap
import os
import socket
import struct
import subprocess
import sys
import unittest


SERVER = sys.argv.pop(1)
HEADER = struct.Struct("<IIIiQQ")
MAPPING = struct.Struct("<QQQII")


class TorqServerTest(unittest.TestCase):
    def setUp(self):
        self.socket, child_socket = socket.socketpair()
        self.socket.settimeout(10)
        self.child = subprocess.Popen(
            [SERVER, "--fd", str(child_socket.fileno()), "--backend",
             os.environ.get("TORQ_TEST_BACKEND", "cmodel")],
            pass_fds=(child_socket.fileno(),),
        )
        child_socket.close()
        self.events = []
        self.sequence = 0
        self.assertEqual(self.receive()[0][:2], (1, 0))
        self.fd = os.memfd_create("torq-test")
        os.ftruncate(self.fd, 8192)
        self.memory = mmap.mmap(self.fd, 8192)
        self.request(2, payload=struct.pack("<QQ", 8192, 0), fd=self.fd)

    def tearDown(self):
        self.socket.close()
        try:
            self.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait()
        self.memory.close()
        os.close(self.fd)

    def receive(self):
        contents = bytearray()
        while len(contents) < HEADER.size:
            block = self.socket.recv(HEADER.size - len(contents))
            if not block:
                self.fail("server disconnected")
            contents.extend(block)
        header = HEADER.unpack(contents)
        self.assertEqual(header[2], 0)
        return header, b""

    def request(self, kind, value=0, payload=b"", generation=0, fd=None,
                status=0):
        self.sequence += 1
        header = HEADER.pack(kind, self.sequence, len(payload), 0,
                             value, generation)
        descriptors = [] if fd is None else [
            (socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))
        ]
        self.assertEqual(self.socket.sendmsg([header[:1]], descriptors), 1)
        self.socket.sendall(header[1:] + payload)
        while True:
            reply, _ = self.receive()
            if reply[0] == 0x101:
                self.events.append(reply)
                continue
            self.assertEqual(reply[0:2], (0x100, self.sequence))
            self.assertEqual(reply[3], status)
            return reply[4]

    def write(self, address, value):
        return self.request(5, (address << 32) | value)

    def read(self, address):
        return self.request(4, address << 32)

    def test_registers_and_reset(self):
        self.write(0, 0x12345678)
        self.assertEqual(self.read(0), 0x12345678)
        self.request(6)
        self.assertEqual(self.read(0x1FD000), 3)
        self.request(7)
        self.assertEqual(self.child.wait(timeout=5), 0)

    def test_mapping_validation(self):
        self.request(3, payload=MAPPING.pack(0x1000, 0, 4096, 0, 3),
                     generation=1)
        self.request(3, payload=MAPPING.pack(0x1000, 8192, 4096, 0, 3),
                     generation=2, status=-22)
        self.request(3, payload=MAPPING.pack(0x1000, 0, 4096, 0, 3),
                     generation=1, status=-22)
        self.request(7)

    def run_nss(self, interrupts):
        self.request(3, generation=1)
        self.write(0, 0x90010100)
        self.write(4, 0)
        self.write(0x1F0004, 1 << 28)
        self.write(0x1F0008, int(interrupts))
        self.write(0x1FC004, int(interrupts))
        self.write(0x1F0010, 1)
        if interrupts:
            while not any(event[4] for event in self.events):
                event, _ = self.receive()
                self.assertEqual(event[0], 0x101)
                self.events.append(event)
            self.assertEqual(self.read(0x1F0010) & 1, 0)
        else:
            self.assertTrue(self.read(0x1F0010) & 1)
        self.assertTrue(self.read(0x1F000C) & 1)
        self.assertEqual(self.read(0x1F0010) & 1, 0)
        self.write(0x1F000C, 1)
        self.write(0, 0xDEADBEEF)
        self.assertEqual(self.read(0), 0xDEADBEEF)
        self.assertEqual(self.read(0x1F000C) & 1, 0)
        self.request(7)

    def test_nss_start_consumed_after_irq(self):
        self.run_nss(interrupts=True)

    def test_nss_start_consumed_after_status_read(self):
        self.run_nss(interrupts=False)

    def test_nss_dma_and_async_irq(self):
        expected = bytes((index * 8 + 1) & 255 for index in range(64))
        self.memory[:len(expected)] = expected
        self.request(3, payload=MAPPING.pack(0x40010000, 0, 4096, 0, 3),
                     generation=1)
        program = (
            0x100C0100, 0x00020011, 0, 64, 0x100, 0x10000040,
            0x40010000, 1, 0, 0, 0, 0, 0,
            0x10030008, 1, 4, 4, 0xE0000004,
        )
        for index, value in enumerate(program):
            self.write(index * 4, value)
        self.write(0x1F000C, 0xFFFFFFFF)
        self.write(0x1F0004, 1 << 28)
        self.write(0x1F0008, 1)
        self.write(0x1FC004, 1)
        self.write(0x1F0010, 1)
        while not any(event[4] for event in self.events):
            event, _ = self.receive()
            self.assertEqual(event[0], 0x101)
            self.events.append(event)
        self.assertEqual(self.read(0x1F000C) & 5, 5)
        actual = b"".join(self.read(0x100 + offset).to_bytes(4, "little")
                          for offset in range(0, len(expected), 4))
        self.assertEqual(actual, expected)
        self.write(0x1F000C, 1)
        self.request(7)

    def test_cdma_and_async_irq(self):
        self.write(0x1FC004, 0)
        self.write(0x1FF008, 0)
        self.write(0x1FE20C, 0)
        self.write(0x1FF000, 2)
        self.request(3, generation=1)
        self.assertEqual(self.read(0x1FF004) & ((1 << 16) | (1 << 17)), 0)
        self.write(0, 0xA1B2C3D4)
        self.write(4, 0x11223344)
        self.write(0x1FF00C, (1 << 9) | (1 << 21))
        self.write(0x1FF010, 0)
        self.write(0x1FF018, 4)
        self.write(0x1FF020, (4 << 16) | 4)
        self.write(0x1FF024, 0)
        self.write(0x1FF028, 0x44)
        self.write(0x1FF02C, 0x44)
        self.write(0x1FF030, (1 << 16) | 1)
        self.write(0x1FF008, 1)
        self.write(0x1FE20C, 1)
        self.write(0x1FC004, 1 << 12)
        self.events.clear()
        self.write(0x1FF000, 1)
        try:
            while not any(event[4] for event in self.events):
                event, _ = self.receive()
                self.assertEqual(event[0], 0x101)
                self.events.append(event)
        except TimeoutError:
            registers = {}
            for name, address in (
                ("CH_CMD", 0x1FF000), ("CH_STATUS", 0x1FF004),
                ("CH_INTREN", 0x1FF008), ("CH_CTRL", 0x1FF00C),
                ("CH_XSIZE", 0x1FF020), ("CH_XADDRINC", 0x1FF030),
                ("NSEC_STATUS", 0x1FE208), ("NSEC_CTRL", 0x1FE20C),
                ("IEN_HST", 0x1FC004), ("destination", 4),
            ):
                try:
                    registers[name] = f"0x{self.read(address):08x}"
                except (TimeoutError, AssertionError) as error:
                    registers[name] = str(error)
                    break
            self.fail(f"CDMA IRQ timed out; registers: {registers}")
        self.assertEqual(self.read(4), 0xA1B2C3D4)
        self.assertTrue(self.read(0x1FF004) & (1 << 16))
        self.request(7)


if __name__ == "__main__":
    unittest.main()
