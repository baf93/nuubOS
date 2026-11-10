#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of nuubos-cast-session against fake receivers.

  python3 tests/cast.py serve-cast|serve-dlna   fake receiver on the LAN for
  a real console (serve-dlna also answers SSDP discovery); Ctrl+C prints
  what was received.

  NUUBOS_CAST_SESSION=/path/to/nuubos-cast-session \
  [OPENH264=/path/to/libopenh264.so] python3 tests/cast.py

The session captures a synthetic screen (NUUBOS_CAST_FAKE_SCREEN). The fake
receivers speak the real protocols and the received media is decoded with
ffmpeg/ffprobe:
  cast       CASTV2 over TLS, OFFER/ANSWER, Cast RTP decrypted (openssl
             CLI) and reassembled into VP8, RTCP feedback with ACKs, a
             NACKed packet that must be retransmitted, a picture loss that
             must bring a key frame, STOP on the local stop.
  fallback   no mirroring app: Default Media Receiver LOAD + live WebM.
  dlna       description + SOAP, the renderer fetches MPEG-TS (MPEG-2/MP2).
  mice       MS-MICE SOURCE_READY, WFD RTSP M1-M7, RTP MPEG-TS with H.264
             (needs OPENH264, Cisco's binary for the host).
"""
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SESSION = os.environ.get("NUUBOS_CAST_SESSION", "nuubos-cast-session")
OPENH264 = os.environ.get("OPENH264", "")
TMP = tempfile.mkdtemp(prefix="nuubos-cast-test-")
FAILS = []


def check(cond, what):
    print(("[PASS] " if cond else "[FAIL] ") + what)
    if not cond:
        FAILS.append(what)


def run_session(args, stop_after=None, env_extra=None):
    env = dict(os.environ, NUUBOS_CAST_FAKE_SCREEN="640x480")
    env.update(env_extra or {})
    p = subprocess.Popen([SESSION] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=open(os.path.join(TMP, args[0] + ".log"), "w"), env=env, text=True)
    lines = []

    def reader():
        for line in p.stdout:
            lines.append(line.rstrip("\n"))
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    if stop_after is not None:
        time.sleep(stop_after)
        try:
            p.stdin.write("STOP\n")
            p.stdin.flush()
        except BrokenPipeError:
            pass
    try:
        p.wait(timeout=40)
    except subprocess.TimeoutExpired:
        p.kill()
        lines.append("@timeout")
    t.join(2)
    return lines


def ffprobe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,width,height",
                          "-of", "json", path], capture_output=True, text=True).stdout
    try:
        return json.loads(out).get("streams", [])
    except ValueError:
        return []


def decoded_frames(path, fmt=None):
    r = subprocess.run(["ffprobe", "-v", "error"] + (["-f", fmt] if fmt else []) +
                       ["-count_frames", "-select_streams", "v:0", "-show_entries",
                        "stream=nb_read_frames", "-of", "csv=p=0", path], capture_output=True, text=True)
    try:
        return int(r.stdout.strip().split("\n")[0].strip(","))
    except ValueError:
        return 0


# --------------------------------------------------------------------------
# Google Cast
# --------------------------------------------------------------------------

def varint(v):
    out = b""
    while True:
        b = v & 0x7F
        v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v:
            return out


def pb_string(field, s):
    s = s.encode()
    return bytes([field << 3 | 2]) + varint(len(s)) + s


def cast_message(source, dest, ns, payload):
    body = b"\x08\x00" + pb_string(2, source) + pb_string(3, dest) + pb_string(4, ns) + b"\x28\x00" + \
        pb_string(6, json.dumps(payload))
    return struct.pack(">I", len(body)) + body


def parse_cast(data):
    p, fields = 0, {}
    while p < len(data):
        key = data[p]
        p += 1
        field, wire = key >> 3, key & 7
        if wire == 0:
            while data[p] & 0x80:
                p += 1
            p += 1
        elif wire == 2:
            ln, shift = 0, 0
            while True:
                b = data[p]
                p += 1
                ln |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
            fields[field] = data[p:p + ln].decode()
            p += ln
    return fields.get(2), fields.get(3), fields.get(4), json.loads(fields.get(6, "{}"))


def tls_server(bind="127.0.0.1"):
    cert = os.path.join(TMP, "cert.pem")
    key = os.path.join(TMP, "key.pem")
    if not os.path.exists(cert):
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert,
                        "-days", "1", "-subj", "/CN=fake-cast"], capture_output=True, check=True)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, 0))
    srv.listen(1)
    return srv, ctx


def aes_ctr(key, nonce, data):
    r = subprocess.run(["openssl", "enc", "-aes-128-ctr", "-K", key.hex(), "-iv", nonce.hex(), "-nosalt"],
                       input=data, capture_output=True)
    return r.stdout


class FakeCast:
    def __init__(self, mirroring=True, bind="127.0.0.1"):
        self.mirroring = mirroring
        self.srv, self.ctx = tls_server(bind)
        self.port = self.srv.getsockname()[1]
        self.events = []
        self.frames = {}        # frame_id -> {pid: payload}
        self.frame_meta = {}    # frame_id -> (max_pid, key)
        self.complete = []
        self.retransmitted = False
        self.dropped = None
        self.pli_sent_at = None
        self.key_after_pli = False
        self.load_url = None
        self.stop_session = None
        threading.Thread(target=self.serve, daemon=True).start()

    def send(self, conn, source, dest, ns, payload):
        conn.sendall(cast_message(source, dest, ns, payload))

    def serve(self):
        raw, _ = self.srv.accept()
        conn = self.ctx.wrap_socket(raw, server_side=True)
        buf = b""
        while True:
            try:
                chunk = conn.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while len(buf) >= 4 and len(buf) >= 4 + struct.unpack(">I", buf[:4])[0]:
                n = struct.unpack(">I", buf[:4])[0]
                src, dst, ns, msg = parse_cast(buf[4:4 + n])
                buf = buf[4 + n:]
                self.handle(conn, src, dst, ns, msg)

    def handle(self, conn, src, dst, ns, msg):
        t = msg.get("type")
        self.events.append((ns.split(".")[-1], t))
        if t == "LAUNCH":
            app = msg["appId"]
            if app == "0F5096E8" and not self.mirroring:
                self.send(conn, "receiver-0", src, ns, {"type": "LAUNCH_ERROR", "requestId": msg["requestId"],
                                                        "reason": "NOT_FOUND"})
                return
            self.send(conn, "receiver-0", "*", ns, {
                "type": "RECEIVER_STATUS", "requestId": msg["requestId"],
                "status": {"applications": [{"appId": app, "sessionId": "S-" + app, "transportId": "T-" + app,
                                             "displayName": "fake"}]}})
        elif t == "OFFER":
            self.offer = msg["offer"]
            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.udp.bind((self.srv.getsockname()[0], 0))
            streams = self.offer["supportedStreams"]
            self.streams = {s["ssrc"]: s for s in streams}
            self.recv_ssrc = {s["ssrc"]: 900000 + s["index"] for s in streams}
            self.send(conn, dst, src, ns, {"type": "ANSWER", "seqNum": msg["seqNum"], "result": "ok", "answer": {
                "udpPort": self.udp.getsockname()[1], "sendIndexes": [s["index"] for s in streams],
                "ssrcs": [900000 + s["index"] for s in streams]}})
            threading.Thread(target=self.media, daemon=True).start()
        elif t == "LOAD":
            self.load_url = msg["media"]["contentId"]
            self.send(conn, dst, src, ns, {"type": "MEDIA_STATUS", "requestId": msg["requestId"], "status": [
                {"mediaSessionId": 1, "playerState": "BUFFERING"}]})
        elif t == "STOP":
            self.stop_session = msg.get("sessionId")
        elif t == "PING":
            self.send(conn, dst, src, ns, {"type": "PONG"})

    def feedback(self, sender_ssrc, receiver_ssrc, checkpoint, nacks=(), pli=False):
        rr = struct.pack(">BBHI", 0x80, 201, 1, receiver_ssrc)
        fb = struct.pack(">IIIBBH", receiver_ssrc, sender_ssrc, 0x43415354, checkpoint & 0xFF, len(nacks), 250)
        for fid, pid in nacks:
            fb += struct.pack(">BHB", fid & 0xFF, pid, 0)
        pkt = rr + struct.pack(">BBH", 0x80 | 15, 206, len(fb) // 4) + fb
        if pli:
            pkt += struct.pack(">BBHII", 0x80 | 1, 206, 2, receiver_ssrc, sender_ssrc)
        self.udp.sendto(pkt, self.peer)

    def media(self):
        video = [s for s in self.streams.values() if s["type"] == "video_source"][0]
        vssrc = video["ssrc"]
        key = bytes.fromhex(video["aesKey"])
        mask = bytes.fromhex(video["aesIvMask"])
        ivf = open(os.path.join(TMP, "cast.ivf"), "wb")
        w, h = video["resolutions"][0]["width"], video["resolutions"][0]["height"]
        ivf.write(b"DKIF" + struct.pack("<HHIHHIIII", 0, 32, 0x30385056, w, h, 30, 1, 0, 0))
        checkpoint = -1
        self.udp.settimeout(0.5)
        nacked = False
        while True:
            try:
                data, self.peer = self.udp.recvfrom(2000)
            except OSError:
                return
            if data[1] >= 200 and data[1] <= 207:
                continue  # sender report
            ssrc = struct.unpack(">I", data[8:12])[0]
            if ssrc != vssrc:
                continue
            flags, fid8, pid, maxpid, ref8 = struct.unpack(">BBHHB", data[12:19])
            ext = flags & 0x3F
            payload = data[19 + 4 * ext:]
            fid = checkpoint + 1 + ((fid8 - (checkpoint + 1)) & 0xFF)
            if fid >= 3 and pid == 0 and maxpid > 0 and self.dropped is None:
                self.dropped = (fid, pid)   # lose it once: the NACK must recover it
                continue
            if self.dropped == (fid, pid) and fid in self.frames:
                self.retransmitted = True
            self.frames.setdefault(fid, {})[pid] = payload
            self.frame_meta[fid] = (maxpid, bool(flags & 0x80))
            if self.pli_sent_at is not None and flags & 0x80 and fid > self.pli_sent_at:
                self.key_after_pli = True
            # complete frames in order
            while checkpoint + 1 in self.frames and len(self.frames[checkpoint + 1]) == \
                    self.frame_meta[checkpoint + 1][0] + 1:
                f = checkpoint + 1
                enc = b"".join(self.frames[f][i] for i in range(self.frame_meta[f][0] + 1))
                nonce = bytearray(16)
                nonce[8:12] = struct.pack(">I", f & 0xFFFFFFFF)
                nonce = bytes(a ^ b for a, b in zip(nonce, mask))
                frame = aes_ctr(key, nonce, enc)
                ivf.write(struct.pack("<IQ", len(frame), f) + frame)
                ivf.flush()
                checkpoint = f
                self.complete.append(f)
            nacks = []
            if self.dropped and self.dropped[0] > checkpoint and not nacked and \
                    max(self.frames) > self.dropped[0]:
                nacks = [self.dropped]
                nacked = True
            pli = len(self.complete) == 40 and self.pli_sent_at is None
            if pli:
                self.pli_sent_at = checkpoint
            self.feedback(vssrc, self.recv_ssrc[vssrc], checkpoint, nacks, pli)


def test_cast():
    rx = FakeCast()
    lines = run_session(["cast", "127.0.0.1", str(rx.port), "Fake TV", "--latency", "300"], stop_after=4)
    check("@state casting" in lines, "cast: session reaches casting")
    check("@info delay=low" in lines, "cast: mirroring (low delay) negotiated")
    check(("receiver", "LAUNCH") in rx.events and ("webrtc", "OFFER") in rx.events, "cast: LAUNCH and OFFER sent")
    check(rx.stop_session == "S-0F5096E8", "cast: STOP with the session id on the local stop")
    check(len(rx.complete) >= 60, "cast: %d frames received complete" % len(rx.complete))
    check(rx.retransmitted, "cast: NACKed packet retransmitted")
    check(rx.key_after_pli, "cast: key frame after picture loss")
    n = decoded_frames(os.path.join(TMP, "cast.ivf"), "ivf")
    check(n >= 60, "cast: %d decrypted VP8 frames decode" % n)


def test_fallback():
    rx = FakeCast(mirroring=False)
    got = {}

    def fetch():
        for _ in range(100):
            if rx.load_url:
                break
            time.sleep(0.05)
        if not rx.load_url:
            return
        data = b""
        try:
            with urllib.request.urlopen(rx.load_url, timeout=5) as r:
                got["type"] = r.headers.get("Content-Type")
                end = time.time() + 3
                while time.time() < end:
                    data += r.read(4096)
        except OSError:
            pass
        open(os.path.join(TMP, "fallback.webm"), "wb").write(data)
    t = threading.Thread(target=fetch)
    t.start()
    lines = run_session(["cast", "127.0.0.1", str(rx.port), "Old TV"], stop_after=5)
    t.join()
    check("@info fallback=1" in lines and "@info delay=high" in lines, "fallback: Default Media Receiver used")
    check(("receiver", "LAUNCH") in rx.events and ("media", "LOAD") in rx.events, "fallback: LOAD sent")
    check(got.get("type") == "video/webm", "fallback: live stream is video/webm")
    streams = ffprobe(os.path.join(TMP, "fallback.webm"))
    check(any(s.get("codec_name") == "vp8" for s in streams), "fallback: WebM carries VP8")
    check(decoded_frames(os.path.join(TMP, "fallback.webm")) >= 30, "fallback: frames decode")


# --------------------------------------------------------------------------
# DLNA
# --------------------------------------------------------------------------

DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
<friendlyName>Fake &amp; TV</friendlyName><modelName>FakeRenderer</modelName>
<serviceList>
<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
<controlURL>/rc</controlURL></service>
<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
<controlURL>ctl/avt</controlURL></service>
</serviceList></device></root>"""


def test_dlna():
    actions = []
    state = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            body = DESCRIPTION.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"])).decode()
            action = self.headers.get("SOAPAction", "").strip('"').split("#")[-1]
            actions.append((self.path, action))
            if action == "SetAVTransportURI":
                state["uri"] = body.split("<CurrentURI>")[1].split("</CurrentURI>")[0]
                state["meta"] = "object.item.videoItem" in body
            if action == "Play":
                threading.Thread(target=fetch, daemon=True).start()
            resp = b'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">' \
                   b'<s:Body/></s:Envelope>'
            self.send_response(200)
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

    def fetch():
        data = b""
        try:
            with urllib.request.urlopen(state["uri"], timeout=5) as r:
                state["type"] = r.headers.get("Content-Type")
                state["dlna"] = r.headers.get("transferMode.dlna.org")
                end = time.time() + 3
                while time.time() < end:
                    data += r.read(4096)
        except OSError:
            pass
        open(os.path.join(TMP, "dlna.ts"), "wb").write(data)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    loc = "http://127.0.0.1:%d/desc.xml" % srv.server_address[1]
    lines = run_session(["dlna", loc, "Fake TV"], stop_after=5)
    srv.shutdown()
    check(("/ctl/avt", "SetAVTransportURI") in actions and ("/ctl/avt", "Play") in actions,
          "dlna: SetAVTransportURI + Play on the resolved control URL")
    check(state.get("meta"), "dlna: DIDL-Lite metadata sent")
    check(actions and actions[-1][1] == "Stop", "dlna: Stop on the local stop")
    check("@state casting" in lines, "dlna: casting once the renderer fetched the stream")
    check(state.get("type") == "video/mpeg" and state.get("dlna") == "Streaming", "dlna: HTTP headers")
    streams = ffprobe(os.path.join(TMP, "dlna.ts"))
    check(any(s.get("codec_name") == "mpeg2video" for s in streams), "dlna: MPEG-2 video in the TS")
    check(decoded_frames(os.path.join(TMP, "dlna.ts")) >= 30, "dlna: frames decode")


# --------------------------------------------------------------------------
# Miracast over Infrastructure
# --------------------------------------------------------------------------

def test_mice():
    if not OPENH264:
        print("[SKIP] mice: OPENH264 not set")
        return
    info = {}
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("127.0.0.1", 0))
    rtp.settimeout(0.5)
    rtp_port = rtp.getsockname()[1]
    mice = socket.socket()
    mice.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mice.bind(("127.0.0.1", 7250))
    mice.listen(1)

    def sink():
        conn, _ = mice.accept()
        msg = conn.recv(1024)
        info["version"], info["command"] = msg[2], msg[3]
        p = 4
        while p < len(msg):
            t, ln = msg[p], struct.unpack(">H", msg[p + 1:p + 3])[0]
            if t == 2:
                info["rtsp_port"] = struct.unpack(">H", msg[p + 3:p + 5])[0]
            p += 3 + ln
        r = socket.create_connection(("127.0.0.1", info["rtsp_port"]))
        f = r.makefile("rb")
        cseq = [100]  # noqa

        def read_msg():
            head = b""
            while not head.endswith(b"\r\n\r\n"):
                c = f.read(1)
                if not c:
                    return None, ""
                head += c
            text = head.decode()
            ln = 0
            for line in text.split("\r\n"):
                if line.lower().startswith("content-length:"):
                    ln = int(line.split(":")[1])
            return text, f.read(ln).decode() if ln else ""

        def reply(text, body=""):
            seq = [ln for ln in text.split("\r\n") if ln.startswith("CSeq")][0]
            out = "RTSP/1.0 200 OK\r\n%s\r\n" % seq
            if body:
                out += "Content-Type: text/parameters\r\nContent-Length: %d\r\n" % len(body)
            r.sendall((out + "\r\n" + body).encode())

        def request(line, extra=""):
            cseq[0] += 1
            r.sendall(("%s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n" % (line, cseq[0], extra)).encode())
            return read_msg()

        text, _ = read_msg()           # M1
        info["m1"] = text.startswith("OPTIONS")
        reply(text)
        resp, _ = request("OPTIONS *", "Require: org.wfa.wfd1.0\r\n")  # M2
        info["m2"] = "org.wfa.wfd1.0" in resp
        text, body = read_msg()        # M3
        reply(text, "wfd_video_formats: 00 00 01 01 0001FFFF 1FFFFFFF 00000FFF 00 0000 0000 00 none none\r\n"
                    "wfd_audio_codecs: AAC 00000001 00\r\n"
                    "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d 0 mode=play\r\n" % rtp_port)
        text, body = read_msg()        # M4
        info["m4"] = body
        reply(text)
        text, body = read_msg()        # M5
        info["m5"] = "SETUP" in body
        reply(text)
        resp, _ = request("SETUP rtsp://127.0.0.1/wfd1.0/streamid=0",
                          "Transport: RTP/AVP/UDP;unicast;client_port=%d\r\n" % rtp_port)
        session = [ln for ln in resp.split("\r\n") if ln.startswith("Session")][0].split(":")[1].split(";")[0].strip()
        resp, _ = request("PLAY rtsp://127.0.0.1/wfd1.0/streamid=0", "Session: %s\r\n" % session)
        info["play"] = resp.startswith("RTSP/1.0 200")
        ts = b""
        end = time.time() + 3
        while time.time() < end:
            try:
                data = rtp.recv(2000)
            except OSError:
                continue
            if data[1] & 0x7F == 33:
                ts += data[12:]
        open(os.path.join(TMP, "mice.ts"), "wb").write(ts)
        text, body = read_msg()        # teardown trigger on stop
        info["teardown"] = "TEARDOWN" in body
        reply(text)
        conn.settimeout(3)
        stop = conn.recv(64)
        info["stop_projection"] = len(stop) > 3 and stop[3] == 2

    t = threading.Thread(target=sink, daemon=True)
    t.start()
    lines = run_session(["mice", "127.0.0.1", "Fake PC", "--openh264", OPENH264], stop_after=5)
    t.join(10)
    check(info.get("command") == 1 and info.get("rtsp_port"), "mice: SOURCE_READY with the RTSP port")
    check(info.get("m1") and info.get("m2") and info.get("m5"), "mice: M1, M2, M5")
    check("00000001" in info.get("m4", "") and "AAC" not in info.get("m4", ""),
          "mice: M4 picks CBP 640x480p60 (no audio without --audio)")
    check(info.get("play") and "@state casting" in lines, "mice: PLAY starts the stream")
    streams = ffprobe(os.path.join(TMP, "mice.ts"))
    check(any(s.get("codec_name") == "h264" for s in streams), "mice: H.264 in the RTP transport stream")
    check(decoded_frames(os.path.join(TMP, "mice.ts")) >= 30, "mice: frames decode")
    check(info.get("teardown") and info.get("stop_projection"), "mice: TEARDOWN trigger + STOP_PROJECTION")


