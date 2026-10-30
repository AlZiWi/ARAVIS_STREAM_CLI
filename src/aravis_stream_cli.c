#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <arv.h>
#include <jpeglib.h>

#ifdef ARAVIS_CLI_HAS_PREVIEW
#include "preview.h"
#endif

static volatile sig_atomic_t keep_running = 1;
#define STREAM_FORMAT_MJPEG 0x47504a4dU

typedef enum {
  OUTPUT_FORMAT_MJPEG,
  OUTPUT_FORMAT_RAW
} output_format_t;

typedef struct {
  const char *name;
  const char *out_dir;
  int debug;
  int width;
  int height;
  int fps;
  int stream;
  int list;
  uint32_t exposure;
  int exposure_auto;
  int exposure_specified;
  output_format_t format;
#ifdef ARAVIS_CLI_HAS_PREVIEW
  int preview;
#endif
} capture_options_t;

typedef struct {
  char magic[4];
  uint32_t frame_id;
  uint64_t timestamp_seconds;
  uint32_t timestamp_milliseconds;
  uint32_t width;
  uint32_t height;
  uint32_t frame_format;
  uint64_t frame_bytes;
  uint64_t metadata_bytes;
} stream_record_header_t;

typedef struct {
  const void *data;
  size_t size;
  uint32_t format;
  unsigned char *owned_data;
} frame_payload_t;

typedef struct {
  struct jpeg_error_mgr manager;
  jmp_buf jump;
} jpeg_error_handler_t;

static void stop_handler(int signal_number) {
  (void) signal_number;
  keep_running = 0;
}

static void debug_log(const capture_options_t *options, const char *message) {
  if (options->debug)
    fprintf(stderr, "[debug] %s\n", message);
}

static void usage(const char *program) {
  fprintf(stderr,
    "Usage: %s <camera_name> [options]\n"
    "  -list                List available GenICam cameras\n"
    "  -resx=<pixels>       Requested width\n"
    "  -resy=<pixels>       Requested height\n"
    "  -fps=<fps>           Requested frame rate\n"
    "  -out=<folder_path>   Save frames to this folder\n"
    "  -stream              Write binary frame records to stdout\n"
    "  -format=mjpeg|raw    Output format (default: mjpeg)\n"
    "  -debug               Print initialization and streaming diagnostics\n"
    "  -exposure=<auto|units> Set exposure; units are 0.0001 seconds\n"
#ifdef ARAVIS_CLI_HAS_PREVIEW
    "  -preview             Display the image stream with metadata\n"
#endif
    "\nCamera name is matched exactly against the model, serial, or device ID.\n",
    program);
}

static int parse_positive_int(const char *value, int *result) {
  char *end;
  long parsed;
  errno = 0;
  parsed = strtol(value, &end, 10);
  if (errno || *value == '\0' || *end != '\0' || parsed <= 0 || parsed > INT32_MAX)
    return 0;
  *result = (int) parsed;
  return 1;
}

static int parse_exposure(const char *value, uint32_t *exposure, int *auto_mode) {
  char *end;
  unsigned long long parsed;
  if (!strcmp(value, "auto")) {
    *auto_mode = 1;
    return 1;
  }
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno || *value == '\0' || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
    fprintf(stderr, "Invalid exposure: %s (expected auto or a positive unit value)\n", value);
    return 0;
  }
  *exposure = (uint32_t) parsed;
  *auto_mode = 0;
  return 1;
}

