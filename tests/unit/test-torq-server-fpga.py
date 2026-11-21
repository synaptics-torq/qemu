#!/usr/bin/env python3

# Copyright 2026 Synaptics Inc.
#
# SPDX-License-Identifier: Apache-2.0

"""Tests of the server's FPGA backend, on the fake FPGA of tests/unit/fake-fpga."""

import array
import errno
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest


SERVER = sys.argv.pop(1)
HEADER = struct.Struct("<IIIiQQ")
MAPPING = struct.Struct("<QQQII")

MSG_RAM, MSG_PREPARE, MSG_WRITE, MSG_COMMIT = 2, 3, 5, 9
MSG_REPLY, MSG_IRQ = 0x100, 0x101
NSS_STATUS, NSS_START = 0x1F000C, 0x1F0010


class FakeFpgaServer:
    def __init__(self, job_us, *options, read_us=1):
        self.socket, child_socket = socket.socketpair()
        self.socket.settimeout(10)
        environment = dict(os.environ, TORQ_FAKE_FPGA_JOB_US=str(job_us),
                           TORQ_FAKE_FPGA_READ_US=str(read_us))
        self.child = subprocess.Popen(
            [SERVER, "--fd", str(child_socket.fileno()), "--backend", "aws-fpga", *options],
            pass_fds=(child_socket.fileno(),), env=environment)
        child_socket.close()
        self.sequence = 0
        assert self.receive()[0] == 1  # HELLO
        self.ram = tempfile.TemporaryFile()
        os.ftruncate(self.ram.fileno(), 8192)
        self.request(MSG_RAM, payload=struct.pack("<QQ", 8192, 0), fd=self.ram.fileno())
        self.request(MSG_PREPARE, payload=MAPPING.pack(0x40010000, 0, 4096, 0, 3),
                     generation=1)

    def close(self):
        self.socket.close()
        try:
            self.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait()
        self.ram.close()

    def receive(self):
        contents = bytearray()
        while len(contents) < HEADER.size:
            block = self.socket.recv(HEADER.size - len(contents))
            if not block:
                raise EOFError("server disconnected")
            contents.extend(block)
        return HEADER.unpack(contents)

    def request(self, kind, value=0, payload=b"", generation=0, fd=None):
        """Returns the reply's status and value."""
        self.sequence += 1
        header = HEADER.pack(kind, self.sequence, len(payload), 0, value, generation)
        descriptors = [] if fd is None else [
            (socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))
        ]
        self.socket.sendmsg([header[:1]], descriptors)
        self.socket.sendall(header[1:] + payload)
        while True:
            reply = self.receive()
            if reply[0] == MSG_IRQ:
                continue
            assert reply[0:2] == (MSG_REPLY, self.sequence), reply
            return reply[3], reply[4]

    def write(self, address, value):
        return self.request(MSG_WRITE, (address << 32) | value)

    def run_job(self):
        """Launches an NSS job and completes it; returns its measured duration in us."""
        status, duration_ns = self.write(NSS_START, 1)
        assert status == 0, status
        assert self.request(MSG_COMMIT, generation=1)[0] == 0
        assert self.write(NSS_STATUS, 1)[0] == 0
        return duration_ns / 1000


class TorqServerFpgaTest(unittest.TestCase):
    def server(self, job_us, *options):
        server = FakeFpgaServer(job_us, *options)
        self.addCleanup(server.close)
        return server

    def test_short_job_duration(self):
        # The server polls without sleeping at first, so a short job is measured to within
        # about one poll (10 register reads of 1 us), not one 100 us sleep.
        server = self.server(50)
        durations = sorted(server.run_job() for _ in range(9))
        self.assertGreaterEqual(durations[0], 50)
        self.assertLess(durations[len(durations) // 2], 50 + 50)

    def test_long_job_duration(self):
        server = self.server(5000)
        duration = server.run_job()
        self.assertGreaterEqual(duration, 5000)
        self.assertLess(duration, 5000 + 1000)

    def test_hung_job_times_out(self):
        server = self.server(-1, "--job-timeout-ms", "200")
        start = time.monotonic()
        status, _ = server.write(NSS_START, 1)
        elapsed = time.monotonic() - start
        self.assertEqual(status, -errno.ETIMEDOUT)
        self.assertGreaterEqual(elapsed, 0.2)
        self.assertLess(elapsed, 5)

    def test_no_job_timeout(self):
        server = self.server(-1, "--job-timeout-ms", "0")
        server.socket.settimeout(1)
        with self.assertRaises(socket.timeout):
            server.write(NSS_START, 1)

    def test_completion_register_write_while_pending(self):
        server = self.server(50)
        self.assertEqual(server.write(NSS_START, 1)[0], 0)
        self.assertEqual(server.write(NSS_STATUS, 1)[0], -errno.EBUSY)
        self.assertEqual(server.request(MSG_COMMIT, generation=1)[0], 0)
        self.assertEqual(server.write(NSS_STATUS, 1)[0], 0)


if __name__ == "__main__":
    unittest.main()