def console_url(url):
    """Through an SSH tunnel the console names itself 127.0.0.1: fetch from
    its LAN address instead (CONSOLE_IP)."""
    ip = os.environ.get("CONSOLE_IP")
    return url.replace("127.0.0.1", ip) if ip else url


def serve_cast(mirroring=True):
    rx = FakeCast(mirroring=mirroring, bind="0.0.0.0")

    def fetch():
        while not rx.load_url:
            time.sleep(0.1)
        path = os.path.join(TMP, "serve-cast.webm")
        print("LOAD", rx.load_url, flush=True)
        with urllib.request.urlopen(console_url(rx.load_url), timeout=10) as r, open(path, "wb") as out:
            end = time.time() + float(os.environ.get("FETCH_SECONDS", "20"))
            while time.time() < end:
                data = r.read(65536)
                if not data:
                    break
                out.write(data)
        print("webm:", ffprobe(path), "frames", decoded_frames(path), flush=True)
    if not mirroring:
        threading.Thread(target=fetch, daemon=True).start()
    print("fake Google Cast receiver: port %d (console: nuubos-cast-session cast <host ip> %d Fake --audio)"
          % (rx.port, rx.port), flush=True)
    try:
        while True:
            time.sleep(5)
            print("frames complete %d, retransmitted %s, events %s" % (len(rx.complete), rx.retransmitted,
                                                                      rx.events[-3:]), flush=True)
    except KeyboardInterrupt:
        pass
    print("decoded VP8 frames:", decoded_frames(os.path.join(TMP, "cast.ivf"), "ivf"), os.path.join(TMP, "cast.ivf"))


