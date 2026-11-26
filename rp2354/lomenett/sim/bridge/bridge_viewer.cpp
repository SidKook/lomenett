// bridge_viewer.cpp -- shows the OLED frames streamed by ../../lomenett_bridge.h
// in an SDL2 window. Other serial text prints to this terminal and lines typed
// here go to the device console (replaces the Serial Monitor). Sends
// "bridge on" on connect.
//
// Usage: bridge_viewer [port] [zoom]   (defaults /dev/ttyACM0, 4)
// Keys:  A follow device rotation   R rotate view   S save BMP   Esc quit
// LOMENETT_BRIDGE_SNAPSHOT=/path.bmp rewrites that file on every frame.

#include <SDL.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Must match lomenett_bridge.h.
static const uint8_t MAGIC[4] = {0xA5, 0x5A, 'L', 'M'};
static const uint8_t FRAME_TYPE_FRAMEBUFFER = 1;
static const int OLED_W = 128, OLED_H = 32;
static const size_t FRAMEBUFFER_BYTES = OLED_W * OLED_H / 8;
static const size_t FRAME_BYTES = 4 + 2 + FRAMEBUFFER_BYTES + 2;

static int ZOOM = 4;

static uint8_t g_framebuffer[FRAMEBUFFER_BYTES];
static int g_deviceRotation = 0;   // rotation byte from the latest frame
static int g_manualRotation = 0;   // used when follow-rotation is off
static bool g_followDeviceRotation = true;
static bool g_haveFrame = false;
static bool g_needsRedraw = true;
static uint32_t g_lastFrameMs = 0;   // SDL ticks of the last good frame

// ---------------------------------------------------------------------
// Serial port
// ---------------------------------------------------------------------
static int openPort(const char* path) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  termios settings{};
  if (tcgetattr(fd, &settings) == 0) {
    cfmakeraw(&settings);
    // Baud is ignored over USB, but 1200 would reboot the RP into its bootloader.
    cfsetispeed(&settings, B115200);
    cfsetospeed(&settings, B115200);
    settings.c_cflag |= CLOCAL | CREAD | HUPCL;   // opening raises DTR, which the device waits for
    tcsetattr(fd, TCSANOW, &settings);
  }
  return fd;
}

static void portWrite(int fd, const char* data, size_t length) {
  while (length) {
    ssize_t written = write(fd, data, length);
    if (written > 0) { data += written; length -= written; continue; }
    if (written < 0 && errno != EAGAIN && errno != EINTR) return;
    SDL_Delay(1);
  }
}

// ---------------------------------------------------------------------
// Stream parser: frames out, everything else printed as text
// ---------------------------------------------------------------------
static std::vector<uint8_t> g_receiveBuffer;
static size_t g_bytesToSwallow = 0;   // bytes of a failed frame left to swallow

static void printTextByte(uint8_t ch) {
  if (g_bytesToSwallow) { g_bytesToSwallow--; return; }
  if (ch == '\n' || ch == '\t' || (ch >= 0x20 && ch < 0x7F)) {
    fputc(ch, stdout);
    if (ch == '\n') fflush(stdout);
  }
}

