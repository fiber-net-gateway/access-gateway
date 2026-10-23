#!/usr/bin/env python3
"""Explicit, disposable loopback test of two real access-server generations.

No credentials, production configuration, or public services are used. This is
an integration test, deliberately separate from deterministic CTest unit tests.
"""

import argparse
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import socketserver
import ssl
import sys
import struct
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request


def free_port(excluded):
    while True:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        if port not in excluded:
            excluded.add(port)
            return port


def control(path, command='STATUS'):
    request = json.dumps({
        'protocolVersion': 1,
        'requestId': 'integration',
        'command': command,
    }).encode()
    with socket.socket(socket.AF_UNIX) as stream:
        stream.settimeout(2)
        stream.connect(str(path))
        stream.sendall(struct.pack('!I', len(request)) + request)

        def exact(size):
            result = bytearray()
            while len(result) < size:
                chunk = stream.recv(size - len(result))
                if not chunk:
                    raise RuntimeError('control endpoint closed before reply')
                result.extend(chunk)
            return bytes(result)

        size, = struct.unpack('!I', exact(4))
        if size > 8192:
            raise RuntimeError('oversized control response')
        return json.loads(exact(size))


def wait_status(process, path, predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'process {process.pid} exited with {process.returncode}')
        try:
            status = control(path)
            if predicate(status):
                return status
            if status.get('phase') == 'failed':
                raise RuntimeError(f'upgrade failed: {status.get("error")}')
        except (OSError, json.JSONDecodeError):
            pass
        time.sleep(0.02)
    raise RuntimeError('process status did not reach its expected state')


def request(port, path='/health', timeout=2):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        connection.request('GET', path, headers={'Host': 'upgrade.local', 'Connection': 'close'})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


class GateProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def run(server_binary, ctl_binary, rnacos_binary, output, tls=False, http3=False, workers=None):
    processes = []
    logs = []
    paused = False
    stop_probe = threading.Event()
    upstream_release = threading.Event()
    upstream_entered = threading.Event()
    gate = threading.Event()
    gate.set()
    excluded = set()
    http_port, grpc_port, proxy_port, traffic_port, metrics_port = [free_port(excluded) for _ in range(5)]
    tls_port = free_port(excluded)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    failures = []
    samples = []
    slow_result = []

    def tls_checks():
        if not tls:
            return
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        connection = http.client.HTTPSConnection('127.0.0.1', tls_port, timeout=2, context=context)
        try:
            connection.request('GET', '/health', headers={'Host': 'upgrade.local', 'Connection': 'close'})
            response = connection.getresponse()
            if (response.status, response.read()) != (200, b'ok'):
                raise RuntimeError('TLS HTTP/1.1 health request failed')
        finally:
            connection.close()
        h2 = subprocess.run([
            'curl', '--silent', '--show-error', '--http2', '--insecure', '--max-time', '2', '--noproxy', '*',
            '--resolve', f'upgrade.local:{tls_port}:127.0.0.1', f'https://upgrade.local:{tls_port}/health',
            '--output', os.devnull, '--write-out', '%{http_version} %{http_code}',
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=4)
        if h2.returncode != 0 or h2.stdout != '2 200':
            raise RuntimeError('TLS HTTP/2 health request failed (curl must support HTTP/2)')

    class ProxyHandler(socketserver.BaseRequestHandler):
        def handle(self):
            while not gate.wait(0.1):
                if stop_probe.is_set():
                    return
            try:
                with socket.create_connection(('127.0.0.1', grpc_port), timeout=2) as remote:
                    remote.settimeout(None)

                    def forward(source, target):
                        try:
                            while chunk := source.recv(65536):
                                target.sendall(chunk)
                        except OSError:
                            pass
                        finally:
                            try:
                                target.shutdown(socket.SHUT_WR)
                            except OSError:
                                pass

                    other = threading.Thread(target=forward, args=(self.request, remote), daemon=True)
                    other.start()
                    forward(remote, self.request)
            except OSError:
                pass

    class Upstream(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            upstream_entered.set()
            if not upstream_release.wait(20):
                self.send_error(504)
                return
            body = b'old request completed'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    proxy = GateProxy(('127.0.0.1', proxy_port), ProxyHandler)
    upstream = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    upstream_thread = threading.Thread(target=upstream.serve_forever, daemon=True)
    proxy_thread.start()
    upstream_thread.start()
    rnacos = None
    udp_peer = None
    try:
        environment = os.environ.copy()
        environment.update({
            'RNACOS_SDK_HOST': '127.0.0.1',
            'RNACOS_HTTP_PORT': str(http_port),
            'RNACOS_GRPC_PORT': str(grpc_port),
            'RNACOS_HTTP_CONSOLE_PORT': '0',
            'RNACOS_ENABLE_NO_AUTH': 'true',
            'RNACOS_ENABLE_OPEN_API_AUTH': 'false',
            'RNACOS_DATA_DIR': str(output / 'rnacos-data'),
            'RNACOS_RAFT_NODE_ADDR': f'127.0.0.1:{grpc_port}',
            'RUST_LOG': 'warn',
        })

        def start(binary, arguments, log_name, env=None):
            log = (output / log_name).open('w')
            logs.append(log)
            command = [str(binary), *arguments]
            if binary == server_binary and workers:
                cpus = sorted(os.sched_getaffinity(0))[:workers]
                if len(cpus) != workers or not shutil.which('taskset'):
                    raise RuntimeError('requested worker affinity requires available CPUs and taskset')
                command = ['taskset', '--cpu-list', ','.join(map(str, cpus)), *command]
            process = subprocess.Popen(command, cwd=output, env=env, stdout=log, stderr=log)
            processes.append(process)
            return process

        rnacos = start(rnacos_binary, [], 'rnacos.log', environment)
        deadline = time.monotonic() + 20
        while True:
            try:
                with socket.create_connection(('127.0.0.1', grpc_port), timeout=0.2):
                    break
            except OSError:
                if rnacos.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('disposable rnacos did not start')
                time.sleep(0.05)

        def publish(data_id, group, content):
            payload = urllib.parse.urlencode({
                'dataId': data_id, 'group': group, 'tenant': '', 'content': content, 'type': 'json',
            }).encode()
            publication = urllib.request.Request(f'http://127.0.0.1:{http_port}/nacos/v1/cs/configs', data=payload)
            with opener.open(publication, timeout=3) as response:
                if response.read().strip().lower() != b'true':
                    raise RuntimeError('fixture publication failed')

        route = {
            'version': 1,
            'host': {'upgrade.local': {'https': 'S_NOT_MUST'}},
            'routes': [
                {'path': '/health', 'type': 'RESPONSE', 'status': 200,
                 'body': {'type': 'TEXT', 'content': 'ok'}},
                {'path': '/slow', 'type': 'PROXY', 'addresses': [f'127.0.0.1:{upstream.server_port}'],
                 'timeout': '30S'},
            ],
        }
        publish('ploto.unified-access.route.upgrade', 'ACCESS-SERVER', json.dumps(route))
        publish('ploto.unified-access.projects', 'ACCESS-SERVER', 'upgrade')
        publish('ploto.unified-access.gray-match', 'DEFAULT_GROUP', '{}')
        if tls:
            key = output / 'test-key.pem'
            certificate = output / 'test-cert.pem'
            subprocess.run([
                'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                '-subj', '/CN=upgrade.local', '-addext', 'subjectAltName=DNS:upgrade.local',
                '-keyout', str(key), '-out', str(certificate),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            key.chmod(0o600)
            publish('ploto.unified-access.tls-certificates', 'ACCESS-SERVER', json.dumps({
                'schemaVersion': 1, 'version': 1, 'defaultCertificate': 'local',
                'certificates': [{'id': 'local', 'certificatePem': certificate.read_text(),
                                  'privateKeyPem': key.read_text()}],
            }))
        configuration = output / 'access.env'
        configuration.write_text('\n'.join([
            f'ACCESS_SERVER_TLS_ENABLED={str(tls).lower()}', f'ACCESS_SERVER_HTTP3_ENABLED={str(http3).lower()}',
            'ACCESS_SERVER_TLS_LISTEN_ADDRESS=127.0.0.1', f'ACCESS_SERVER_TLS_LISTEN_PORT={tls_port}',
            'ACCESS_SERVER_PLAIN_LISTEN_ENABLED=true', 'ACCESS_SERVER_PLAIN_LISTEN_ADDRESS=127.0.0.1',
            f'ACCESS_SERVER_PLAIN_LISTEN_PORT={traffic_port}',
            'ACCESS_SERVER_METRICS_LISTEN_ADDRESS=127.0.0.1', f'ACCESS_SERVER_METRICS_LISTEN_PORT={metrics_port}',
            'ACCESS_SERVER_NETWORK_ENTRY=internet', 'ACCESS_SERVER_ACCESS_LOG_DIR=',
            'ACCESS_SERVER_ADVERTISE_ADDRESS=127.0.0.1', 'ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS=15000',
            'NACOS_SERVER_ADDRESSES=127.0.0.1', f'NACOS_HTTP_PORT={http_port}', f'NACOS_GRPC_PORT={proxy_port}',
        ]) + '\n')
        configuration.chmod(0o600)
        directory = output / 'control'
        directory.mkdir(mode=0o700)
        if http3:
            # Keep a same-port UDP peer alive across the entire TCP handoff.
            # No REUSEADDR: both gateway generations must really use REUSEPORT,
            # including when CPU affinity gives them only one worker.
            udp_peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp_peer.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            udp_peer.bind(('127.0.0.1', tls_port))
        old = start(server_binary, [str(configuration), f'--control-dir={directory}'], 'old.log')
        old_socket = directory / f'{old.pid}.sock'
        initial_status = wait_status(old, old_socket, lambda value: value['state'] == 'serving')
        if initial_status['http3'] != http3:
            raise RuntimeError('old HTTP/3 setting was not applied')
        assert request(traffic_port) == (200, b'ok')
        tls_checks()

        def slow_request():
            try:
                slow_result.append(request(traffic_port, '/slow', timeout=20))
            except Exception as error:
                slow_result.append(type(error).__name__)

        slow = threading.Thread(target=slow_request, daemon=True)
        slow.start()
        if not upstream_entered.wait(5):
            raise RuntimeError('old generation did not reach delayed upstream')

        def probe():
            while not stop_probe.is_set():
                began = time.monotonic()
                try:
                    status, body = request(traffic_port, timeout=0.5)
                    if (status, body) != (200, b'ok'):
                        failures.append('unexpected_response')
                except Exception as error:
                    failures.append(type(error).__name__)
                samples.append((began, time.monotonic()))
                stop_probe.wait(0.005)

        health = threading.Thread(target=probe, daemon=True)
        health.start()
        gate.clear()
        fresh = start(server_binary, [str(configuration), '--start-mode=prewarm', f'--control-dir={directory}'], 'new.log')
        fresh_socket = directory / f'{fresh.pid}.sock'
        wait_status(fresh, fresh_socket, lambda value: value['state'] == 'preparing')
        before_delay = len(samples)
        time.sleep(0.8)
        if control(fresh_socket)['ready']:
            raise RuntimeError('new generation reported ready before initial Nacos responses')
        if len(samples) - before_delay < 3 or failures:
            raise RuntimeError('old generation stopped serving during delayed prewarm')
        requests_during_gate = len(samples) - before_delay
        gate.set()
        wait_status(fresh, fresh_socket, lambda value: value['ready'])

        # Hold server-side RPCs after prewarm. The new entry listener must serve
        # without waiting for registration and the old reply must be local-only.
        rnacos.send_signal(signal.SIGSTOP)
        paused = True
        began = time.monotonic()
        upgrade = subprocess.run([
            str(ctl_binary), 'upgrade', '--old-socket', str(old_socket), '--new-socket', str(fresh_socket), '--wait',
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)
        elapsed = time.monotonic() - began
        if upgrade.returncode != 0:
            raise RuntimeError(f'upgrade command failed: {upgrade.stdout.strip()} {upgrade.stderr.strip()}')
        new_status = control(fresh_socket)
        if new_status['state'] != 'serving' or new_status['registration'] == 'registered':
            raise RuntimeError('new traffic readiness incorrectly depends on registration')
        if old.poll() is not None or control(old_socket)['state'] != 'draining':
            raise RuntimeError('old generation exited before its outstanding request finished')
        if new_status['http3'] != http3 or (http3 and udp_peer.fileno() < 0):
            raise RuntimeError('new generation did not keep the requested UDP overlap policy')
        assert request(traffic_port) == (200, b'ok')
        tls_checks()
        time.sleep(0.2)
        rnacos.send_signal(signal.SIGCONT)
        paused = False
        upstream_release.set()
        slow.join(timeout=5)
        if slow_result != [(200, b'old request completed')]:
            raise RuntimeError('outstanding old-generation request did not finish')
        if old.wait(timeout=10) != 0:
            raise RuntimeError('old generation did not exit cleanly')
        wait_status(fresh, fresh_socket, lambda value: value['registration'] == 'registered')
        if udp_peer is not None:
            udp_peer.close()
        if old_socket.exists() or not fresh_socket.exists():
            raise RuntimeError('old generation removed the wrong PID socket')
        # A disposable idle prewarm can be aborted through the same PID socket
        # without disturbing the active generation or removing its endpoint.
        idle = start(server_binary, [str(configuration), '--start-mode=prewarm', f'--control-dir={directory}'], 'idle.log')
        idle_socket = directory / f'{idle.pid}.sock'
        wait_status(idle, idle_socket, lambda value: value['ready'])
        aborted = subprocess.run([str(ctl_binary), 'abort', '--socket', str(idle_socket)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
        if aborted.returncode != 0 or idle.wait(timeout=5) != 0 or idle_socket.exists():
            raise RuntimeError('idle prewarm did not acknowledge abort and clean up')
        if not fresh_socket.exists() or request(traffic_port) != (200, b'ok'):
            raise RuntimeError('aborting idle prewarm affected the active generation')
        stop_probe.set()
        health.join(timeout=2)
        print(json.dumps({
            'passed': True,
            'old_pid': old.pid,
            'new_pid': fresh.pid,
            'prewarm_gate_seconds': 0.8,
            'requests_during_gate': requests_during_gate,
            'upgrade_command_ms': round(elapsed * 1000, 3),
            'probe_attempts': len(samples),
            'probe_failures': len(failures),
            'registration_pending_while_serving': True,
            'registration_recovered': True,
            'old_request_completed': True,
            'idle_prewarm_aborted': True,
            'tls_http1_and_http2_verified': tls,
            'http3_enabled': http3,
            'udp_peer_held_during_cutover': http3,
            'worker_affinity_limit': workers,
            'note': 'command duration includes preflight and CLI polling; it is not a measured listener gap',
        }, indent=2))
    finally:
        stop_probe.set()
        gate.set()
        upstream_release.set()
        if paused and rnacos and rnacos.poll() is None:
            rnacos.send_signal(signal.SIGCONT)
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if udp_peer is not None:
            udp_peer.close()
        proxy.shutdown()
        proxy.server_close()
        upstream.shutdown()
        upstream.server_close()
        for log in logs:
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--ctl', type=Path, required=True)
    parser.add_argument('--rnacos', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--http3', action='store_true', help='enable HTTP/3 and retain a reusable UDP peer across handoff (also enables TLS)')
    parser.add_argument('--workers', type=int, choices=(1, 2), help='limit gateway CPU affinity to exercise single/multiple workers')
    parser.add_argument('--tls', action='store_true', help='also verify TLS HTTP/1.1 and HTTP/2 (requires openssl and HTTP/2 curl)')
    arguments = parser.parse_args()
    for binary in (arguments.server, arguments.ctl, arguments.rnacos):
        if not binary.is_file():
            parser.error(f'binary does not exist: {binary}')
    output = arguments.output_dir.resolve() if arguments.output_dir else Path(tempfile.mkdtemp(prefix='access-upgrade-'))
    if arguments.output_dir:
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        run(arguments.server.resolve(), arguments.ctl.resolve(), arguments.rnacos.resolve(), output, arguments.tls or arguments.http3, arguments.http3, arguments.workers)
    except Exception as error:
        print(f'Upgrade integration failed: {error}; artifacts: {output}', file=sys.stderr)
        return 1
    if not arguments.output_dir:
        shutil.rmtree(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
