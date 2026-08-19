/*
 * MJPEG streaming server for 'Geek szitman supercamera' (3301:2001)
 * 
 * Captures video from the camera (MFi probe on EP1, open on EP2, read EP2)
 * and serves it as an MJPEG HTTP stream on port 8080.
 * 
 * Endpoints:
 *   GET /            - HTML viewer page
 *   GET /stream      - MJPEG stream (multipart/x-mixed-replace)
 *   GET /snapshot    - Single JPEG frame
 * 
 * Usage: ./supercamera_server [port]
 */
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

using byteVector = std::vector<uint8_t>;
using vid_pid_t = std::pair<uint16_t, uint16_t>;

/* ============ USB Camera Driver ============ */

class UsbSupercamera {
    static constexpr vid_pid_t USB_VENDOR_PRODUCT_ID_LIST[] =
        {{0x3301, 0x2001}};
    static constexpr int INTERFACE_A_NUMBER = 0;
    static constexpr int INTERFACE_B_NUMBER = 1;
    static constexpr int INTERFACE_B_ALTERNATE_SETTING = 1;
    static constexpr unsigned char ENDPOINT_1 = 1;   /* 0x81 IN / 0x01 OUT (iAP) */
    static constexpr unsigned char ENDPOINT_2 = 2;   /* 0x82 IN / 0x02 OUT (video) */
    static constexpr unsigned int USB_TIMEOUT = 2000; /* in ms */

    libusb_context *ctx;
    libusb_device_handle *handle;

    int usb_read(unsigned char endpoint, byteVector &buf, size_t max_size) {
        int ret;
        int transferred;
        buf.resize(max_size);
        ret = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_IN | endpoint,
                                   buf.data(), buf.size(), &transferred, USB_TIMEOUT);
        if (ret != 0) {
            buf.resize(0);
            return ret;
        }
        buf.resize(transferred);
        return 0;
    }

    int usb_write(unsigned char endpoint, byteVector buf) {
        int ret;
        int transferred;
        ret = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_OUT | endpoint,
                                   buf.data(), buf.size(), &transferred, USB_TIMEOUT);
        if (ret != 0) {
            return ret;
        }
        return 0;
    }

    libusb_device_handle *libusb_open_device_with_vid_pid_list(
        libusb_context *ctx, std::span<const vid_pid_t> vid_pid_list) {
        struct libusb_device **devs;
        struct libusb_device *found = nullptr;
        struct libusb_device *dev;
        struct libusb_device_handle *dev_handle = nullptr;

        if (libusb_get_device_list(ctx, &devs) < 0) {
            return nullptr;
        }

        size_t i = 0;
        while ((dev = devs[i++]) != nullptr) {
            struct libusb_device_descriptor desc;
            int ret = libusb_get_device_descriptor(dev, &desc);
            if (ret < 0) {
                goto out;
            }
            for (const auto &vid_pid : vid_pid_list)
            if (desc.idVendor == vid_pid.first && desc.idProduct == vid_pid.second) {
                found = dev;
                break;
            }
        }

        if (found) {
            int ret = libusb_open(found, &dev_handle);
            if (ret < 0) {
                dev_handle = NULL;
            }
        }

        out:
        libusb_free_device_list(devs, 1);
        return dev_handle;
    }

    int setup() {
        int ret;

        ret = libusb_init(&ctx);
        if (ret < 0) {
            std::cerr << "fatal: libusb_init fail (" << ret << ") " << libusb_strerror(ret) << std::endl;
            return 1;
        }

        handle = libusb_open_device_with_vid_pid_list(ctx, std::span(USB_VENDOR_PRODUCT_ID_LIST));
        if (!handle) {
            std::cerr << "fatal: usb device not found" << std::endl;
            return 1;
        }

        ret = libusb_claim_interface(handle, INTERFACE_A_NUMBER);
        if (ret < 0) {
            std::cerr << "fatal: usb_claim_interface A error (" << ret << ") " << libusb_strerror(ret) << std::endl;
            return 1;
        }

        ret = libusb_claim_interface(handle, INTERFACE_B_NUMBER);
        if (ret < 0) {
            std::cerr << "fatal: usb_claim_interface B error (" << ret << ") " << libusb_strerror(ret) << std::endl;
            return 1;
        }

        ret = libusb_set_interface_alt_setting(handle, INTERFACE_B_NUMBER, INTERFACE_B_ALTERNATE_SETTING);
        if (ret < 0) {
            std::cerr << "fatal: libusb_set_interface_alt_setting B error (" << ret << ") "
                      << libusb_strerror(ret) << std::endl;
            return 1;
        }

        ret = libusb_clear_halt(handle, ENDPOINT_1);
        if (ret < 0) {
            std::cerr << "fatal: libusb_clear_halt EP1 error (" << ret << ") " << libusb_strerror(ret) << std::endl;
            return 1;
        }

        return 0;
    }

