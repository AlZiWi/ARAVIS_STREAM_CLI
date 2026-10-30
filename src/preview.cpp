#include "preview.h"

#include <cstdio>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace {
const char *window_name = nullptr;
}

extern "C" int preview_init(const char *name) {
  window_name = name;
  try {
    cv::namedWindow(window_name, cv::WINDOW_AUTOSIZE);
  } catch (const cv::Exception &error) {
    std::fprintf(stderr, "Could not create preview window: %s\n", error.what());
    window_name = nullptr;
    return 0;
  }
  return 1;
}

extern "C" int preview_frame(ArvBuffer *buffer) {
  size_t size;
  gint width, height, x, y;
  const void *data = arv_buffer_get_image_data(buffer, &size);
  const ArvPixelFormat format = arv_buffer_get_image_pixel_format(buffer);
  cv::Mat image;
  char line[256];
  if (!window_name || !data) return 0;
  arv_buffer_get_image_region(buffer, &x, &y, &width, &height);
  (void) x;
  (void) y;
  if (format == ARV_PIXEL_FORMAT_MONO_8)
    image = cv::Mat(height, width, CV_8UC1, const_cast<void *>(data));
  else if (format == ARV_PIXEL_FORMAT_RGB_8_PACKED)
    cv::cvtColor(cv::Mat(height, width, CV_8UC3, const_cast<void *>(data)),
      image, cv::COLOR_RGB2BGR);
  else if (format == ARV_PIXEL_FORMAT_BGR_8_PACKED)
    image = cv::Mat(height, width, CV_8UC3, const_cast<void *>(data));
  else {
    std::fprintf(stderr, "Preview does not support pixel format 0x%08x\n", format);
    return 0;
  }
  std::snprintf(line, sizeof(line), "frame %llu  %ux%u  payload %zu bytes",
    (unsigned long long) arv_buffer_get_frame_id(buffer), (unsigned) width,
    (unsigned) height, size);
  cv::putText(image, line, cv::Point(12, 24), cv::FONT_HERSHEY_SIMPLEX,
    0.55, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
  try {
    cv::imshow(window_name, image);
    return (cv::waitKey(1) & 0xff) == 'q';
  } catch (const cv::Exception &error) {
    std::fprintf(stderr, "Preview display failed: %s\n", error.what());
    return 1;
  }
}

extern "C" void preview_close(void) {
  if (window_name) {
    cv::destroyWindow(window_name);
    window_name = nullptr;
  }
}
