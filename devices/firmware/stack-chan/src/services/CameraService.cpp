#include "CameraService.h"

#include "Config.h"

#if STACK_CHAN_ENABLE_CAMERA
#include <cstring>
#include <cerrno>
#include <esp32-hal-i2c.h>
#include <esp_http_server.h>
#include <esp_video_device.h>
#include <esp_video_init.h>
#include <fcntl.h>
#include <img_converters.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr char kStreamContentType[] =
    "multipart/x-mixed-replace;boundary=stackchanframe";
constexpr char kStreamBoundary[] = "\r\n--stackchanframe\r\n";
constexpr char kStreamPart[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t streamHandler(httpd_req_t *request) {
  auto *camera = static_cast<CameraService *>(request->user_ctx);
  esp_err_t result = httpd_resp_set_type(request, kStreamContentType);
  if (result != ESP_OK) {
    return result;
  }
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");

  while (result == ESP_OK) {
    uint8_t *jpeg = nullptr;
    size_t jpegLength = 0;
    if (!camera->captureJpeg(&jpeg, &jpegLength)) {
      return ESP_FAIL;
    }

    char header[96];
    const size_t headerLength =
        snprintf(header, sizeof(header), kStreamPart,
                 static_cast<unsigned>(jpegLength));
    result = httpd_resp_send_chunk(request, kStreamBoundary,
                                   strlen(kStreamBoundary));
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(request, header, headerLength);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(jpeg), jpegLength);
    }
    free(jpeg);
    vTaskDelay(pdMS_TO_TICKS(35));
  }
  return result;
}

bool startStreamServer(CameraService *camera) {
  httpd_config_t serverConfig = HTTPD_DEFAULT_CONFIG();
  serverConfig.server_port = Config::kCameraStreamPort;
  serverConfig.ctrl_port = Config::kCameraStreamPort + 1000;
  serverConfig.stack_size = 8192;
  serverConfig.max_open_sockets = 2;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &serverConfig) != ESP_OK) {
    return false;
  }

  httpd_uri_t streamUri = {};
  streamUri.uri = "/stream";
  streamUri.method = HTTP_GET;
  streamUri.handler = streamHandler;
  streamUri.user_ctx = camera;
  return httpd_register_uri_handler(server, &streamUri) == ESP_OK;
}
} // namespace

bool CameraService::begin() {
  static esp_cam_ctlr_dvp_pin_config_t pins = {};
  pins.data_width = CAM_CTLR_DATA_WIDTH_8;
  pins.data_io[0] = static_cast<gpio_num_t>(39);
  pins.data_io[1] = static_cast<gpio_num_t>(40);
  pins.data_io[2] = static_cast<gpio_num_t>(41);
  pins.data_io[3] = static_cast<gpio_num_t>(42);
  pins.data_io[4] = static_cast<gpio_num_t>(15);
  pins.data_io[5] = static_cast<gpio_num_t>(16);
  pins.data_io[6] = static_cast<gpio_num_t>(48);
  pins.data_io[7] = static_cast<gpio_num_t>(47);
  pins.vsync_io = static_cast<gpio_num_t>(46);
  pins.de_io = static_cast<gpio_num_t>(38);
  pins.pclk_io = static_cast<gpio_num_t>(45);
  pins.xclk_io = static_cast<gpio_num_t>(-1); // External 20 MHz clock.

  esp_video_init_sccb_config_t sccb = {};
  sccb.init_sccb = false;
  sccb.i2c_handle = static_cast<i2c_master_bus_handle_t>(i2cBusHandle(1));
  sccb.freq = 100000;
  if (!sccb.i2c_handle) {
    _error = "internal I2C bus unavailable";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  esp_video_init_dvp_config_t dvp = {};
  dvp.sccb_config = sccb;
  dvp.reset_pin = static_cast<gpio_num_t>(-1);
  dvp.pwdn_pin = static_cast<gpio_num_t>(-1);
  dvp.dvp_pin = pins;
  dvp.xclk_freq = 20000000;

  esp_video_init_config_t video = {};
  video.dvp = &dvp;
  const esp_err_t initResult = esp_video_init(&video);
  if (initResult != ESP_OK) {
    _error = "esp_video init 0x" +
             String(static_cast<unsigned>(initResult), HEX);
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  _videoFd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
  if (_videoFd < 0) {
    _error = "open native video device failed errno=" + String(errno);
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  v4l2_requestbuffers request = {};
  request.count = 1;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(_videoFd, VIDIOC_REQBUFS, &request) != 0 || request.count == 0) {
    _error = "native video buffer request failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  v4l2_buffer buffer = {};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  buffer.index = 0;
  if (ioctl(_videoFd, VIDIOC_QUERYBUF, &buffer) != 0) {
    _error = "native video buffer query failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }
  _frameBuffer = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                      MAP_SHARED, _videoFd, buffer.m.offset);
  if (_frameBuffer == MAP_FAILED) {
    _frameBuffer = nullptr;
    _error = "native video mmap failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }
  _frameBufferLength = buffer.length;
  if (ioctl(_videoFd, VIDIOC_QBUF, &buffer) != 0) {
    _error = "native video queue failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(_videoFd, VIDIOC_G_FMT, &format) != 0) {
    _error = "native video format query failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }
  _width = format.fmt.pix.width;
  _height = format.fmt.pix.height;
  _pixelFormat = format.fmt.pix.pixelformat;

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(_videoFd, VIDIOC_STREAMON, &type) != 0) {
    _error = "native video stream-on failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }

  _initialized = true;
  Serial.printf("[Camera] native sensor ready %ux%u format=0x%08x\n",
                static_cast<unsigned>(_width), static_cast<unsigned>(_height),
                static_cast<unsigned>(_pixelFormat));
  return true;
}

bool CameraService::captureJpeg(uint8_t **jpeg, size_t *jpegLength) {
  if (!_initialized || !jpeg || !jpegLength) {
    return false;
  }

  v4l2_buffer buffer = {};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (ioctl(_videoFd, VIDIOC_DQBUF, &buffer) != 0) {
    return false;
  }

  bool converted = false;
  if (buffer.flags & V4L2_BUF_FLAG_DONE) {
    const size_t used = buffer.bytesused ? buffer.bytesused : _frameBufferLength;
    if (_pixelFormat == V4L2_PIX_FMT_JPEG) {
      *jpeg = static_cast<uint8_t *>(malloc(used));
      if (*jpeg) {
        memcpy(*jpeg, _frameBuffer, used);
        *jpegLength = used;
        converted = true;
      }
    } else {
      pixformat_t sourceFormat = PIXFORMAT_YUV422;
      if (_pixelFormat == V4L2_PIX_FMT_RGB565) {
        sourceFormat = PIXFORMAT_RGB565;
      }
      converted = fmt2jpg(static_cast<uint8_t *>(_frameBuffer), used, _width,
                          _height, sourceFormat, 72, jpeg, jpegLength);
    }
  }
  ioctl(_videoFd, VIDIOC_QBUF, &buffer);
  return converted;
}

bool CameraService::startStream() {
  if (!_initialized) {
    return false;
  }
  if (!startStreamServer(this)) {
    _error = "stream server failed";
    Serial.printf("[Camera] %s\n", _error.c_str());
    return false;
  }
  _available = true;
  Serial.printf("[Camera] native stream ready on port %u\n",
                Config::kCameraStreamPort);
  return true;
}

#else

bool CameraService::begin() {
  _error = "disabled";
  return false;
}

bool CameraService::captureJpeg(uint8_t **, size_t *) { return false; }

bool CameraService::startStream() { return false; }

#endif