// Fletcher-16 over type, rotation and buffer.
static bool frameValid(const uint8_t* frame) {
  uint16_t sum1 = 0, sum2 = 0;
  for (size_t i = 4; i < 6 + FRAMEBUFFER_BYTES; i++) {
    sum1 = (sum1 + frame[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  return frame[4] == FRAME_TYPE_FRAMEBUFFER && frame[5] < 4 &&
         frame[6 + FRAMEBUFFER_BYTES] == sum1 && frame[6 + FRAMEBUFFER_BYTES + 1] == sum2;
}

static void parse() {
  size_t i = 0;
  while (i < g_receiveBuffer.size()) {
    size_t remaining = g_receiveBuffer.size() - i;
    size_t magicMatched = 0;                 // does a frame start here?
    while (magicMatched < 4 && magicMatched < remaining && g_receiveBuffer[i + magicMatched] == MAGIC[magicMatched]) magicMatched++;
    if (magicMatched < 4 && magicMatched == remaining) break;   // partial magic at the end: wait
    if (magicMatched == 4) {
      if (remaining < FRAME_BYTES) break;    // whole frame not here yet
      const uint8_t* frame = &g_receiveBuffer[i];
      if (frameValid(frame)) {
        memcpy(g_framebuffer, frame + 6, FRAMEBUFFER_BYTES);
        g_deviceRotation = frame[5];
        g_haveFrame = true;
        g_lastFrameMs = SDL_GetTicks();
        g_needsRedraw = true;
        g_bytesToSwallow = 0;
        i += FRAME_BYTES;
        continue;
      }
      // Corrupted (console text landed inside it): skip the magic and
      // swallow its would-be payload instead of printing junk.
      g_bytesToSwallow = FRAME_BYTES - 4;
      i += 4;
      continue;
    }
    printTextByte(g_receiveBuffer[i]);
    i++;
  }
  g_receiveBuffer.erase(g_receiveBuffer.begin(), g_receiveBuffer.begin() + i);
}

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------
static int viewRotation() { return g_followDeviceRotation ? g_deviceRotation : g_manualRotation; }

static void viewSize(int rotation, int& width, int& height) {
  bool portrait = rotation & 1;
  width = (portrait ? OLED_H : OLED_W) * ZOOM;
  height = (portrait ? OLED_W : OLED_H) * ZOOM;
}

// Panel pixel -> upright view pixel (undo setRotation) so it reads like the device in hand.
static void toView(int rotation, int panelX, int panelY, int& viewX, int& viewY) {
  switch (rotation) {
    default:
    case 0: viewX = panelX;              viewY = panelY;              break;
    case 1: viewX = panelY;              viewY = OLED_W - 1 - panelX; break;
    case 2: viewX = OLED_W - 1 - panelX; viewY = OLED_H - 1 - panelY; break;
    case 3: viewX = OLED_H - 1 - panelY; viewY = panelX;              break;
  }
}

// Render the current view to a new surface (used by the window and snapshots).
static SDL_Surface* paintView() {
  int rotation = viewRotation();
  int width, height;
  viewSize(rotation, width, height);
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface) return nullptr;
  SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, 8, 10, 14));
  if (g_haveFrame) {
    Uint32 pixelColor = SDL_MapRGB(surface->format, 235, 235, 245);
    for (int y = 0; y < OLED_H; y++) {
      for (int x = 0; x < OLED_W; x++) {
        if (!(g_framebuffer[x + (y / 8) * OLED_W] & (1 << (y & 7)))) continue;
        int viewX, viewY;
        toView(rotation, x, y, viewX, viewY);
        SDL_Rect pixelRect{viewX * ZOOM, viewY * ZOOM, ZOOM, ZOOM};
        SDL_FillRect(surface, &pixelRect, pixelColor);
      }
    }
  }
  return surface;
}

static void redraw(SDL_Window* window, SDL_Renderer* renderer, const char* port, bool connected,
                   const char* autoSnapshotPath) {
  int rotation = viewRotation();
  int width, height, windowWidth, windowHeight;
  viewSize(rotation, width, height);
  SDL_GetWindowSize(window, &windowWidth, &windowHeight);
  if (windowWidth != width || windowHeight != height) SDL_SetWindowSize(window, width, height);

  char title[256];
  snprintf(title, sizeof title, "Lomenett Bridge - %s %s - rot %d%s", port,
           connected ? (g_haveFrame ? "" : "(waiting for frames)") : "(not connected)",
           rotation, g_followDeviceRotation ? " (follow)" : "");
  SDL_SetWindowTitle(window, title);

  SDL_Surface* surface = paintView();
  if (!surface) return;
  if (autoSnapshotPath) SDL_SaveBMP(surface, autoSnapshotPath);
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_FreeSurface(surface);
  if (!texture) return;
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, nullptr, nullptr);   // stretched to the window until a resize lands
  SDL_RenderPresent(renderer);
  SDL_DestroyTexture(texture);
}

static void snapshot() {
  static int snapshotNumber = 0;
  SDL_Surface* surface = paintView();
  if (!surface) return;
  char name[64];
  snprintf(name, sizeof name, "bridge_snap_%d.bmp", snapshotNumber++);
  if (SDL_SaveBMP(surface, name) == 0) fprintf(stderr, "[BRIDGE] saved %s\n", name);
  SDL_FreeSurface(surface);
}