public:
    UsbSupercamera() {
        int ret = setup();
        if (ret != 0) {
            throw 1;
        }
    }

    ~UsbSupercamera() {
        libusb_close(handle);
        libusb_exit(ctx);
    }

    void reset_device() {
        libusb_reset_device(handle);
        libusb_claim_interface(handle, INTERFACE_A_NUMBER);
        libusb_claim_interface(handle, INTERFACE_B_NUMBER);
        libusb_set_interface_alt_setting(handle, INTERFACE_B_NUMBER, INTERFACE_B_ALTERNATE_SETTING);
        libusb_clear_halt(handle, ENDPOINT_1);
    }

    int read_video(byteVector &read_buf) {
        return usb_read(ENDPOINT_2, read_buf, 0x400);
    }

    int write_ep1(byteVector buf) { return usb_write(ENDPOINT_1, buf); }
    int write_ep2(byteVector buf) { return usb_write(ENDPOINT_2, buf); }
};

/* ============ UPP Frame Parser ============ */

class UPPCamera {
    struct [[gnu::packed]] upp_usb_frame_t {
        uint16_t magic;   /* AA BB */
        uint8_t cid;      /* camera id: 7 = image */
        uint16_t length;  /* payload length (LE) */
    };

    struct [[gnu::packed]] upp_cam_frame_t {
        uint8_t fid;      /* frame id */
        uint8_t cam_num;  /* camera number */
        unsigned char has_g:1;
        unsigned char button_press:1;
        unsigned char other:6;
        uint32_t g_sensor;
    };

    typedef void(*pic_callback_t)(const byteVector &pic);
    pic_callback_t pic_callback;

    static constexpr uint16_t UPP_USB_MAGIC = 0xBBAA;
    static constexpr uint8_t UPP_CAMID_7 = 7;

    byteVector camera_buffer;
    bool first_frame_skipped = false;

    static constexpr size_t BLOCK_SIZE = 512;
    static constexpr size_t HDR_SIZE = 12;
    static constexpr size_t DATA_SIZE = 500;

    /* Find header (aa bb 07 fb 01) starting at or near `start` (0-3 pad bytes allowed).
     * Returns position of header, or npos. */
    size_t find_header(size_t start) const {
        static const uint8_t magic[5] = {0xAA, 0xBB, 0x07, 0xFB, 0x01};
        size_t max_pad = 4;
        for (size_t pad = 0; pad < max_pad && start + pad + 5 <= camera_buffer.size(); pad++) {
            size_t p = start + pad;
            if (std::memcmp(camera_buffer.data() + p, magic, 5) == 0) {
                return p;
            }
        }
        return std::string::npos;
    }

public:
    UPPCamera(pic_callback_t pic_callback) {
        this->pic_callback = pic_callback;
    }

    void handle_upp_frame(const byteVector &data) {
        /* 3301:2001 framing (empirically determined from raw dump):
         * - USB transfer = 1024B = two 512B blocks
         * - Each 512B block: 12B header (aa bb 07 fb 01 + fid + cam + cont + 4B meta) + 500B JPEG
         * - Frame boundary = EOI (FF D9). JPEG data never contains FF D9 (byte stuffing),
         *   so EOI detection is exact.
         * - After EOI, next frame starts with a fresh header; 0-3 pad bytes may precede it.
         * - First frame after open (fid=0) may be incomplete; skip frames without SOI. */
        camera_buffer.insert(camera_buffer.end(), data.begin(), data.end());

        while (true) {
            size_t hdr = find_header(0);
            if (hdr == std::string::npos || hdr + HDR_SIZE > camera_buffer.size()) {
                /* Not enough data yet; keep buffered */
                break;
            }
            /* Consume pad bytes before header */
            if (hdr > 0) {
                camera_buffer.erase(camera_buffer.begin(), camera_buffer.begin() + hdr);
            }

            byteVector jpeg;
            size_t block_pos = 0;
            bool eoi_found = false;

            while (block_pos + BLOCK_SIZE + 5 <= camera_buffer.size()) {
                size_t next_hdr = block_pos + BLOCK_SIZE;
                if (std::memcmp(camera_buffer.data() + next_hdr, "\xAA\xBB\x07\xFB\x01", 5) == 0) {
                    /* Full 512B block: 12B header + 500B data.
                     * EOI may fall in the middle of this block (frame ends mid-block);
                     * check for it before appending. */
                    size_t eoi = std::string::npos;
                    for (size_t i = block_pos + HDR_SIZE; i + 1 < next_hdr; i++) {
                        if (camera_buffer[i] == 0xFF && camera_buffer[i+1] == 0xD9) {
                            eoi = i;
                            break;
                        }
                    }
                    if (eoi != std::string::npos) {
                        jpeg.insert(jpeg.end(), camera_buffer.begin() + block_pos + HDR_SIZE,
                                    camera_buffer.begin() + eoi + 2);
                        eoi_found = true;
                        camera_buffer.erase(camera_buffer.begin(), camera_buffer.begin() + eoi + 2);
                        break;
                    }
                    jpeg.insert(jpeg.end(), camera_buffer.begin() + block_pos + HDR_SIZE,
                                 camera_buffer.begin() + next_hdr);
                    block_pos = next_hdr;
                } else {
                    /* Last block of frame: data runs until EOI */
                    size_t eoi = std::string::npos;
                    for (size_t i = block_pos + HDR_SIZE; i + 1 < camera_buffer.size(); i++) {
                        if (camera_buffer[i] == 0xFF && camera_buffer[i+1] == 0xD9) {
                            eoi = i;
                            break;
                        }
                    }
                    if (eoi == std::string::npos) {
                        /* EOI not yet arrived; keep buffered */
                        break;
                    }
                    jpeg.insert(jpeg.end(), camera_buffer.begin() + block_pos + HDR_SIZE,
                                camera_buffer.begin() + eoi + 2);
                    eoi_found = true;
                    camera_buffer.erase(camera_buffer.begin(), camera_buffer.begin() + eoi + 2);
                    break;
                }
            }

            if (!eoi_found) {
                break;
            }

            /* Emit frame if it looks like a complete JPEG (starts with SOI).
             * Skip the very first frame after open: the device sends an
             * incomplete/partial frame (fid=0) that fails to decode. */
            if (jpeg.size() >= 4 && jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
                if (!first_frame_skipped) {
                    first_frame_skipped = true;
                } else {
                    pic_callback(jpeg);
                }
            }
        }
    }
};

/* ============ Shared frame state ============ */

static std::mutex frame_mtx;
static byteVector latest_frame;
static std::atomic_uint32_t frame_count{0};
static std::atomic_bool exit_program = false;

static void pic_callback(const byteVector &pic)
{
    std::lock_guard lock(frame_mtx);
    latest_frame = pic;
    frame_count++;
}

/* ============ HTTP Server ============ */

static const char *HTML_PAGE = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Periscope Camera</title>
<style>
body { background: #111; color: #eee; font-family: sans-serif; margin: 0; padding: 20px; text-align: center; }
h1 { font-size: 1.2em; }
img { max-width: 100%; border-radius: 8px; box-shadow: 0 4px 20px rgba(0,0,0,0.5); }
.info { color: #888; font-size: 0.85em; margin-top: 10px; }
a { color: #4af; }
</style>
</head>
<body>
<h1>🔭 Periscope Camera</h1>
<img src="/stream" alt="Live stream">
<div class="info">MJPEG stream · <a href="/snapshot">snapshot</a> · <a href="/stream">direct stream URL</a></div>
</body>
</html>
)HTML";

static void send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += n;
    }
}

static void send_all(int fd, const std::string &s) {
    send_all(fd, s.data(), s.size());
}

