#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline DNS/TCP/TLS/HTTPS peer for Sapote's QEMU dgram NIC."""

from __future__ import annotations

import argparse
import ssl
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import network_fixture as network


ROOT = Path(__file__).resolve().parent.parent
CERTIFICATE = ROOT / "tests" / "fixtures" / "tls" / "valid.pem"
PRIVATE_KEY = ROOT / "tests" / "fixtures" / "tls" / "valid-key.pem"
HOSTNAME = b"repo.sapote.test"
BODY = b"hello from the Sapote HTTPS peer\n"
SERVER_ISN = 0x63000000
TLS_PORT = 443
MAX_REQUEST = 2048
TCP_CHUNK = 1200


def make_server_context() -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers(
        "ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256"
    )
    context.load_cert_chain(CERTIFICATE, PRIVATE_KEY)
    return context


@dataclass
class HttpsPeer:
    client_port: int
    client_next: int
    server_next: int
    incoming: ssl.MemoryBIO
    outgoing: ssl.MemoryBIO
    tls: ssl.SSLObject
    handshake_complete: bool = False
    closing: bool = False
    finished: bool = False
    request: bytearray = field(default_factory=bytearray)


class HttpsFixture(network.Fixture):
    def __init__(self, group: str, port: int, peer_port: int, capture: Path,
                 unicast: bool = False) -> None:
        super().__init__(group, port, peer_port, "normal", capture, unicast)
        self.https_peers: dict[int, HttpsPeer] = {}
        self.context = make_server_context()

    def new_peer(self, client_port: int, sequence: int) -> HttpsPeer:
        incoming = ssl.MemoryBIO()
        outgoing = ssl.MemoryBIO()
        tls = self.context.wrap_bio(incoming, outgoing, server_side=True)
        return HttpsPeer(
            client_port, (sequence + 1) & 0xFFFFFFFF,
            (SERVER_ISN + client_port) & 0xFFFFFFFF,
            incoming, outgoing, tls,
        )

    def send_https(self, peer: HttpsPeer, flags: int,
                   payload: bytes = b"") -> None:
        segment = network.tcp(
            network.HTTP_IP, network.GUEST_IP, TLS_PORT, peer.client_port,
            peer.server_next, peer.client_next, flags, payload,
        )
        self.send_ipv4(
            network.GUEST_MAC, network.HTTP_IP, network.GUEST_IP, 6, segment
        )
        peer.server_next = (
            peer.server_next + len(payload) + (1 if flags & 0x03 else 0)
        ) & 0xFFFFFFFF

    def drain_tls(self, peer: HttpsPeer) -> None:
        while peer.outgoing.pending:
            block = peer.outgoing.read(min(TCP_CHUNK, peer.outgoing.pending))
            self.send_https(peer, 0x18, block)

    def advance_tls(self, peer: HttpsPeer) -> None:
        try:
            if not peer.handshake_complete:
                peer.tls.do_handshake()
                peer.handshake_complete = True
            if peer.handshake_complete and not peer.closing:
                while True:
                    try:
                        block = peer.tls.read(512)
                    except ssl.SSLWantReadError:
                        break
                    if not block:
                        break
                    if len(peer.request) + len(block) > MAX_REQUEST:
                        raise ValueError("HTTPS request exceeded fixture bound")
                    peer.request.extend(block)
                if b"\r\n\r\n" in peer.request:
                    expected = (
                        b"GET /artifact.bin HTTP/1.1\r\n"
                        b"Host: repo.sapote.test\r\n"
                        b"Accept: application/octet-stream\r\n"
                        b"Accept-Encoding: identity\r\n"
                        b"Connection: close\r\n\r\n"
                    )
                    if bytes(peer.request) != expected:
                        raise ValueError("HTTPS request was not canonical")
                    response = (
                        b"HTTP/1.1 200 OK\r\nContent-Length: "
                        + str(len(BODY)).encode("ascii")
                        + b"\r\nConnection: close\r\n\r\n"
                        + BODY
                    )
                    if peer.tls.write(response) != len(response):
                        raise ValueError("short fixture TLS write")
                    peer.closing = True
            if peer.closing:
                try:
                    peer.tls.unwrap()
                    peer.finished = True
                except ssl.SSLWantReadError:
                    pass
        except ssl.SSLWantReadError:
            pass
        self.drain_tls(peer)
        if peer.finished:
            self.send_https(peer, 0x11)

    def handle_dns(self, source_ip: bytes, source_port: int,
                   payload: bytes) -> None:
        parsed = network.dns_question(payload)
        if parsed is None:
            return
        name, question_end = parsed
        identifier = payload[:2]
        if name == HOSTNAME:
            flags, answers = 0x8180, 1
            suffix = (
                b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04"
                + network.HTTP_IP
            )
        else:
            flags, answers, suffix = 0x8183, 0, b""
        answer = identifier + struct.pack("!HHHHH", flags, 1, answers, 0, 0)
        answer += payload[12:question_end] + suffix
        datagram = network.udp(
            network.DNS_IP, source_ip, 53, source_port, answer
        )
        self.send_ipv4(
            network.GUEST_MAC, network.DNS_IP, source_ip, 17, datagram
        )

    def handle_tcp(self, source_ip: bytes, destination_ip: bytes,
                   segment: bytes) -> None:
        if len(segment) < 20 or destination_ip != network.HTTP_IP:
            return
        source_port, destination_port, sequence, _ = struct.unpack_from(
            "!HHII", segment, 0
        )
        offset = (segment[12] >> 4) * 4
        flags = segment[13]
        if offset < 20 or offset > len(segment) or destination_port != TLS_PORT:
            return
        peer = self.https_peers.get(source_port)
        if flags & 0x02 and not flags & 0x10:
            peer = self.new_peer(source_port, sequence)
            self.https_peers[source_port] = peer
            self.send_https(peer, 0x12)
            return
        if peer is None:
            return
        payload = segment[offset:]
        if payload:
            if sequence != peer.client_next:
                self.send_https(peer, 0x10)
                return
            peer.client_next = (sequence + len(payload)) & 0xFFFFFFFF
            self.send_https(peer, 0x10)
            peer.incoming.write(payload)
            self.advance_tls(peer)
        elif flags & 0x01:
            peer.client_next = (sequence + 1) & 0xFFFFFFFF
            self.send_https(peer, 0x10)