static int parse_options(int argc, char **argv, capture_options_t *options) {
  int index;
  const char *separator;
  const char *value;
  memset(options, 0, sizeof(*options));
  options->format = OUTPUT_FORMAT_MJPEG;
  index = 1;
  if (index < argc && argv[index][0] != '-') {
    options->name = argv[index++];
  }
  for (; index < argc; ++index) {
    separator = strchr(argv[index], '=');
    value = separator ? separator + 1 : NULL;
    if (separator && !strncmp(argv[index], "-out", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 4) {
      if (!*value) {
        fprintf(stderr, "Empty value for -out\n");
        return 0;
      }
      options->out_dir = value;
    } else if (separator && !strncmp(argv[index], "-resx", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 5) {
      if (!parse_positive_int(value, &options->width)) return 0;
    } else if (separator && !strncmp(argv[index], "-resy", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 5) {
      if (!parse_positive_int(value, &options->height)) return 0;
    } else if (separator && !strncmp(argv[index], "-fps", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 4) {
      if (!parse_positive_int(value, &options->fps)) return 0;
    } else if (separator && !strncmp(argv[index], "-exposure", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 9) {
      if (!parse_exposure(value, &options->exposure, &options->exposure_auto))
        return 0;
        options->exposure_specified = 1;
    } else if (!strcmp(argv[index], "-debug") || !strcmp(argv[index], "--debug")) {
      options->debug = 1;
    } else if (!strcmp(argv[index], "-stream")) {
      options->stream = 1;
    } else if (!strcmp(argv[index], "-list")) {
      options->list = 1;
    } else if (separator && !strncmp(argv[index], "-format", (size_t) (separator - argv[index])) &&
        separator - argv[index] == 7) {
      if (!strcmp(value, "mjpeg")) options->format = OUTPUT_FORMAT_MJPEG;
      else if (!strcmp(value, "raw")) options->format = OUTPUT_FORMAT_RAW;
      else {
        fprintf(stderr, "Invalid format: %s (expected mjpeg or raw)\n", value);
        return 0;
      }
#ifdef ARAVIS_CLI_HAS_PREVIEW
    } else if (!strcmp(argv[index], "-preview")) {
      options->preview = 1;
#endif
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[index]);
      return 0;
    }
  }
  if (!options->name && !options->list) {
    fprintf(stderr, "camera name is required as the first argument\n");
    return 0;
  }
  return 1;
}

static void print_devices(void) {
  unsigned int index;
  arv_update_device_list();
  printf("Available GenICam devices:\n");
  for (index = 0; index < arv_get_n_devices(); ++index) {
    printf("[%u] id=\"%s\" model=\"%s\" serial=\"%s\" vendor=\"%s\" "
           "protocol=\"%s\" address=\"%s\"\n", index,
      arv_get_device_id(index) ? arv_get_device_id(index) : "[none]",
      arv_get_device_model(index) ? arv_get_device_model(index) : "[none]",
      arv_get_device_serial_nbr(index) ? arv_get_device_serial_nbr(index) : "[none]",
      arv_get_device_vendor(index) ? arv_get_device_vendor(index) : "[none]",
      arv_get_device_protocol(index) ? arv_get_device_protocol(index) : "[none]",
      arv_get_device_address(index) ? arv_get_device_address(index) : "[none]");
  }
}

static const char *find_device_id(const char *name) {
  unsigned int index;
  arv_update_device_list();
  for (index = 0; index < arv_get_n_devices(); ++index) {
    const char *id = arv_get_device_id(index);
    const char *model = arv_get_device_model(index);
    const char *serial = arv_get_device_serial_nbr(index);
    if ((id && !strcmp(id, name)) || (model && !strcmp(model, name)) ||
        (serial && !strcmp(serial, name)))
      return id;
  }
  return NULL;
}

static int check_error(GError **error, const char *operation) {
  if (!*error) return 1;
  fprintf(stderr, "%s failed: %s\n", operation, (*error)->message);
  g_clear_error(error);
  return 0;
}

static int create_output_directory(const char *path) {
  if (!path) return 1;
  if (mkdir(path, 0777) && errno != EEXIST) {
    perror(path);
    return 0;
  }
  return 1;
}

static void jpeg_error_exit(j_common_ptr common) {
  jpeg_error_handler_t *handler = (jpeg_error_handler_t *) common->err;
  longjmp(handler->jump, 1);
}

static int encode_jpeg(ArvBuffer *buffer, frame_payload_t *payload) {
  struct jpeg_compress_struct compressor;
  jpeg_error_handler_t error_handler;
  const void *data;
  size_t size;
  gint width, height, x_padding, y_padding;
  ArvPixelFormat pixel_format;
  int channels;
  JSAMPROW row_pointer[1];
  unsigned char *row_data = NULL;
  unsigned long jpeg_size = 0;
  unsigned char *jpeg_data = NULL;

  data = arv_buffer_get_image_data(buffer, &size);
  pixel_format = arv_buffer_get_image_pixel_format(buffer);
  width = arv_buffer_get_image_width(buffer);
  height = arv_buffer_get_image_height(buffer);
  arv_buffer_get_image_padding(buffer, &x_padding, &y_padding);
  (void) x_padding;
  (void) y_padding;
  if (pixel_format == ARV_PIXEL_FORMAT_MONO_8) channels = 1;
  else if (pixel_format == ARV_PIXEL_FORMAT_RGB_8_PACKED ||
           pixel_format == ARV_PIXEL_FORMAT_BGR_8_PACKED) channels = 3;
  else {
    fprintf(stderr, "MJPEG output does not support pixel format 0x%08x\n",
      pixel_format);
    return 0;
  }
  if (width <= 0 || height <= 0 ||
      size < (size_t) width * (size_t) height * (size_t) channels) {
    fprintf(stderr, "Frame payload is smaller than its image dimensions\n");
    return 0;
  }
  compressor.err = jpeg_std_error(&error_handler.manager);
  error_handler.manager.error_exit = jpeg_error_exit;
  if (setjmp(error_handler.jump)) {
    jpeg_destroy_compress(&compressor);
    free(row_data);
    free(jpeg_data);
    fprintf(stderr, "JPEG encoding failed\n");
    return 0;
  }
  jpeg_create_compress(&compressor);
  jpeg_mem_dest(&compressor, &jpeg_data, &jpeg_size);
  compressor.image_width = (JDIMENSION) width;
  compressor.image_height = (JDIMENSION) height;
  compressor.input_components = channels;
  compressor.in_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&compressor);
  jpeg_set_quality(&compressor, 90, TRUE);
  jpeg_start_compress(&compressor, TRUE);
  if (channels == 3 && pixel_format == ARV_PIXEL_FORMAT_BGR_8_PACKED)
    row_data = malloc((size_t) width * 3);
  while (compressor.next_scanline < compressor.image_height) {
    const unsigned char *source = (const unsigned char *) data +
      (size_t) compressor.next_scanline * (size_t) width * (size_t) channels;
    if (row_data) {
      for (int column = 0; column < width; ++column) {
        row_data[column * 3] = source[column * 3 + 2];
        row_data[column * 3 + 1] = source[column * 3 + 1];
        row_data[column * 3 + 2] = source[column * 3];
      }
      row_pointer[0] = row_data;
    } else {
      row_pointer[0] = (JSAMPROW) source;
    }
    jpeg_write_scanlines(&compressor, row_pointer, 1);
  }
  jpeg_finish_compress(&compressor);
  jpeg_destroy_compress(&compressor);
  free(row_data);
  payload->data = jpeg_data;
  payload->size = (size_t) jpeg_size;
  payload->format = STREAM_FORMAT_MJPEG;
  payload->owned_data = jpeg_data;
  return 1;
}

static int get_frame_payload(const capture_options_t *options, ArvBuffer *buffer,
    frame_payload_t *payload) {
  memset(payload, 0, sizeof(*payload));
  if (options->format == OUTPUT_FORMAT_MJPEG)
    return encode_jpeg(buffer, payload);
  payload->data = arv_buffer_get_data(buffer, &payload->size);
  payload->format = (uint32_t) arv_buffer_get_image_pixel_format(buffer);
  return 1;
}

static int save_frame(const capture_options_t *options, ArvBuffer *buffer,
    const frame_payload_t *payload) {
  char filename[512];
  FILE *file;
  uint64_t timestamp = arv_buffer_get_system_timestamp(buffer);
  if (!options->out_dir) return 1;
  if (snprintf(filename, sizeof(filename), "%s/frame_%06llu_%llu.%s",
      options->out_dir, (unsigned long long) arv_buffer_get_frame_id(buffer),
      (unsigned long long) timestamp,
      options->format == OUTPUT_FORMAT_MJPEG ? "jpg" : "raw") >= (int) sizeof(filename)) {
    fprintf(stderr, "Output path is too long\n");
    return 0;
  }
  file = fopen(filename, "wb");
  if (!file) {
    perror(filename);
    return 0;
  }
  if (fwrite(payload->data, 1, payload->size, file) != payload->size) {
    perror(filename);
    fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static int stream_frame(ArvBuffer *buffer, const frame_payload_t *payload) {
  gint width, height, x, y;
  uint64_t timestamp = arv_buffer_get_system_timestamp(buffer);
  stream_record_header_t header = {{'A', 'R', 'V', 'F'},
    (uint32_t) arv_buffer_get_frame_id(buffer), timestamp / 1000000000ULL,
    (uint32_t) ((timestamp / 1000000ULL) % 1000), 0, 0,
    payload->format, payload->size, 0};
  arv_buffer_get_image_region(buffer, &x, &y, &width, &height);
  header.width = (uint32_t) width;
  header.height = (uint32_t) height;
  (void) x;
  (void) y;
  return fwrite(&header, sizeof(header), 1, stdout) == 1 &&
    fwrite(payload->data, 1, payload->size, stdout) == payload->size &&
    fflush(stdout) == 0;
}

static int configure_camera(ArvCamera *camera, const capture_options_t *options) {
  GError *error = NULL;
  gint x, y;
  gint sensor_width, sensor_height;
  gint width, height;
  double frame_rate_min, frame_rate_max;
  double exposure_min, exposure_max, exposure_increment;
  width = options->width;
  height = options->height;
  if (width || height) {
    arv_camera_get_region(camera, &x, &y, &sensor_width, &sensor_height, &error);
    if (!check_error(&error, "get region")) return 0;
    if (!width) width = sensor_width;
    if (!height) height = sensor_height;
    arv_camera_set_region(camera, 0, 0, width, height, &error);
    if (!check_error(&error, "set region")) return 0;
  }
  if (options->fps) {
    if (!arv_camera_is_frame_rate_available(camera, &error) ||
        !check_error(&error, "check frame rate availability")) return 0;
    arv_camera_get_frame_rate_bounds(camera, &frame_rate_min, &frame_rate_max, &error);
    if (!check_error(&error, "get frame rate bounds") ||
        options->fps < frame_rate_min || options->fps > frame_rate_max) {
      fprintf(stderr, "Requested frame rate %d is outside camera range %.3f-%.3f\n",
        options->fps, frame_rate_min, frame_rate_max);
      return 0;
    }
    arv_camera_set_frame_rate_enable(camera, TRUE, &error);
    arv_camera_set_frame_rate(camera, options->fps, &error);
    if (!check_error(&error, "set frame rate")) return 0;
  }
  if (options->exposure_specified) {
    if (!arv_camera_is_exposure_time_available(camera, &error) ||
        !check_error(&error, "check exposure availability")) return 0;
    if (options->exposure_auto) {
      arv_camera_set_exposure_time_auto(camera, ARV_AUTO_CONTINUOUS, &error);
    } else {
      const double requested_us = options->exposure * 100.0;
      arv_camera_get_exposure_time_bounds(camera, &exposure_min, &exposure_max, &error);
      exposure_increment = arv_camera_get_exposure_time_increment(camera, &error);
      {
        const double increment_count =
          exposure_increment > 0.0 ?
            (requested_us - exposure_min) / exposure_increment : 0.0;
        const int increment_valid = exposure_increment <= 0.0 ||
          fabs(increment_count - round(increment_count)) <= 1e-6;
      if (!check_error(&error, "get exposure range") || requested_us < exposure_min ||
          requested_us > exposure_max ||
          !increment_valid) {
        fprintf(stderr, "Exposure %u is outside camera range or resolution "
          "(%.3f-%.3f us, increment %.3f us)\n", options->exposure,
          exposure_min, exposure_max, exposure_increment);
        return 0;
      }
      }
      arv_camera_set_exposure_time_auto(camera, ARV_AUTO_OFF, &error);
      if (!check_error(&error, "set manual exposure mode")) return 0;
      arv_camera_set_exposure_time(camera, requested_us, &error);
    }
    if (!check_error(&error, "set exposure")) return 0;
  }
  arv_camera_set_acquisition_mode(camera, ARV_ACQUISITION_MODE_CONTINUOUS, &error);
  if (!check_error(&error, "set continuous acquisition mode")) return 0;
  if (options->debug)
    fprintf(stderr, "[debug] requested stream: %dx%d at %d fps, pixel format %s\n",
      width, height, options->fps,
      arv_camera_get_pixel_format_as_string(camera, &error));
  return check_error(&error, "get pixel format");
}

int main(int argc, char **argv) {
  capture_options_t options;
  ArvCamera *camera = NULL;
  ArvStream *stream = NULL;
  ArvBuffer *buffer = NULL;
  GError *error = NULL;
  const char *device_id;
  int exit_code = 1;

  if (!parse_options(argc, argv, &options)) {
    usage(argv[0]);
    return 2;
  }
  if (!create_output_directory(options.out_dir)) return 2;
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  if (options.list) {
    print_devices();
    if (!options.name) return 0;
  }
  device_id = find_device_id(options.name);
  if (!device_id) {
    fprintf(stderr, "camera not found: %s\n", options.name);
    return 1;
  }
  debug_log(&options, "opening camera");
  camera = arv_camera_new(device_id, &error);
  if (!camera || !check_error(&error, "open camera")) goto cleanup;
  if (!configure_camera(camera, &options)) goto cleanup;
  stream = arv_camera_create_stream(camera, NULL, NULL, &error);
  if (!stream || !check_error(&error, "create stream")) goto cleanup;
  {
    const guint payload_size = arv_camera_get_payload(camera, &error);
    if (!check_error(&error, "get stream payload size")) goto cleanup;
    for (unsigned int index = 0; index < 16; ++index)
      arv_stream_push_buffer(stream, arv_buffer_new_allocate(payload_size));
  }
#ifdef ARAVIS_CLI_HAS_PREVIEW
  if (options.preview && !preview_init("aravis stream preview")) goto cleanup;
#endif
  arv_camera_start_acquisition(camera, &error);
  if (!check_error(&error, "start acquisition")) goto cleanup;
  debug_log(&options, "stream started");
  while (keep_running) {
    frame_payload_t payload;
    buffer = arv_stream_timeout_pop_buffer(stream, 100000);
    if (!buffer) {
      continue;
    }
    if (arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS) {
      arv_stream_push_buffer(stream, buffer);
      buffer = NULL;
      continue;
    }
#ifdef ARAVIS_CLI_HAS_PREVIEW
    if (options.preview && preview_frame(buffer)) keep_running = 0;
#endif
    if (!get_frame_payload(&options, buffer, &payload) ||
        !save_frame(&options, buffer, &payload) ||
        (options.stream && !stream_frame(buffer, &payload)))
      keep_running = 0;
    free(payload.owned_data);
    arv_stream_push_buffer(stream, buffer);
    buffer = NULL;
  }
  arv_camera_stop_acquisition(camera, &error);
  if (!check_error(&error, "stop acquisition")) goto cleanup;
  exit_code = 0;
cleanup:
#ifdef ARAVIS_CLI_HAS_PREVIEW
  if (options.preview) preview_close();
#endif
  if (buffer) g_object_unref(buffer);
  if (stream) g_object_unref(stream);
  if (camera) {
    if (error) g_clear_error(&error);
    g_object_unref(camera);
  }
  return exit_code;
}
