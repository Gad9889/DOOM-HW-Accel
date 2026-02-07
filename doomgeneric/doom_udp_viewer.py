import socket
import struct
import pygame
import sys
import argparse

# Stream protocol
HELLO_MAGIC = b"DGv1"
HELLO_SIZE = 9

# Network Config
SERVER_IP = "192.168.137.2"
SERVER_PORT = 5000

KEY_MAP = {
    pygame.K_LEFT: 0xac,
    pygame.K_RIGHT: 0xae,
    pygame.K_UP: 0xad,
    pygame.K_DOWN: 0xaf,
    pygame.K_w: 0xad,
    pygame.K_s: 0xaf,
    pygame.K_a: 0xac,
    pygame.K_d: 0xae,
    pygame.K_q: 0xa0,
    pygame.K_e: 0xa1,
    pygame.K_SPACE: 0xa2,
    pygame.K_f: 0xa3,
    pygame.K_LCTRL: 0xa3,
    pygame.K_RCTRL: 0xa3,
    pygame.K_LSHIFT: (0x80+0x36),
    pygame.K_RSHIFT: (0x80+0x36),
    pygame.K_RETURN: 13,
    pygame.K_ESCAPE: 27,
}

def recvall(sock, n):
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data

def main():
    parser = argparse.ArgumentParser(description="Doom TCP stream viewer")
    parser.add_argument("--ip", default=SERVER_IP, help="Server IP")
    parser.add_argument("--port", type=int, default=SERVER_PORT, help="Server port")
    parser.add_argument("--scale", type=int, default=0, help="Display scale (0=auto)")
    args = parser.parse_args()

    pygame.init()

    print(f"Connecting to {args.ip}:{args.port}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((args.ip, args.port))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("Connected!")
    except Exception as e:
        print(f"Connection failed: {e}")
        return

    hello = recvall(sock, HELLO_SIZE)
    if not hello:
        print("Disconnected before stream hello")
        return

    if hello[:4] != HELLO_MAGIC:
        print("Unsupported stream hello from server")
        return

    width, height = struct.unpack("!HH", hello[4:8])
    bpp = hello[8]
    if bpp != 24:
        print(f"Unsupported stream format: {bpp} bpp")
        return

    display_scale = args.scale if args.scale > 0 else (2 if width <= 400 else 1)
    total_bytes = width * height * 3

    screen = pygame.display.set_mode((width * display_scale, height * display_scale))
    pygame.display.set_caption(f"Doom TCP Stream ({width}x{height}, scale {display_scale}x)")
    clock = pygame.time.Clock()

    print(f"Stream config: {width}x{height}, {bpp}bpp, display scale {display_scale}x")

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN or event.type == pygame.KEYUP:
                if event.key in KEY_MAP:
                    doom_key = KEY_MAP[event.key]
                    pressed = 1 if event.type == pygame.KEYDOWN else 0
                    try:
                        sock.sendall(struct.pack('BB', doom_key, pressed))
                    except:
                        pass
        
        try:
            # Read 24-bit frame
            frame_data = recvall(sock, total_bytes)
            if not frame_data:
                print("Disconnected")
                running = False
                continue

            # Render
            doom_surface = pygame.image.frombuffer(frame_data, (width, height), "BGR")
            if display_scale != 1:
                doom_surface = pygame.transform.scale(doom_surface, (width * display_scale, height * display_scale))
            screen.blit(doom_surface, (0, 0))
            pygame.display.flip()
            
        except Exception as e:
            print(f"Error: {e}")
            running = False

        clock.tick(120)

    sock.close()
    pygame.quit()
    sys.exit()

if __name__ == "__main__":
    main()
