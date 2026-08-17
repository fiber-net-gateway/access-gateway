#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import queue
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path


DEFAULT_IMAGE = (
    'qingpan/rnacos@sha256:'
    '6c749166929fa565152d26acc344ccdfc437eb6cc57e02a752b5a3cf338edfb2'
)
READY_MARKER = 'ACCESS_SERVER_INTEROP_READY_FOR_DROP'


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as candidate:
        candidate.bind(('127.0.0.1', 0))
        return int(candidate.getsockname()[1])


def wait_for_port(port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=0.25):
                return True
        except OSError:
            time.sleep(0.05)
    return False


class TcpFaultProxy:
    def __init__(self, upstream_port: int, initial_drops: int = 2) -> None:
        self._upstream_port = upstream_port
        self._initial_drop_budget = initial_drops
        self._initial_drops_done = 0
        self._dropped_active = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._connections: dict[int, tuple[socket.socket, socket.socket]] = {}
        self._workers: list[threading.Thread] = []
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(('127.0.0.1', 0))
        self._listener.listen(16)
        self._listener.settimeout(0.2)
        self.port = int(self._listener.getsockname()[1])
        self._accept_thread = threading.Thread(target=self._accept_loop, name='fault-proxy-accept')

    @property
    def initial_drops_done(self) -> int:
        with self._lock:
            return self._initial_drops_done

    @property
    def dropped_active(self) -> int:
        with self._lock:
            return self._dropped_active

    @property
    def active_count(self) -> int:
        with self._lock:
            return len(self._connections)

    def start(self) -> None:
        self._accept_thread.start()

    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                client, _ = self._listener.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            with self._lock:
                should_drop = self._initial_drop_budget > 0
                if should_drop:
                    self._initial_drop_budget -= 1
                    self._initial_drops_done += 1
            if should_drop:
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                client.close()
                continue
            worker = threading.Thread(target=self._relay, args=(client,), name='fault-proxy-relay')
            worker.start()
            with self._lock:
                self._workers.append(worker)

    def _relay(self, client: socket.socket) -> None:
        upstream: socket.socket | None = None
        connection_id = id(client)
        try:
            upstream = socket.create_connection(('127.0.0.1', self._upstream_port), timeout=2.0)
            client.setblocking(False)
            upstream.setblocking(False)
            with self._lock:
                self._connections[connection_id] = (client, upstream)
            while not self._stop.is_set():
                readable, _, _ = select.select((client, upstream), (), (), 0.2)
                if not readable:
                    continue
                for source in readable:
                    target = upstream if source is client else client
                    payload = source.recv(65536)
                    if not payload:
                        return
                    view = memoryview(payload)
                    while view and not self._stop.is_set():
                        try:
                            sent = target.send(view)
                        except BlockingIOError:
                            select.select((), (target,), (), 0.2)
                            continue
                        if sent <= 0:
                            return
                        view = view[sent:]
        except OSError:
            return
        finally:
            with self._lock:
                self._connections.pop(connection_id, None)
            for stream in (client, upstream):
                if stream is None:
                    continue
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()

    def wait_for_active(self, minimum: int, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.active_count >= minimum:
                return True
            time.sleep(0.01)
        return False

    def drop_active(self) -> int:
        with self._lock:
            connections = list(self._connections.values())
            self._dropped_active += len(connections)
        for client, upstream in connections:
            for stream in (client, upstream):
                try:
                    stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    stream.close()
                except OSError:
                    pass
        return len(connections)

    def close(self) -> None:
        self._stop.set()
        try:
            self._listener.close()
        except OSError:
            pass
        self.drop_active()
        self._accept_thread.join(timeout=2.0)
        with self._lock:
            workers = list(self._workers)
        for worker in workers:
            worker.join(timeout=2.0)


def docker_output(arguments: list[str]) -> str:
    completed = subprocess.run(
        ['docker', *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout.strip()


def read_output(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line)


def run(binary: Path, image: str) -> int:
    if shutil.which('docker') is None:
        print('Docker is unavailable; external rnacos interoperability was not run', file=sys.stderr)
        return 77
    try:
        image_id = docker_output(['image', 'inspect', '--format', '{{.Id}}', image])
    except subprocess.CalledProcessError:
        print(f'Pinned image {image} is not present; refusing an implicit pull', file=sys.stderr)
        return 77

    container_name = f'access-gateway-rnacos-interop-{os.getpid()}-{uuid.uuid4().hex[:8]}'
    http_port = reserve_port()
    grpc_port = reserve_port()
    while grpc_port == http_port:
        grpc_port = reserve_port()
    proxy: TcpFaultProxy | None = None
    child: subprocess.Popen[str] | None = None
    try:
        docker_output(
            [
                'run',
                '--detach',
                '--rm',
                '--pull=never',
                '--name',
                container_name,
                '--log-driver=none',
                '--tmpfs',
                '/io:rw,nosuid,nodev,size=64m',
                '--env',
                'RNACOS_HTTP_PORT=8848',
                '--env',
                'RNACOS_GRPC_PORT=9848',
                '--env',
                'RNACOS_ENABLE_OPEN_API_AUTH=false',
                '--env',
                'RNACOS_CONSOLE_ENABLE_CAPTCHA=false',
                '--publish',
                f'127.0.0.1:{http_port}:8848',
                '--publish',
                f'127.0.0.1:{grpc_port}:9848',
                image,
            ],
        )
        if not wait_for_port(http_port, 30.0) or not wait_for_port(grpc_port, 30.0):
            print('Pinned rnacos container did not expose both loopback ports', file=sys.stderr)
            return 1

        proxy = TcpFaultProxy(grpc_port)
        proxy.start()
        print(f'external rnacos image: {image} ({image_id})')
        child = subprocess.Popen(
            [str(binary), str(http_port), str(proxy.port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        output_lines: queue.Queue[str] = queue.Queue()
        reader = threading.Thread(target=read_output, args=(child, output_lines), name='interop-output-reader')
        reader.start()
        marker_seen = False
        deadline = time.monotonic() + 65.0
        while child.poll() is None and time.monotonic() < deadline:
            try:
                line = output_lines.get(timeout=0.1)
            except queue.Empty:
                continue
            sys.stdout.write(line)
            sys.stdout.flush()
            if READY_MARKER in line and not marker_seen:
                marker_seen = True
                if not proxy.wait_for_active(2, 5.0):
                    print('Expected two ready gRPC connections before fault injection', file=sys.stderr)
                dropped = proxy.drop_active()
                print(f'injected post-ready gRPC connection resets: {dropped}')
        if child.poll() is None:
            child.kill()
            child.wait(timeout=5.0)
            print('External rnacos interoperability process exceeded its deadline', file=sys.stderr)
            return 1
        reader.join(timeout=2.0)
        while not output_lines.empty():
            line = output_lines.get_nowait()
            sys.stdout.write(line)
            if READY_MARKER in line:
                marker_seen = True
        if child.returncode != 0:
            return int(child.returncode or 1)
        if not marker_seen or proxy.initial_drops_done != 2 or proxy.dropped_active < 2:
            print(
                'Interop binary passed without proving both pre-connect and post-ready fault injection',
                file=sys.stderr,
            )
            return 1
        print(
            f'fault injection summary: initial_resets={proxy.initial_drops_done} '
            f'post_ready_resets={proxy.dropped_active}',
        )
        return 0
    except subprocess.CalledProcessError as error:
        print(f'Docker-backed rnacos interoperability setup failed: {error}', file=sys.stderr)
        return 1
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=5.0)
        if proxy is not None:
            proxy.close()
        subprocess.run(
            ['docker', 'rm', '--force', container_name],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Run deterministic access-server reconnect interoperability against pinned rnacos',
    )
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--image', default=DEFAULT_IMAGE)
    arguments = parser.parse_args()
    if not arguments.binary.is_file():
        parser.error(f'interoperability binary does not exist: {arguments.binary}')
    return run(arguments.binary.resolve(), arguments.image)


if __name__ == '__main__':
    raise SystemExit(main())
