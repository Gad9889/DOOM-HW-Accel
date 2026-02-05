import socket
import struct
import pygame
import sys

# Doom Resolution
WIDTH = 640
HEIGHT = 400
# 24-bit Color (BGR)
TOTAL_BYTES = WIDTH * HEIGHT * 3

# Network Config
SERVER_IP = "192.168.3.4" 
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
    pygame.init()
    
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Doom TCP Stream (24-bit Stable)")
    clock = pygame.time.Clock()

    print(f"Connecting to {SERVER_IP}:{SERVER_PORT}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((SERVER_IP, SERVER_PORT))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("Connected!")
    except Exception as e:
        print(f"Connection failed: {e}")
        return

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
            frame_data = recvall(sock, TOTAL_BYTES)
            if not frame_data:
                print("Disconnected")
                running = False
                continue

            # Render
            doom_surface = pygame.image.frombuffer(frame_data, (WIDTH, HEIGHT), "BGR")
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