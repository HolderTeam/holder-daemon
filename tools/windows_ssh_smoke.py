import os, sys, socket, threading, subprocess, tempfile, pathlib, ctypes, struct, shutil, logging, stat

# Requires Python 3.12+ and paramiko. Runs only against a disposable localhost remote.
# Usage: python tools/windows_ssh_smoke.py <holder_core_tests.exe> <file|ed25519|agent> [git.exe]
import paramiko

if os.name != "nt":
    raise SystemExit("This fixture requires Windows named pipes")
if len(sys.argv) < 3 or sys.argv[2] not in ("file", "ed25519", "agent"):
    raise SystemExit(
        "Usage: windows_ssh_smoke.py <holder_core_tests.exe> <file|ed25519|agent> [git.exe]"
    )

git = sys.argv[3] if len(sys.argv) > 3 else shutil.which("git")
if not git:
    raise SystemExit("Provide a git executable as the third argument")
logging.getLogger("paramiko.transport").setLevel(logging.CRITICAL)
root = pathlib.Path(tempfile.mkdtemp(prefix="holder-ssh-smoke-"))
ssh = root / ".ssh"
ssh.mkdir()
host = paramiko.RSAKey.generate(2048)
identity = paramiko.RSAKey.generate(2048)
key_name = "id_rsa"
if sys.argv[2] == "ed25519":
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization

    key_name = "id_ed25519"
    key = Ed25519PrivateKey.generate()
    (ssh / key_name).write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.OpenSSH,
            serialization.NoEncryption(),
        )
    )
    identity = paramiko.Ed25519Key.from_private_key_file(str(ssh / key_name))
else:
    identity.write_private_key_file(str(ssh / key_name))
(ssh / (key_name + ".pub")).write_text(
    identity.get_name() + " " + identity.get_base64() + "\n"
)
repo = root / "remote.git"
subprocess.run(
    [git, "init", "--bare", "--initial-branch=ssh-smoke", str(repo)],
    check=True,
    capture_output=True,
)
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(8)
port = listener.getsockname()[1]
(ssh / "known_hosts").write_text(
    f"[127.0.0.1]:{port} {host.get_name()} {host.get_base64()}\n"
)
accepted = []


class Server(paramiko.ServerInterface):
    def __init__(self):
        self.command = None
        self.ready = threading.Event()

    def get_allowed_auths(self, username):
        return "publickey"

    def check_auth_publickey(self, username, key):
        if username == "git" and key == identity:
            accepted.append(key.get_name())
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, channel_id):
        return (
            paramiko.OPEN_SUCCEEDED
            if kind == "session"
            else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
        )

    def check_channel_exec_request(self, channel, command):
        operation = command.decode().split(" ", 1)[0]
        if operation not in ("git-upload-pack", "git-receive-pack"):
            return False
        self.command = operation
        self.ready.set()
        return True


def serve_connection(client):
    transport = paramiko.Transport(client)
    transport.add_server_key(host)
    server = Server()
    try:
        transport.start_server(server=server)
        channel = transport.accept(15)
        if channel is None or not server.ready.wait(15):
            return
        process = subprocess.Popen(
            [git, server.command.removeprefix("git-"), str(repo)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        def receive():
            try:
                while data := channel.recv(32768):
                    process.stdin.write(data)
                    process.stdin.flush()
            except (OSError, EOFError):
                pass
            finally:
                try:
                    process.stdin.close()
                except OSError:
                    pass

        threading.Thread(target=receive, daemon=True).start()
        while data := process.stdout.read1(32768):
            channel.sendall(data)
        process.wait(timeout=15)
        channel.send_exit_status(process.returncode)
        channel.shutdown_write()
        channel.close()
    except EOFError:
        pass
    except Exception as error:
        print("server:", repr(error), flush=True)
    finally:
        transport.close()


def accept():
    while True:
        client, _ = listener.accept()
        threading.Thread(target=serve_connection, args=(client,), daemon=True).start()


threading.Thread(target=accept, daemon=True).start()

# A disposable Windows named-pipe agent exercises libssh2's actual OpenSSH
# agent protocol without changing the user's service or loading their keys.
pipe = r"\\.\pipe\holder-ssh-smoke-" + str(os.getpid())
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateNamedPipeW.restype = ctypes.c_void_p
kernel.CreateNamedPipeW.argtypes = [
    ctypes.c_wchar_p,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_void_p,
]
for name in (
    "ConnectNamedPipe",
    "ReadFile",
    "WriteFile",
    "DisconnectNamedPipe",
    "CloseHandle",
):
    getattr(kernel, name).argtypes = {
        "ConnectNamedPipe": [ctypes.c_void_p, ctypes.c_void_p],
        "ReadFile": [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ],
        "WriteFile": [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ],
        "DisconnectNamedPipe": [ctypes.c_void_p],
        "CloseHandle": [ctypes.c_void_p],
    }[name]
agent_ready = threading.Event()
signs = []


def agent():
    while True:
        handle = kernel.CreateNamedPipeW(pipe, 3, 0, 255, 65536, 65536, 0, None)
        agent_ready.set()
        kernel.ConnectNamedPipe(handle, None)

        def read(n):
            result = b""
            while len(result) < n:
                buf = ctypes.create_string_buffer(n - len(result))
                count = ctypes.c_uint32()
                if (
                    not kernel.ReadFile(
                        handle, buf, len(buf), ctypes.byref(count), None
                    )
                    or not count.value
                ):
                    raise EOFError()
                result += buf.raw[: count.value]
            return result

        try:
            while True:
                message = paramiko.Message(read(struct.unpack(">I", read(4))[0]))
                kind = message.get_byte()
                reply = paramiko.Message()
                if kind == b"\x0b":
                    reply.add_byte(b"\x0c")
                    reply.add_int(1)
                    reply.add_string(identity.asbytes())
                    reply.add_string("smoke")
                elif kind == b"\x0d":
                    message.get_string()
                    data = message.get_string()
                    flags = message.get_int()
                    algorithm = (
                        "rsa-sha2-512"
                        if flags & 4
                        else "rsa-sha2-256" if flags & 2 else "ssh-rsa"
                    )
                    reply.add_byte(b"\x0e")
                    reply.add_string(identity.sign_ssh_data(data, algorithm).asbytes())
                    signs.append(algorithm)
                else:
                    reply.add_byte(b"\x05")
                payload = reply.asbytes()
                packet = struct.pack(">I", len(payload)) + payload
                count = ctypes.c_uint32()
                kernel.WriteFile(handle, packet, len(packet), ctypes.byref(count), None)
        except EOFError:
            pass
        finally:
            kernel.DisconnectNamedPipe(handle)
            kernel.CloseHandle(handle)


threading.Thread(target=agent, daemon=True).start()
agent_ready.wait(5)
env = os.environ.copy()
env["HOLDER_TEST_SSH_REMOTE_URL"] = f"ssh://git@127.0.0.1:{port}/smoke.git"
env["USERPROFILE"] = str(root)
env["HOMEDRIVE"] = root.drive
env["HOMEPATH"] = str(root)[len(root.drive) :]
env.pop("HOME", None)
env["SSH_AUTH_SOCK"] = pipe if sys.argv[2] == "agent" else pipe + "-missing"
env["GIT_CONFIG_NOSYSTEM"] = "1"
if sys.argv[2] == "agent":
    (ssh / "id_rsa").unlink()
    (ssh / "id_rsa.pub").unlink()
print("mode:", sys.argv[2], "temporary fixture:", root, flush=True)
result = subprocess.run([sys.argv[1], "[.ssh-smoke]"], env=env, timeout=90)
print("accepted keys:", accepted, "agent signatures:", signs, flush=True)
code = result.returncode
if sys.argv[2] == "agent" and not signs:
    code = 1


def remove_readonly(function, path, error):
    os.chmod(path, stat.S_IWRITE)
    function(path)


shutil.rmtree(root, onexc=remove_readonly)
sys.exit(code)