def self_test() -> int:
    client_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_context.minimum_version = ssl.TLSVersion.TLSv1_2
    client_context.maximum_version = ssl.TLSVersion.TLSv1_2
    client_context.load_verify_locations(
        ROOT / "tests" / "fixtures" / "tls" / "ca.pem"
    )
    server_in, server_out = ssl.MemoryBIO(), ssl.MemoryBIO()
    client_in, client_out = ssl.MemoryBIO(), ssl.MemoryBIO()
    server = make_server_context().wrap_bio(
        server_in, server_out, server_side=True
    )
    client = client_context.wrap_bio(
        client_in, client_out, server_hostname="repo.sapote.test"
    )
    server_done = client_done = False
    for _ in range(64):
        if not client_done:
            try:
                client.do_handshake()
                client_done = True
            except ssl.SSLWantReadError:
                pass
        if client_out.pending:
            server_in.write(client_out.read())
        if not server_done:
            try:
                server.do_handshake()
                server_done = True
            except ssl.SSLWantReadError:
                pass
        if server_out.pending:
            client_in.write(server_out.read())
        if server_done and client_done:
            break
    assert server_done and client_done
    assert client.version() == "TLSv1.2"
    parsed = network.dns_question(
        b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
        b"\x04repo\x06sapote\x04test\x00\x00\x01\x00\x01"
    )
    assert parsed is not None and parsed[0] == HOSTNAME
    assert len(BODY) == 33
    assert CERTIFICATE.is_file() and PRIVATE_KEY.is_file()
    print("HTTPS network fixture self-test: DNS, bounds, TLS 1.2 certificate passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", default=network.GROUP)
    parser.add_argument("--port", type=int, default=network.PORT)
    parser.add_argument("--peer-port", type=int, default=network.PORT + 1)
    parser.add_argument("--unicast", action="store_true")
    parser.add_argument("--mode", default="https", choices=("https",))
    parser.add_argument(
        "--capture", type=Path, default=Path("build/https-network.pcap")
    )
    parser.add_argument("--ready", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    fixture = HttpsFixture(
        args.group, args.port, args.peer_port, args.capture, args.unicast
    )
    try:
        fixture.run(args.ready)
    except KeyboardInterrupt:
        return 0
    finally:
        fixture.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