def serve_dlna():
    """Renderer + SSDP responder: the console finds it with DISCOVER."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect(("192.168.5.1", 9))
    host = probe.getsockname()[0]
    state = {"bytes": 0}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            body = DESCRIPTION.replace("Fake &amp; TV", "nuubOS test renderer").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"])).decode()
            action = self.headers.get("SOAPAction", "").strip('"').split("#")[-1]
            print("SOAP", action, flush=True)
            if action == "SetAVTransportURI":
                state["uri"] = body.split("<CurrentURI>")[1].split("</CurrentURI>")[0]
            if action == "Play":
                threading.Thread(target=fetch, daemon=True).start()
            if action == "Stop":
                state["stop"] = True
            resp = b'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">' \
                   b'<s:Body/></s:Envelope>'
            self.send_response(200)
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

    def fetch():
        state["stop"] = False
        path = os.path.join(TMP, "serve-dlna.ts")
        with urllib.request.urlopen(console_url(state["uri"]), timeout=10) as r, open(path, "wb") as out:
            while not state.get("stop"):
                data = r.read(65536)
                if not data:
                    break
                out.write(data)
                state["bytes"] += len(data)
        print("stream closed:", ffprobe(path), "frames", decoded_frames(path), flush=True)

    srv = ThreadingHTTPServer(("0.0.0.0", int(os.environ.get("DLNA_PORT", "0"))), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    location = "http://%s:%d/desc.xml" % (os.environ.get("ADVERTISE_HOST", host), srv.server_address[1])
    ssdp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ssdp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ssdp.bind(("", 1900))
    ssdp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton("239.255.255.250") +
                    socket.inet_aton(host))
    print("fake DLNA renderer at", location, flush=True)
    while True:
        data, peer = ssdp.recvfrom(4096)
        if data.startswith(b"M-SEARCH") and b"MediaRenderer" in data:
            ssdp.sendto(("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=60\r\nLOCATION: %s\r\n"
                         "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                         "USN: uuid:nuubos-test::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n"
                         % location).encode(), peer)
            print("answered M-SEARCH from", peer[0], "bytes received so far", state["bytes"], flush=True)


if __name__ == "__main__":
    if sys.argv[1:2] == ["serve-cast"]:
        serve_cast(mirroring="--no-mirroring" not in sys.argv)
        sys.exit(0)
    if sys.argv[1:] == ["serve-dlna"]:
        serve_dlna()
        sys.exit(0)
    only = sys.argv[1:]
    for name, fn in [("cast", test_cast), ("fallback", test_fallback), ("dlna", test_dlna), ("mice", test_mice)]:
        if not only or name in only:
            fn()
    print("logs in", TMP)
    if FAILS:
        print("[FAIL] %d checks failed" % len(FAILS))
        sys.exit(1)
    print("[PASS] all checks")