int main(int argc, char** argv) {
  const char* port = argc > 1 ? argv[1] : "/dev/ttyACM0";
  if (argc > 2 && atoi(argv[2]) > 0) ZOOM = atoi(argv[2]);
  const char* autoSnapshotPath = getenv("LOMENETT_BRIDGE_SNAPSHOT");

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[BRIDGE] SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("Lomenett Bridge", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     OLED_W * ZOOM, OLED_H * ZOOM, SDL_WINDOW_SHOWN);
  if (!window) { fprintf(stderr, "[BRIDGE] SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) { fprintf(stderr, "[BRIDGE] SDL_CreateRenderer failed: %s\n", SDL_GetError()); return 1; }

  printf("Lomenett bridge viewer on %s\n"
         "  A  follow the device's rotation   R  rotate view   S  snapshot   Esc  quit\n"
         "  (close the Arduino IDE's Serial Monitor first; console text appears here,\n"
         "   and lines typed in this terminal go to the board's console)\n\n",
         port);
  fflush(stdout);

  int fd = -1;
  bool stdinOpen = true;
  uint32_t lastBridgeOnSentMs = 0;
  bool warned = false;
  uint32_t nextOpenAttemptMs = 0;
  bool quit = false;

  while (!quit) {
    // (Re)connect: the port disappears whenever the device resets or is unplugged.
    if (fd < 0 && SDL_GetTicks() >= nextOpenAttemptMs) {
      fd = openPort(port);
      if (fd >= 0) {
        fprintf(stderr, "[BRIDGE] connected to %s\n", port);
        warned = false;
        g_receiveBuffer.clear();
        g_bytesToSwallow = 0;
        g_haveFrame = false;
        lastBridgeOnSentMs = 0;
      } else {
        if (!warned) {
          fprintf(stderr, "[BRIDGE] can't open %s (%s) - retrying. Is the device plugged in "
                          "and the Serial Monitor closed?\n", port, strerror(errno));
          warned = true;
        }
        nextOpenAttemptMs = SDL_GetTicks() + 500;
      }
      g_needsRedraw = true;
    }

    if (fd >= 0) {
      uint32_t now = SDL_GetTicks();
      if (now - g_lastFrameMs > 2000 && (lastBridgeOnSentMs == 0 || now - lastBridgeOnSentMs > 2000)) {
        portWrite(fd, "bridge on\n", 10);
        lastBridgeOnSentMs = now;
      }
    }

    // Terminal -> console. stdin is line buffered, so whole lines arrive.
    if (stdinOpen) {
      pollfd stdinPoll{0, POLLIN, 0};
      if (poll(&stdinPoll, 1, 0) > 0) {
        char readBuffer[512];
        ssize_t bytesRead = read(0, readBuffer, sizeof readBuffer);
        if (bytesRead <= 0) stdinOpen = false;
        else if (fd >= 0) portWrite(fd, readBuffer, bytesRead);
      }
    }

    if (fd >= 0) {
      pollfd portPoll{fd, POLLIN, 0};
      if (poll(&portPoll, 1, 5) > 0) {
        if (portPoll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
          close(fd);
          fd = -1;
          fprintf(stderr, "\n[BRIDGE] device disconnected\n");
          g_needsRedraw = true;
        } else {
          uint8_t readBuffer[4096];
          ssize_t bytesRead = read(fd, readBuffer, sizeof readBuffer);
          if (bytesRead > 0) {
            g_receiveBuffer.insert(g_receiveBuffer.end(), readBuffer, readBuffer + bytesRead);
            parse();
          } else if (bytesRead == 0 || (errno != EAGAIN && errno != EINTR)) {
            close(fd);
            fd = -1;
            fprintf(stderr, "\n[BRIDGE] device disconnected\n");
            g_needsRedraw = true;
          }
        }
      }
    } else {
      SDL_Delay(5);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) quit = true;
      if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
          case SDLK_ESCAPE: quit = true; break;
          case SDLK_a: g_followDeviceRotation = !g_followDeviceRotation; g_manualRotation = g_deviceRotation; g_needsRedraw = true; break;
          case SDLK_r: g_manualRotation = (viewRotation() + 1) & 3; g_followDeviceRotation = false; g_needsRedraw = true; break;
          case SDLK_s: snapshot(); break;
        }
      }
      if (event.type == SDL_WINDOWEVENT) g_needsRedraw = true;
    }

    if (g_needsRedraw) {
      redraw(window, renderer, port, fd >= 0, autoSnapshotPath);
      g_needsRedraw = false;
    }
  }

  if (fd >= 0) close(fd);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
