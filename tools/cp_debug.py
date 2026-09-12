"""Debug helper used by CP IDE for Python solutions.

Usage: python -u cp_debug.py <port> <bp1,bp2,...|-> <script>

Runs <script> under bdb, talks to the IDE over a local TCP socket with
newline-delimited JSON. The program's own stdin/stdout are untouched, so the
solution reads the test input normally and its output reaches the Console pane.
"""
import bdb
import json
import os
import socket
import sys
import traceback


def main():
    port = int(sys.argv[1])
    bps = [int(x) for x in sys.argv[2].split(",") if x.strip().isdigit()]
    script = os.path.abspath(sys.argv[3])

    sock = socket.create_connection(("127.0.0.1", port))
    rfile = sock.makefile("r", encoding="utf-8")

    def send(obj):
        sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def recv():
        line = rfile.readline()
        if not line:
            return {"cmd": "stop"}
        try:
            return json.loads(line)
        except Exception:
            return {"cmd": "continue"}

    def fmt(v):
        try:
            r = repr(v)
        except Exception:
            r = "<unrepr>"
        if len(r) > 200:
            r = r[:197] + "..."
        return r

    class Dbg(bdb.Bdb):
        def __init__(self):
            super().__init__()
            self.first = True

        def in_script(self, frame):
            try:
                return os.path.abspath(frame.f_code.co_filename) == script
            except Exception:
                return False

        def user_line(self, frame):
            if self.first:
                # First stop is the module's first line: run to the first breakpoint
                # unless none are set, in which case pause here.
                self.first = False
                if bps:
                    self.set_continue()
                    return
            if not self.in_script(frame):
                # Stepped into library code: step until we are back in the solution.
                self.set_step()
                return
            self.report(frame)
            self.wait_cmd(frame)

        def user_return(self, frame, value):
            pass

        def user_exception(self, frame, exc_info):
            pass

        def report(self, frame):
            vars_ = []
            loc = frame.f_locals
            for k, v in list(loc.items()):
                if k.startswith("__") or callable(v) and not isinstance(v, type):
                    continue
                if type(v).__name__ == "module":
                    continue
                vars_.append({"k": k, "v": fmt(v)})
                if len(vars_) >= 60:
                    break
            stack = []
            f = frame
            while f is not None:
                if self.in_script(f):
                    name = f.f_code.co_name
                    stack.append({"fn": ("<module>" if name == "<module>" else name + "()"), "line": f.f_lineno})
                f = f.f_back
            send({"event": "paused", "line": frame.f_lineno, "vars": vars_, "stack": stack})

        def wait_cmd(self, frame):
            msg = recv()
            cmd = msg.get("cmd", "continue")
            if cmd == "continue":
                self.set_continue()
            elif cmd == "next":
                self.set_next(frame)
            elif cmd == "step":
                self.set_step()
            elif cmd == "out":
                self.set_return(frame)
            else:
                self.set_quit()

    dbg = Dbg()
    for b in bps:
        dbg.set_break(script, b)

    sys.argv = [script]
    sys.path[0] = os.path.dirname(script)
    with open(script, "rb") as f:
        src = f.read()
    code = compile(src, script, "exec")
    globs = {"__name__": "__main__", "__file__": script, "__builtins__": __builtins__}
    exit_code = 0
    try:
        dbg.run(code, globs)
    except bdb.BdbQuit:
        exit_code = -1
    except SystemExit as e:
        exit_code = e.code if isinstance(e.code, int) else 0
    except Exception:
        send({"event": "exception", "message": traceback.format_exc()})
        exit_code = 1
    try:
        sys.stdout.flush()
    except Exception:
        pass
    send({"event": "exited", "code": exit_code})
    sock.close()


if __name__ == "__main__":
    main()
