#!/usr/bin/env python3
"""Serveur de canal LOCAL pour les essais (http://127.0.0.1 seulement).

Sert un dossier publie avec Range (reprise) et des pannes a la demande :
  --cut NOM:OCTETS   coupe la connexion apres OCTETS la premiere fois que NOM est servi
  --corrupt NOM      sert NOM avec un octet modifie (empreinte fausse)
  --down             repond 503 a tout (serveur en panne)
Le client n'accepte http que si L4ZY_TEST_ALLOW_LOCAL_HTTP=1 : ce n'est
PAS une validation HTTPS, seulement la mecanique de mise a jour.
"""
import argparse, http.server, os, socketserver, sys, threading
from pathlib import Path

A = None
CUT_DONE = set()
LOG = []


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        LOG.append(fmt % args)
        sys.stdout.write((fmt % args) + "\n")
        sys.stdout.flush()

    def do_GET(self):
        if A.down:
            self.send_error(503)
            return
        rel = self.path.split("?")[0].lstrip("/")
        p = (Path(A.root) / rel).resolve()
        if not str(p).startswith(str(Path(A.root).resolve())) or not p.is_file():
            self.send_error(404)
            return
        data = p.read_bytes()
        name = p.name
        if name in A.corrupt:
            data = bytearray(data)
            data[len(data) // 2] ^= 0xFF
            data = bytes(data)
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[6:].split("-")[0] or 0)
            print(f"RANGE {name} depuis {start}", flush=True)
            if start >= len(data):
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
        else:
            self.send_response(200)
        body = data[start:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        cut = A.cuts.get(name)
        if cut is not None and name not in CUT_DONE:
            CUT_DONE.add(name)
            self.wfile.write(body[:cut])
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        self.wfile.write(body)


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = False


def main():
    global A
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--cut", action="append", default=[])
    ap.add_argument("--corrupt", action="append", default=[])
    ap.add_argument("--down", action="store_true")
    A = ap.parse_args()
    A.cuts = {c.split(":")[0]: int(c.split(":")[1]) for c in A.cut}
    srv = S(("127.0.0.1", A.port), H)
    print(f"serveur de canal local sur http://127.0.0.1:{A.port}/ -> {A.root}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
