#!/usr/bin/env python3
"""Explicit, disposable loopback test of boss-managed access-server generations.

access-server-boss supervises real access-server processes; access-serverctl
replaces the binary and drives upgrades. No credentials, production
configuration, or public services are used. This is an integration test,
deliberately separate from deterministic CTest unit tests.
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
        'protocolVersion': 2,
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


def wait_status(boss, path, predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if boss.poll() is not None:
            raise RuntimeError(f'boss {boss.pid} exited with {boss.returncode}')
        try:
            status = control(path)
            if predicate(status):
                return status
        except (OSError, json.JSONDecodeError, RuntimeError):
            pass
        time.sleep(0.02)
    raise RuntimeError('boss status did not reach its expected state')


def alive(pid):
    try:
        with open(f'/proc/{pid}/stat') as stat:
            # A zombie has exited even before its parent reaps it.
            return stat.read().rsplit(')', 1)[1].split()[0] != 'Z'
    except FileNotFoundError:
        return False


def wait_gone(pid, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not alive(pid):
            return time.monotonic()
        time.sleep(0.02)
    raise RuntimeError(f'process {pid} did not exit within {seconds}s')


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


def run(server_binary, boss_binary, ctl_binary, rnacos_binary, output, tls=False, http3=False, workers=None):
    processes = []
    logs = []
    stop_probe = threading.Event()
    upstream_release = threading.Event()
    upstream_entered = threading.Event()
    hold_release = threading.Event()
    hold_entered = threading.Event()
    gate = threading.Event()
    gate.set()
    excluded = set()
    http_port, grpc_port, proxy_port, traffic_port, metrics_port = [free_port(excluded) for _ in range(5)]
    tls_port = free_port(excluded)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    failures = []
    samples = []
    slow_result = []
    held_result = []
    control_directories = []

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
            held = self.path == '/hold'
            (hold_entered if held else upstream_entered).set()
            if not (hold_release if held else upstream_release).wait(20):
                self.send_error(504)
                return
            body = b'old request completed'
            try:
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except OSError:
                # The held request's gateway is killed on purpose.
                pass

        def log_message(self, *_):
            pass

    proxy = GateProxy(('127.0.0.1', proxy_port), ProxyHandler)
    upstream = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    upstream_thread = threading.Thread(target=upstream.serve_forever, daemon=True)
    proxy_thread.start()
    upstream_thread.start()
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

        def start(command, log_name, env=None):
            log = (output / log_name).open('w')
            logs.append(log)
            process = subprocess.Popen([str(part) for part in command], cwd=output, env=env, stdout=log, stderr=log)
            processes.append(process)
            return process

        rnacos = start([rnacos_binary], 'rnacos.log', environment)
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

        upstream_address = f'127.0.0.1:{upstream.server_port}'
        route = {
            'version': 1,
            'host': {'upgrade.local': {'https': 'S_NOT_MUST'}},
            'routes': [
                {'path': '/health', 'type': 'RESPONSE', 'status': 200,
                 'body': {'type': 'TEXT', 'content': 'ok'}},
                {'path': '/slow', 'type': 'PROXY', 'addresses': [upstream_address], 'timeout': '30S'},
                {'path': '/hold', 'type': 'PROXY', 'addresses': [upstream_address], 'timeout': '30S'},
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
        # Both generations share one access-log directory.
        log_directory = output / 'access-logs'
        log_directory.mkdir(mode=0o700)
        configuration = output / 'access.env'
        configuration.write_text('\n'.join([
            f'ACCESS_SERVER_TLS_ENABLED={str(tls).lower()}', f'ACCESS_SERVER_HTTP3_ENABLED={str(http3).lower()}',
            'ACCESS_SERVER_TLS_LISTEN_ADDRESS=127.0.0.1', f'ACCESS_SERVER_TLS_LISTEN_PORT={tls_port}',
            'ACCESS_SERVER_PLAIN_LISTEN_ENABLED=true', 'ACCESS_SERVER_PLAIN_LISTEN_ADDRESS=127.0.0.1',
            f'ACCESS_SERVER_PLAIN_LISTEN_PORT={traffic_port}',
            'ACCESS_SERVER_METRICS_LISTEN_ADDRESS=127.0.0.1', f'ACCESS_SERVER_METRICS_LISTEN_PORT={metrics_port}',
            'ACCESS_SERVER_NETWORK_ENTRY=internet', f'ACCESS_SERVER_ACCESS_LOG_DIR={log_directory}',
            'ACCESS_SERVER_ADVERTISE_ADDRESS=127.0.0.1', 'ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS=15000',
            'NACOS_SERVER_ADDRESSES=127.0.0.1', f'NACOS_HTTP_PORT={http_port}', f'NACOS_GRPC_PORT={proxy_port}',
        ]) + '\n')
        configuration.chmod(0o600)
        # The boss runs the binary installed here; access-serverctl replaces it.
        installed = output / 'bin' / 'access-server'
        installed.parent.mkdir(mode=0o700)
        shutil.copy2(server_binary, installed)
        installed.chmod(0o755)
        builds = output / 'builds'
        builds.mkdir(mode=0o700)
        # Unix socket paths are short; artifact directories may not be.
        directory = Path(tempfile.mkdtemp(prefix='access-ctl-'))
        control_directories.append(directory)
        boss_socket = directory / 'boss.sock'
        if http3:
            # Keep a same-port UDP peer alive across the entire TCP handoff.
            # No REUSEADDR: both gateway generations must really use REUSEPORT,
            # including when CPU affinity gives them only one worker.
            udp_peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp_peer.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            udp_peer.bind(('127.0.0.1', tls_port))

        def start_boss(log_name):
            command = [boss_binary, f'--socket={boss_socket}', f'--server={installed}', f'--config={configuration}',
                       '--prewarm-timeout-millis=30000']
            if workers:
                # Children inherit the boss affinity.
                cpus = sorted(os.sched_getaffinity(0))[:workers]
                if len(cpus) != workers or not shutil.which('taskset'):
                    raise RuntimeError('requested worker affinity requires available CPUs and taskset')
                command = ['taskset', '--cpu-list', ','.join(map(str, cpus)), *command]
            return start(command, log_name)

        def ctl(*arguments, timeout=10):
            return subprocess.run([str(ctl_binary), *arguments, '--socket', str(boss_socket)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)

        def new_build(name):
            build = builds / name
            shutil.copy2(server_binary, build)
            return build

        boss = start_boss('boss.log')
        first = wait_status(boss, boss_socket, lambda value: value['activeState'] == 'serving')
        old_pid = first['activePid']
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

        # Upgrade: the candidate cannot reach Nacos until the gate opens.
        gate.clear()
        previous_inode = installed.stat().st_ino
        began = time.monotonic()
        upgrade = subprocess.Popen([str(ctl_binary), 'upgrade', '--socket', str(boss_socket),
                                    '--binary', str(new_build('access-server-v2'))],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        preparing = wait_status(boss, boss_socket,
                                lambda value: value['phase'] == 'preparing' and value['candidatePid'] != 0)
        before_delay = len(samples)
        time.sleep(0.8)
        if control(boss_socket)['candidateState'] not in ('preparing', 'starting'):
            raise RuntimeError('candidate reported prepared before its initial Nacos responses')
        if len(samples) - before_delay < 3 or failures:
            raise RuntimeError('old generation stopped serving during delayed prewarm')
        requests_during_gate = len(samples) - before_delay
        backup = Path(f'{installed}.prev')
        if installed.stat().st_ino == previous_inode or backup.stat().st_ino != previous_inode:
            raise RuntimeError('upgrade did not replace the binary while keeping the previous one')
        gate.set()
        stdout, stderr = upgrade.communicate(timeout=40)
        elapsed = time.monotonic() - began
        if upgrade.returncode != 0:
            raise RuntimeError(f'upgrade command failed: {stdout.strip()} {stderr.strip()}')
        result = json.loads(stdout.strip().splitlines()[-1])
        new_pid = result['activePid']
        if result['phase'] != 'serving' or new_pid != preparing['candidatePid'] or result['drainingPid'] != old_pid:
            raise RuntimeError(f'unexpected upgrade result: {result}')
        if not alive(old_pid):
            raise RuntimeError('old generation exited before its outstanding request finished')
        if http3 and udp_peer.fileno() < 0:
            raise RuntimeError('new generation did not keep the requested UDP overlap policy')
        upgrade_failures = len(failures)
        assert request(traffic_port) == (200, b'ok')
        tls_checks()
        upstream_release.set()
        slow.join(timeout=5)
        if slow_result != [(200, b'old request completed')]:
            raise RuntimeError('outstanding old-generation request did not finish')
        wait_gone(old_pid, 10)
        wait_status(boss, boss_socket,
                    lambda value: value['drainingPid'] == 0 and value['activeRegistration'] == 'registered')
        if not any(path.name == 'access.log' for path in log_directory.iterdir()):
            raise RuntimeError('shared access-log directory was not written')

        # Abort while prewarming: the active generation and its binary stay.
        gate.clear()
        serving_inode = installed.stat().st_ino
        aborted_upgrade = subprocess.Popen([str(ctl_binary), 'upgrade', '--socket', str(boss_socket),
                                            '--binary', str(new_build('access-server-v3'))],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        wait_status(boss, boss_socket, lambda value: value['phase'] == 'preparing' and value['candidatePid'] != 0)
        aborted = ctl('abort')
        stdout, stderr = aborted_upgrade.communicate(timeout=20)
        gate.set()
        if aborted.returncode != 0 or aborted_upgrade.returncode == 0 or 'ABORTED' not in stderr:
            raise RuntimeError(f'abort did not cancel the prewarm: {aborted.stderr.strip()} {stderr.strip()}')
        if installed.stat().st_ino != serving_inode:
            raise RuntimeError('failed upgrade did not restore the serving binary')
        status = wait_status(boss, boss_socket, lambda value: value['candidatePid'] == 0)
        if status['activePid'] != new_pid or request(traffic_port) != (200, b'ok'):
            raise RuntimeError('aborting the prewarm affected the active generation')

        # Crash recovery: the boss cold-starts a replacement.
        failures_before_crash = len(failures)
        os.kill(new_pid, signal.SIGKILL)
        recovered = wait_status(boss, boss_socket,
                                lambda value: value['activeState'] == 'serving' and value['activePid'] != new_pid,
                                seconds=30)
        stop_probe.set()
        health.join(timeout=2)
        assert request(traffic_port) == (200, b'ok')

        # Orderly stop: every child exits before the boss does.
        recovered_pid = recovered['activePid']
        boss.send_signal(signal.SIGTERM)
        if boss.wait(timeout=20) != 0 or alive(recovered_pid) or boss_socket.exists():
            raise RuntimeError('boss did not stop its generations and remove its socket')

        # Boss crash: nothing keeps serving, a held request bounds the drain.
        boss = start_boss('boss-crash.log')
        crashed = wait_status(boss, boss_socket, lambda value: value['activeState'] == 'serving')

        def held_request():
            try:
                held_result.append(request(traffic_port, '/hold', timeout=20))
            except Exception as error:
                held_result.append(type(error).__name__)

        held = threading.Thread(target=held_request, daemon=True)
        held.start()
        if not hold_entered.wait(5):
            raise RuntimeError('held request did not reach the upstream')
        killed = time.monotonic()
        boss.kill()
        boss.wait(timeout=5)
        orphan_stop_seconds = wait_gone(crashed['activePid'], 10) - killed
        hold_release.set()
        held.join(timeout=5)
        if orphan_stop_seconds > 8:
            raise RuntimeError('orphaned generation outlived its bounded stop')
        if udp_peer is not None:
            udp_peer.close()
        print(json.dumps({
            'passed': True,
            'old_pid': old_pid,
            'new_pid': new_pid,
            'prewarm_gate_seconds': 0.8,
            'requests_during_gate': requests_during_gate,
            'upgrade_command_ms': round(elapsed * 1000, 3),
            'probe_attempts': len(samples),
            'probe_failures_through_upgrade': upgrade_failures,
            'probe_failures_after_upgrade_before_crash': failures_before_crash - upgrade_failures,
            'probe_failures_during_crash_recovery': len(failures) - failures_before_crash,
            'old_request_completed': True,
            'registration_recovered': True,
            'shared_access_log_directory': True,
            'binary_replaced_with_backup': True,
            'prewarm_abort_restored_binary': True,
            'crash_recovered_pid': recovered_pid,
            'orphan_stop_seconds': round(orphan_stop_seconds, 3),
            'tls_http1_and_http2_verified': tls,
            'http3_enabled': http3,
            'udp_peer_held_during_cutover': http3,
            'worker_affinity_limit': workers,
            'note': 'command duration includes prewarm and CLI polling; it is not a measured listener gap',
        }, indent=2))
    finally:
        stop_probe.set()
        gate.set()
        upstream_release.set()
        hold_release.set()
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
        for directory in control_directories:
            shutil.rmtree(directory, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--boss', type=Path, required=True)
    parser.add_argument('--ctl', type=Path, required=True)
    parser.add_argument('--rnacos', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--http3', action='store_true', help='enable HTTP/3 and retain a reusable UDP peer across handoff (also enables TLS)')
    parser.add_argument('--workers', type=int, choices=(1, 2), help='limit gateway CPU affinity to exercise single/multiple workers')
    parser.add_argument('--tls', action='store_true', help='also verify TLS HTTP/1.1 and HTTP/2 (requires openssl and HTTP/2 curl)')
    arguments = parser.parse_args()
    for binary in (arguments.server, arguments.boss, arguments.ctl, arguments.rnacos):
        if not binary.is_file():
            parser.error(f'binary does not exist: {binary}')
    output = arguments.output_dir.resolve() if arguments.output_dir else Path(tempfile.mkdtemp(prefix='access-upgrade-'))
    if arguments.output_dir:
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        run(arguments.server.resolve(), arguments.boss.resolve(), arguments.ctl.resolve(), arguments.rnacos.resolve(),
            output, arguments.tls or arguments.http3, arguments.http3, arguments.workers)
    except Exception as error:
        print(f'Upgrade integration failed: {error}; artifacts: {output}', file=sys.stderr)
        return 1
    if not arguments.output_dir:
        shutil.rmtree(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
