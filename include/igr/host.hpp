#pragma once

namespace igr {

enum class HostMode {
  owned_window,
  external_swap_chain,
  injected_overlay,
};

enum class PresentationMode {
  backend_managed,
  host_managed,
};

enum class ResizeMode {
  backend_managed,
  host_managed,
};

enum class InputMode {
  none,
  external_forwarded,
  subclassed_window_proc,
};

struct BackendHostOptions {
  HostMode host_mode{HostMode::owned_window};
  PresentationMode presentation_mode{PresentationMode::backend_managed};
  ResizeMode resize_mode{ResizeMode::backend_managed};
  InputMode input_mode{InputMode::external_forwarded};
  bool clear_target{true};
  bool restore_host_state{true};
};

}  // namespace igr
