/*
 * Full capture for 'Geek szitman supercamera' (3301:2001)
 * MFi probe on EP1, open on EP2, read video from EP2 (0x82).
 * Reassembles JPEG frames and saves them.
 */
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <span>
#include <thread>
#include <vector>

#include <libusb-1.0/libusb.h>

using byteVector = std::vector<uint8_t>;
using vid_pid_t = std::pair<uint16_t, uint16_t>;

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
        /* After reset, interfaces are released - re-claim them */
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

/* UPP frame parsing (from hbens PoC, adapted for 3301:2001) */
class UPPCamera {
    struct [[gnu::packed]] upp_usb_frame_t {
        uint16_t magic;   /* AA BB */
        uint8_t cid;      /* camera id: 7 = image */
        uint16_t length;  /* payload length (LE), does not include 5-byte header */
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

    static constexpr uint16_t UPP_USB_MAGIC = 0xBBAA; /* little-endian read of AA BB */
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

static std::atomic_bool exit_program = false;
static constexpr std::string_view pic_dir = "pics";

static void pic_callback(const byteVector &pic)
{
    static uint32_t i = 0;

    std::cout << "PIC i:" << i << " size:" << pic.size() << std::endl;

    std::ostringstream filename;
    filename << pic_dir << "/frame_" << std::setfill('0') << std::setw(6) << i << ".jpg";
    std::ofstream output(filename.str(), std::ios::binary);
    output.write(reinterpret_cast<const char *>(pic.data()), pic.size());
    std::cout << "Saved frame to " << filename.str() << std::endl;

    i++;
}

static void capture(UsbSupercamera *usb) {
    UPPCamera upp_camera(pic_callback);
    byteVector read_buf;

    while (!exit_program) {
        int ret = usb->read_video(read_buf);
        if (ret == 0) {
            upp_camera.handle_upp_frame(read_buf);
        } else if (ret == LIBUSB_ERROR_NO_DEVICE) {
            exit_program = true;
        }
    }
}

int main(int argc, char **argv)
{
    int max_frames = 5;
    if (argc > 1) {
        max_frames = std::atoi(argv[1]);
    }

    try {
        UsbSupercamera usb;

        std::filesystem::create_directory(pic_dir);

        /* Init sequence for 3301:2001:
         * MFi probe on EP1 (iAP interface), open stream on EP2 (video interface) */
        std::cout << "=== Resetting device ===" << std::endl;
        usb.reset_device();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "=== MFi probe on EP1 ===" << std::endl;
        usb.write_ep1({0xFF, 0x55, 0xFF, 0x55, 0xEE, 0x10});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "=== Open stream on EP2 ===" << std::endl;
        usb.write_ep2({0xBB, 0xAA, 5, 0, 0});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::thread capture_thread(capture, &usb);

        auto start = std::chrono::steady_clock::now();
        while (!exit_program) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            size_t count = 0;
            for (const auto &entry : std::filesystem::directory_iterator(pic_dir)) {
                (void)entry;
                count++;
            }
            if (count >= (size_t)max_frames) {
                std::cout << "Reached " << max_frames << " frames, stopping." << std::endl;
                exit_program = true;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
                std::cout << "Timeout 30s reached." << std::endl;
                exit_program = true;
            }
        }

        capture_thread.join();
        return 0;
    } catch (...) {
        return 1;
    }
}
