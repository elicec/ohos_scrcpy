import socket
import struct
import threading
import time

HOST = '127.0.0.1'
VIDEO_PORT = 9901
CONTROL_PORT = 9902

HANDSHAKE_MAGIC = 0x4F534352
PROTOCOL_VERSION = 1

MSG_VIDEO_SPS_PPS = 0x01
MSG_VIDEO_IDR_FRAME = 0x02
MSG_VIDEO_P_FRAME = 0x03
MSG_VIDEO_CONFIG = 0x04
MSG_VIDEO_RAW_RGBA = 0x05

MSG_CTRL_TOUCH = 0x10
MSG_CTRL_KEY = 0x20
MSG_CTRL_SCROLL = 0x30
MSG_CTRL_BACK = 0x40
MSG_CTRL_HOME = 0x41
MSG_CTRL_POWER = 0x42
MSG_CTRL_VOLUME_UP = 0x43
MSG_CTRL_VOLUME_DOWN = 0x44
MSG_CTRL_HEARTBEAT = 0xFF

CTRL_TOUCH_DOWN = 0
CTRL_TOUCH_UP = 1
CTRL_TOUCH_MOVE = 2

video_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
video_sock.connect((HOST, VIDEO_PORT))
print('video connected')

def video_reader():
    while True:
        header = video_sock.recv(13)
        if len(header) < 13:
            break
        msg_type, length, timestamp = struct.unpack('<BIQ', header)
        remaining = length
        while remaining > 0:
            chunk = video_sock.recv(min(65536, remaining))
            if not chunk:
                return
            remaining -= len(chunk)

video_thread = threading.Thread(target=video_reader, daemon=True)
video_thread.start()

ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ctrl_sock.connect((HOST, CONTROL_PORT))
print('control connected')

# handshake
hs_req = struct.pack('<IIHH', HANDSHAKE_MAGIC, PROTOCOL_VERSION, VIDEO_PORT, CONTROL_PORT)
ctrl_sock.sendall(hs_req)
hs_resp = ctrl_sock.recv(20)
print('handshake resp:', hs_resp.hex())

if len(hs_resp) >= 20:
    magic, version, result, vp, cp, sw, sh = struct.unpack('<IIiHHHH', hs_resp[:20])
    print(f'screen: {sw}x{sh}, result={result}')

def send_touch(action, x, y):
    header = struct.pack('<BBH', MSG_CTRL_TOUCH, 0, 20)
    event = struct.pack('<BBHIIQ', action, 0, 0, x, y, 0)
    ctrl_sock.sendall(header + event)
    print(f'sent touch action={action} x={x} y={y}')

def send_key(key_type):
    ctrl_sock.sendall(struct.pack('<BBH', key_type, 0, 0))
    print(f'sent key 0x{key_type:02X}')

time.sleep(1)

# tap center
send_touch(CTRL_TOUCH_DOWN, 540, 1204)
time.sleep(0.1)
send_touch(CTRL_TOUCH_UP, 540, 1204)
time.sleep(1)

# swipe down
send_touch(CTRL_TOUCH_DOWN, 540, 600)
time.sleep(0.05)
send_touch(CTRL_TOUCH_MOVE, 540, 1200)
time.sleep(0.05)
send_touch(CTRL_TOUCH_MOVE, 540, 1800)
time.sleep(0.05)
send_touch(CTRL_TOUCH_UP, 540, 1800)
time.sleep(1)

# home key
send_key(MSG_CTRL_HOME)
time.sleep(1)

ctrl_sock.close()
video_sock.close()
print('done')