static void handle_client(int fd) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    buf[n] = '\0';
    std::string req(buf);

    std::string path = "/";
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    if (path == "/" || path == "/index.html") {
        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: " + std::to_string(strlen(HTML_PAGE)) + "\r\n"
                           "Connection: close\r\n\r\n";
        send_all(fd, resp);
        send_all(fd, HTML_PAGE, strlen(HTML_PAGE));
        close(fd);
    } else if (path == "/snapshot") {
        byteVector frame;
        {
            std::lock_guard lock(frame_mtx);
            frame = latest_frame;
        }
        if (frame.empty()) {
            std::string resp = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 20\r\n"
                               "Connection: close\r\n\r\n"
                               "No frame available yet";
            send_all(fd, resp);
            close(fd);
            return;
        }
        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: " + std::to_string(frame.size()) + "\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n";
        send_all(fd, resp);
        send_all(fd, reinterpret_cast<const char *>(frame.data()), frame.size());
        close(fd);
    } else if (path == "/stream.raw") {
        /* Raw MJPEG stream: consecutive JPEG frames with no multipart framing.
         * ffmpeg can consume this with: ffmpeg -f mjpeg -i http://host:8080/stream.raw ... */
        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: video/x-mjpeg\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: keep-alive\r\n\r\n";
        send_all(fd, resp);

        uint32_t last_count = 0;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (!exit_program) {
            byteVector frame;
            uint32_t count;
            {
                std::lock_guard lock(frame_mtx);
                frame = latest_frame;
                count = frame_count.load();
            }
            if (!frame.empty() && count != last_count) {
                last_count = count;
                send_all(fd, reinterpret_cast<const char *>(frame.data()), frame.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        close(fd);
    } else if (path == "/stream") {
        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: keep-alive\r\n\r\n";
        send_all(fd, resp);

        uint32_t last_count = 0;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (!exit_program) {
            byteVector frame;
            uint32_t count;
            {
                std::lock_guard lock(frame_mtx);
                frame = latest_frame;
                count = frame_count.load();
            }
            if (!frame.empty() && count != last_count) {
                last_count = count;
                std::string header = "--frame\r\n"
                                     "Content-Type: image/jpeg\r\n"
                                     "Content-Length: " + std::to_string(frame.size()) + "\r\n\r\n";
                send_all(fd, header);
                send_all(fd, reinterpret_cast<const char *>(frame.data()), frame.size());
                send_all(fd, "\r\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        close(fd);
    } else {
        std::string resp = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 9\r\n"
                           "Connection: close\r\n\r\n"
                           "Not Found";
        send_all(fd, resp);
        close(fd);
    }
}

static void http_server(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed" << std::endl;
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind failed on port " << port << std::endl;
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 8) < 0) {
        std::cerr << "listen failed" << std::endl;
        close(listen_fd);
        return;
    }

    std::cout << "HTTP server listening on port " << port << std::endl;

    while (!exit_program) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (exit_program) break;
            continue;
        }
        std::thread(handle_client, client_fd).detach();
    }
    close(listen_fd);
}

/* ============ Capture loop ============ */

static void capture_loop(UsbSupercamera *usb) {
    UPPCamera upp_camera(pic_callback);
    byteVector read_buf;

    while (!exit_program) {
        int ret = usb->read_video(read_buf);
        if (ret == 0) {
            upp_camera.handle_upp_frame(read_buf);
        } else if (ret == LIBUSB_ERROR_NO_DEVICE) {
            std::cerr << "Device disconnected" << std::endl;
            exit_program = true;
        } else if (ret == LIBUSB_ERROR_TIMEOUT) {
            /* Timeout - try to recover by re-opening the stream */
            std::cerr << "Read timeout, re-opening stream..." << std::endl;
            usb->write_ep2({0xBB, 0xAA, 5, 0, 0});
        }
    }
}

/* ============ Main ============ */

int main(int argc, char **argv)
{
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    try {
        UsbSupercamera usb;

        /* Init sequence for 3301:2001 */
        std::cout << "=== Resetting device ===" << std::endl;
        usb.reset_device();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "=== MFi probe on EP1 ===" << std::endl;
        usb.write_ep1({0xFF, 0x55, 0xFF, 0x55, 0xEE, 0x10});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "=== Open stream on EP2 ===" << std::endl;
        usb.write_ep2({0xBB, 0xAA, 5, 0, 0});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::thread capture_thread(capture_loop, &usb);
        http_server(port);

        exit_program = true;
        capture_thread.join();
        return 0;
    } catch (...) {
        return 1;
    }
}
