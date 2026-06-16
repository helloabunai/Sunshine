/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <cstdlib>

// platform includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xf86drm.h>

// local includes
#include "graphics.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  // Define buffer params listener
  static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_t::buffer_params_created,
    .failed = dmabuf_t::buffer_params_failed
  };

  int display_t::init(const char *display_name) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }

    if (!display_name) {
      BOOST_LOG(error) << "[wayland] Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "[wayland] Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "[wayland] Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout The timeout in milliseconds.
   * @return `true` if new events were dispatched or `false` if the timeout expired.
   */
  bool display_t::dispatch(std::chrono::milliseconds timeout) {
    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      if (poll(&pfd, 1, timeout.count()) == 1 && (pfd.revents & POLLIN)) {
        // Read the new event(s)
        wl_display_read_events(display_internal.get());
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events
    wl_display_dispatch_pending(display_internal.get());
    return true;
  }

  wl_registry *display_t::registry() {
    return wl_display_get_registry(display_internal.get());
  }

  inline monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        &CLASS_CALL(monitor_t, wl_geometry),
        &CLASS_CALL(monitor_t, wl_mode),
        &CLASS_CALL(monitor_t, wl_done),
        &CLASS_CALL(monitor_t, wl_scale),
      },
      xdg_listener {
        &CLASS_CALL(monitor_t, xdg_position),
        &CLASS_CALL(monitor_t, xdg_size),
        &CLASS_CALL(monitor_t, xdg_done),
        &CLASS_CALL(monitor_t, xdg_name),
        &CLASS_CALL(monitor_t, xdg_description)
      },
      cm_output_listener {
        .image_description_changed = &CLASS_CALL(monitor_t, cm_image_description_changed),
      },
      cm_image_desc_listener {
        .failed = &CLASS_CALL(monitor_t, cm_failed),
        .ready = &CLASS_CALL(monitor_t, cm_ready),
        .ready2 = &CLASS_CALL(monitor_t, cm_ready2),
      },
      cm_info_listener {
        .done = &CLASS_CALL(monitor_t, info_done),
        .icc_file = &CLASS_CALL(monitor_t, info_icc_file),
        .primaries = &CLASS_CALL(monitor_t, info_primaries),
        .primaries_named = &CLASS_CALL(monitor_t, info_primaries_named),
        .tf_power = &CLASS_CALL(monitor_t, info_tf_power),
        .tf_named = &CLASS_CALL(monitor_t, info_tf_named),
        .luminances = &CLASS_CALL(monitor_t, info_luminances),
        .target_primaries = &CLASS_CALL(monitor_t, info_target_primaries),
        .target_luminance = &CLASS_CALL(monitor_t, info_target_luminance),
        .target_max_cll = &CLASS_CALL(monitor_t, info_target_max_cll),
        .target_max_fall = &CLASS_CALL(monitor_t, info_target_max_fall),
      } {
  }

  inline void monitor_t::xdg_name(zxdg_output_v1 *, const char *name) {
    this->name = name;

    BOOST_LOG(info) << "[wayland] Name: "sv << this->name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    this->description = description;

    BOOST_LOG(info) << "[wayland] Found monitor: "sv << this->description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    viewport.offset_x = x;
    viewport.offset_y = y;

    BOOST_LOG(info) << "[wayland] Offset: "sv << x << 'x' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    viewport.logical_width = width;
    viewport.logical_height = height;
    BOOST_LOG(info) << "[wayland] Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    viewport.width = width;
    viewport.height = height;

    BOOST_LOG(info) << "[wayland] Resolution: "sv << width << 'x' << height;
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    auto xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  // --- Color management / HDR ---

  void monitor_t::listen_color(wp_color_manager_v1 *color_manager) {
    if (!color_manager) {
      return;
    }

    cm_query_complete = false;
    hdr = false;

    // The wp_color_management_output_v1 stays alive for the lifetime of the
    // monitor so we keep receiving image_description_changed events. The image
    // description itself is transient: we query it, read the info, and drop it.
    cm_output = wp_color_manager_v1_get_output(color_manager, output);
    wp_color_management_output_v1_add_listener(cm_output, &cm_output_listener, this);

    cm_image_desc = wp_color_management_output_v1_get_image_description(cm_output);
    wp_image_description_v1_add_listener(cm_image_desc, &cm_image_desc_listener, this);
  }

  void monitor_t::cm_image_description_changed(wp_color_management_output_v1 *) {
    BOOST_LOG(info) << "[wayland] Output color/HDR image description changed"sv;
    image_description_changed = true;
  }

  void monitor_t::cm_request_info(wp_image_description_v1 *image_description) {
    // Reset accumulators before a fresh dump.
    cm_tf = 0;
    cm_has_primaries = false;
    cm_has_target_primaries = false;
    cm_has_target_luminance = false;
    cm_lum_max = cm_lum_min = 0;
    cm_target_lum_max = cm_target_lum_min = 0;
    cm_max_cll = cm_max_fall = 0;

    auto info = wp_image_description_v1_get_information(image_description);
    wp_image_description_info_v1_add_listener(info, &cm_info_listener, this);
  }

  void monitor_t::cm_ready(wp_image_description_v1 *image_description, std::uint32_t) {
    cm_request_info(image_description);
  }

  void monitor_t::cm_ready2(wp_image_description_v1 *image_description, std::uint32_t, std::uint32_t) {
    cm_request_info(image_description);
  }

  void monitor_t::cm_failed(wp_image_description_v1 *, std::uint32_t cause, const char *msg) {
    BOOST_LOG(warning) << "[wayland] Output image description unavailable (cause "sv << cause << "): "sv << (msg ? msg : "");
    hdr = false;
    cm_query_complete = true;
  }

  void monitor_t::info_icc_file(wp_image_description_info_v1 *, std::int32_t icc, std::uint32_t) {
    // We don't use ICC profiles; close the fd so it isn't leaked.
    if (icc >= 0) {
      close(icc);
    }
  }

  void monitor_t::info_primaries(wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
    cm_primaries[0] = r_x;
    cm_primaries[1] = r_y;
    cm_primaries[2] = g_x;
    cm_primaries[3] = g_y;
    cm_primaries[4] = b_x;
    cm_primaries[5] = b_y;
    cm_primaries[6] = w_x;
    cm_primaries[7] = w_y;
    cm_has_primaries = true;
  }

  void monitor_t::info_primaries_named(wp_image_description_info_v1 *, std::uint32_t) {
    // We always have the explicit chromaticities from info_primaries.
  }

  void monitor_t::info_tf_power(wp_image_description_info_v1 *, std::uint32_t) {
    // HDR uses the named ST 2084 PQ transfer function, handled in info_tf_named.
  }

  void monitor_t::info_tf_named(wp_image_description_info_v1 *, std::uint32_t tf) {
    cm_tf = tf;
  }

  void monitor_t::info_luminances(wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum, std::uint32_t) {
    cm_lum_min = min_lum;
    cm_lum_max = max_lum;
  }

  void monitor_t::info_target_primaries(wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
    cm_target_primaries[0] = r_x;
    cm_target_primaries[1] = r_y;
    cm_target_primaries[2] = g_x;
    cm_target_primaries[3] = g_y;
    cm_target_primaries[4] = b_x;
    cm_target_primaries[5] = b_y;
    cm_target_primaries[6] = w_x;
    cm_target_primaries[7] = w_y;
    cm_has_target_primaries = true;
  }

  void monitor_t::info_target_luminance(wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum) {
    cm_target_lum_min = min_lum;
    cm_target_lum_max = max_lum;
    cm_has_target_luminance = true;
  }

  void monitor_t::info_target_max_cll(wp_image_description_info_v1 *, std::uint32_t max_cll) {
    cm_max_cll = max_cll;
  }

  void monitor_t::info_target_max_fall(wp_image_description_info_v1 *, std::uint32_t max_fall) {
    cm_max_fall = max_fall;
  }

  void monitor_t::info_done(wp_image_description_info_v1 *) {
    // 'done' is a destructor event: libwayland frees the info proxy for us.

    // We only treat ST 2084 PQ as streamable HDR; that matches what the encoder
    // pipeline supports (BT.2020 + SMPTE 2084). HLG is reported but unsupported.
    hdr = (cm_tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    if (cm_tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG) {
      BOOST_LOG(warning) << "[wayland] Output uses HLG HDR, which is not supported for streaming; treating as SDR"sv;
    }

    if (hdr) {
      // SS_HDR_METADATA expects the mastering-display (target) volume. Prefer the
      // target primaries/luminances; fall back to the primary color volume when the
      // compositor omits them (it only sends target_* when they differ).
      const std::int32_t *p = cm_has_target_primaries ? cm_target_primaries : cm_primaries;

      // Protocol chromaticities are CIE xy * 1,000,000; SS_HDR_METADATA wants
      // them normalized to 50,000, i.e. value / 20.
      auto norm = [](std::int32_t v) -> std::uint16_t {
        long n = static_cast<long>(v) / 20;
        if (n < 0) {
          n = 0;
        }
        if (n > 65535) {
          n = 65535;
        }
        return static_cast<std::uint16_t>(n);
      };
      auto clamp16 = [](std::uint32_t v) -> std::uint16_t {
        return static_cast<std::uint16_t>(v > 65535 ? 65535 : v);
      };

      hdr_metadata = {};
      hdr_metadata.displayPrimaries[0].x = norm(p[0]);
      hdr_metadata.displayPrimaries[0].y = norm(p[1]);
      hdr_metadata.displayPrimaries[1].x = norm(p[2]);
      hdr_metadata.displayPrimaries[1].y = norm(p[3]);
      hdr_metadata.displayPrimaries[2].x = norm(p[4]);
      hdr_metadata.displayPrimaries[2].y = norm(p[5]);
      hdr_metadata.whitePoint.x = norm(p[6]);
      hdr_metadata.whitePoint.y = norm(p[7]);

      // maxDisplayLuminance is in nits (protocol max_lum is unscaled cd/m²).
      // minDisplayLuminance is in 1/10000 nit, which matches the protocol's
      // min_lum scaling (cd/m² * 10000) exactly.
      std::uint32_t max_lum = cm_has_target_luminance ? cm_target_lum_max : cm_lum_max;
      std::uint32_t min_lum = cm_has_target_luminance ? cm_target_lum_min : cm_lum_min;
      hdr_metadata.maxDisplayLuminance = clamp16(max_lum);
      hdr_metadata.minDisplayLuminance = clamp16(min_lum);
      hdr_metadata.maxContentLightLevel = clamp16(cm_max_cll);
      hdr_metadata.maxFrameAverageLightLevel = clamp16(cm_max_fall);
      hdr_metadata.maxFullFrameLuminance = 0;

      BOOST_LOG(info) << "[wayland] HDR output detected (PQ/ST2084), peak "sv << hdr_metadata.maxDisplayLuminance << " nits"sv;
    }

    cm_query_complete = true;

    // The image description has been consumed; release it. The output object is
    // kept so we still receive image_description_changed events.
    if (cm_image_desc) {
      wp_image_description_v1_destroy(cm_image_desc);
      cm_image_desc = nullptr;
    }
  }

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      },
      dmabuf_listener {
        &CLASS_CALL(interface_t, dmabuf_format),
        &CLASS_CALL(interface_t, dmabuf_modifier)
      } {
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::dmabuf_format(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format) {
  }

  void interface_t::dmabuf_modifier(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
    uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;
    supported_modifiers[format].push_back(modifier);
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "[wayland] Available interface: "sv << interface << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface, wl_output_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 2)
        )
      );
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, version);

      this->interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);

      this->interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface << '(' << id << ") version "sv << version;
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u));
      zwp_linux_dmabuf_v1_add_listener(dmabuf_interface, &dmabuf_listener, this);

      this->interface[LINUX_DMABUF] = true;
    } else if (!std::strcmp(interface, wp_color_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface << '(' << id << ") version "sv << version;
      // We only rely on v1 events (image description info), but bind up to v2
      // so v2-only compositors stay compatible (they emit ready2 instead of ready).
      color_manager = (wp_color_manager_v1 *) wl_registry_bind(registry, id, &wp_color_manager_v1_interface, std::min(version, 2u));

      this->interface[COLOR_MANAGEMENT] = true;
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "[wayland] Delete: "sv << id;
  }

  // Initialize GBM
  bool dmabuf_t::init_gbm() {
    if (gbm_device) {
      return true;
    }

    auto render_path = platf::resolve_render_device();
    int drm_fd = open(render_path.c_str(), O_RDWR);
    if (drm_fd < 0) {
      BOOST_LOG(error) << "[wayland] Failed to open DRM render node: "sv << render_path;
      return false;
    }

    gbm_device = gbm_create_device(drm_fd);
    if (!gbm_device) {
      close(drm_fd);
      BOOST_LOG(error) << "[wayland] Failed to create GBM device"sv;
      return false;
    }

    return true;
  }

  // Cleanup GBM
  void dmabuf_t::cleanup_gbm() {
    if (current_bo) {
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
    }

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
  }

  dmabuf_t::dmabuf_t():
      status {READY},
      frames {},
      current_frame {&frames[0]},
      listener {
        &CLASS_CALL(dmabuf_t, buffer),
        &CLASS_CALL(dmabuf_t, flags),
        &CLASS_CALL(dmabuf_t, ready),
        &CLASS_CALL(dmabuf_t, failed),
        &CLASS_CALL(dmabuf_t, damage),
        &CLASS_CALL(dmabuf_t, linux_dmabuf),
        &CLASS_CALL(dmabuf_t, buffer_done),
      } {
  }

  // Start capture
  void dmabuf_t::listen(
    zwlr_screencopy_manager_v1 *screencopy_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *supported_modifiers,
    wl_output *output,
    bool blend_cursor
  ) {
    this->dmabuf_interface = dmabuf_interface;
    this->supported_modifiers = supported_modifiers;
    // Reset state
    shm_info.supported = false;
    dmabuf_info.supported = false;

    // Create new frame
    auto frame = zwlr_screencopy_manager_v1_capture_output(
      screencopy_manager,
      blend_cursor ? 1 : 0,
      output
    );

    // Store frame data pointer for callbacks
    zwlr_screencopy_frame_v1_set_user_data(frame, this);

    // Add listener
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
  }

  dmabuf_t::~dmabuf_t() {
    cleanup_gbm();

    for (auto &frame : frames) {
      frame.destroy();
    }

    if (gbm_device) {
      // We should close the DRM FD, but it's owned by GBM
      gbm_device_destroy(gbm_device);
      gbm_device = nullptr;
    }
  }

  // Buffer format callback
  void dmabuf_t::buffer(
    zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride
  ) {
    shm_info.supported = true;
    shm_info.format = format;
    shm_info.width = width;
    shm_info.height = height;
    shm_info.stride = stride;
  }

  // DMA-BUF format callback
  void dmabuf_t::linux_dmabuf(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height
  ) {
    dmabuf_info.supported = true;
    dmabuf_info.format = format;
    dmabuf_info.width = width;
    dmabuf_info.height = height;
  }

  // Flags callback
  void dmabuf_t::flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags) {
    y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    BOOST_LOG(verbose) << "Frame flags: "sv << flags << (y_invert ? " (y_invert)" : "");
  }

  // DMA-BUF creation helper
  void dmabuf_t::create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame) {
    if (!init_gbm()) {
      BOOST_LOG(error) << "Failed to initialize GBM"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Create GBM buffer
    if (supported_modifiers) {
      auto it = supported_modifiers->find(dmabuf_info.format);
      if (it != supported_modifiers->end() && !it->second.empty()) {
        current_bo = gbm_bo_create_with_modifiers(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, it->second.data(), it->second.size());
      }
    }

    if (!current_bo) {
      current_bo = gbm_bo_create(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, GBM_BO_USE_RENDERING);
    }

    if (!current_bo) {
      BOOST_LOG(error) << "Failed to create GBM buffer"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Get buffer info
    int fd = gbm_bo_get_fd(current_bo);
    if (fd < 0) {
      BOOST_LOG(error) << "Failed to get buffer FD"sv;
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    uint32_t stride = gbm_bo_get_stride(current_bo);
    uint64_t modifier = gbm_bo_get_modifier(current_bo);

    // Store in surface descriptor for later use
    auto next_frame = get_next_frame();
    next_frame->sd.fds[0] = fd;
    next_frame->sd.pitches[0] = stride;
    next_frame->sd.offsets[0] = 0;
    next_frame->sd.modifier = modifier;

    // Create linux-dmabuf buffer
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    // Add listener for buffer creation
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, frame);

    // Create Wayland buffer (async - callback will handle copy)
    zwp_linux_buffer_params_v1_create(params, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, 0);
  }

  // Buffer done callback - time to create buffer
  void dmabuf_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();

    // Prefer DMA-BUF if supported
    if (dmabuf_info.supported && dmabuf_interface) {
      // Store format info first
      next_frame->sd.fourcc = dmabuf_info.format;
      next_frame->sd.width = dmabuf_info.width;
      next_frame->sd.height = dmabuf_info.height;

      // Create and start copy
      create_and_copy_dmabuf(frame);
    } else if (shm_info.supported) {
      // SHM fallback would go here
      BOOST_LOG(warning) << "[wayland] SHM capture not implemented"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    } else {
      BOOST_LOG(error) << "[wayland] No supported buffer types"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    }
  }

  // Buffer params created callback
  void dmabuf_t::buffer_params_created(
    void *data,
    struct zwp_linux_buffer_params_v1 *params,
    struct wl_buffer *buffer
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    // Store for cleanup
    self->current_wl_buffer = buffer;

    // Start the actual copy
    zwp_linux_buffer_params_v1_destroy(params);
    zwlr_screencopy_frame_v1_copy(frame, buffer);
  }

  // Buffer params failed callback
  void dmabuf_t::buffer_params_failed(
    void *data,
    struct zwp_linux_buffer_params_v1 *params
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    BOOST_LOG(error) << "[wayland] Failed to create buffer from params"sv;
    self->cleanup_gbm();

    zwp_linux_buffer_params_v1_destroy(params);
    zwlr_screencopy_frame_v1_destroy(frame);
    self->status = REINIT;
  }

  // Ready callback
  void dmabuf_t::ready(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {
    // Frame is ready for use, GBM buffer now contains screen content
    current_frame->destroy();
    current_frame = get_next_frame();

    std::uint64_t sec = (std::uint64_t(tv_sec_hi) << 32) | tv_sec_lo;
    auto ready_ts = std::chrono::seconds(sec) + std::chrono::nanoseconds(tv_nsec);
    current_frame->frame_timestamp = std::chrono::steady_clock::time_point {
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(ready_ts)
    };

    // Keep the GBM buffer alive but destroy the Wayland objects
    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }

    cleanup_gbm();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = READY;
  }

  // Failed callback
  void dmabuf_t::failed(zwlr_screencopy_frame_v1 *frame) {
    BOOST_LOG(error) << "[wayland] Frame capture failed"sv;

    // Clean up resources
    cleanup_gbm();
    auto next_frame = get_next_frame();
    next_frame->destroy();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = REINIT;
  }

  // Only called if using zwlr_screencopy_frame_v1_copy_with_damage()
  void dmabuf_t::damage(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height
  ) {};

  void frame_t::destroy() {
    for (auto x = 0; x < 4; ++x) {
      if (sd.fds[x] >= 0) {
        close(sd.fds[x]);

        sd.fds[x] = -1;
      }
    }
  }

  frame_t::frame_t() {
    // File descriptors aren't open
    std::fill_n(sd.fds, 4, -1);
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "[wayland] Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  static bool validate() {
    display_t display;

    return display.init() == 0;
  }

  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop
