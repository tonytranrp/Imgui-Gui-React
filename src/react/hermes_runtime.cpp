#include "igr/react/hermes_runtime.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if IGR_ENABLE_HERMES
#ifdef _WIN32
#define NAPI_EXTERN __declspec(dllimport)
#endif
#include <hermes/hermes_api.h>
#endif

namespace igr::react {
namespace {

#if IGR_ENABLE_HERMES

Status read_text_file(const std::filesystem::path& path, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("Hermes text file read requires a valid output string.");
  }

  std::ifstream stream(path, std::ios::in | std::ios::binary);
  if (!stream.is_open()) {
    return Status::invalid_argument("Failed to open Hermes bundle: " + path.string());
  }

  output->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return Status::success();
}

Status read_binary_file(const std::filesystem::path& path, std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return Status::invalid_argument("Hermes binary file read requires a valid output buffer.");
  }

  std::ifstream stream(path, std::ios::in | std::ios::binary);
  if (!stream.is_open()) {
    return Status::invalid_argument("Failed to open Hermes bytecode bundle: " + path.string());
  }

  output->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return Status::success();
}

bool try_read_utf8_string(napi_env env, napi_value value, std::string* output) {
  if (env == nullptr || value == nullptr || output == nullptr) {
    return false;
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return false;
  }

  std::string text(length, '\0');
  size_t written = 0;
  if (napi_get_value_string_utf8(env, value, text.data(), text.size() + 1, &written) != napi_ok) {
    return false;
  }

  text.resize(written);
  *output = std::move(text);
  return true;
}

bool try_coerce_to_utf8(napi_env env, napi_value value, std::string* output) {
  napi_value string_value{};
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok) {
    return false;
  }
  return try_read_utf8_string(env, string_value, output);
}

std::string describe_pending_exception(napi_env env) {
  if (env == nullptr) {
    return {};
  }

  bool has_exception = false;
  if (napi_is_exception_pending(env, &has_exception) != napi_ok || !has_exception) {
    return {};
  }

  napi_value exception{};
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok) {
    return "Hermes runtime threw a JavaScript exception.";
  }

  std::string text;
  if (try_coerce_to_utf8(env, exception, &text) && !text.empty()) {
    return text;
  }

  return "Hermes runtime threw a JavaScript exception.";
}

Status status_from_napi(napi_env env, napi_status status, std::string_view action) {
  if (status == napi_ok) {
    return Status::success();
  }

  std::string message(action);
  const std::string exception_text = describe_pending_exception(env);
  if (!exception_text.empty()) {
    message += ": ";
    message += exception_text;
    return Status::backend_error(std::move(message));
  }

  const napi_extended_error_info* error_info = nullptr;
  if (env != nullptr && napi_get_last_error_info(env, &error_info) == napi_ok && error_info != nullptr && error_info->error_message != nullptr) {
    message += ": ";
    message += error_info->error_message;
  } else {
    message += " (napi_status=" + std::to_string(static_cast<int>(status)) + ")";
  }

  return Status::backend_error(std::move(message));
}

class ScopedEnvScope {
 public:
  explicit ScopedEnvScope(napi_env env) : env_(env) {
    if (env_ == nullptr) {
      status_ = Status::invalid_argument("Hermes runtime did not expose a valid Node-API environment.");
      return;
    }

    status_ = status_from_napi(env_, jsr_open_napi_env_scope(env_, &scope_), "Failed to open the Hermes Node-API scope");
    opened_ = static_cast<bool>(status_);
  }

  ~ScopedEnvScope() {
    if (opened_) {
      jsr_close_napi_env_scope(env_, scope_);
    }
  }

  [[nodiscard]] const Status& status() const noexcept {
    return status_;
  }

 private:
  napi_env env_{};
  jsr_napi_env_scope scope_{};
  Status status_{Status::success()};
  bool opened_{false};
};

class ScopedHandleScope {
 public:
  explicit ScopedHandleScope(napi_env env) : env_(env) {
    if (env_ == nullptr) {
      status_ = Status::invalid_argument("Hermes runtime did not expose a valid Node-API environment.");
      return;
    }

    status_ = status_from_napi(env_, napi_open_handle_scope(env_, &scope_), "Failed to open the Hermes handle scope");
    opened_ = static_cast<bool>(status_);
  }

  ~ScopedHandleScope() {
    if (opened_) {
      napi_close_handle_scope(env_, scope_);
    }
  }

  [[nodiscard]] const Status& status() const noexcept {
    return status_;
  }

 private:
  napi_env env_{};
  napi_handle_scope scope_{};
  Status status_{Status::success()};
  bool opened_{false};
};

void noop_script_delete(void*, void*) {}

Status set_named_number(napi_env env, napi_value object, const char* name, double value) {
  napi_value number_value{};
  Status status = status_from_napi(env, napi_create_double(env, value, &number_value), std::string("Failed to create the Hermes request property '") + name + "'");
  if (!status) {
    return status;
  }
  return status_from_napi(env, napi_set_named_property(env, object, name, number_value),
                          std::string("Failed to set the Hermes request property '") + name + "'");
}

Status set_named_uint32(napi_env env, napi_value object, const char* name, std::uint32_t value) {
  napi_value number_value{};
  Status status = status_from_napi(env, napi_create_uint32(env, value, &number_value), std::string("Failed to create the Hermes request property '") + name + "'");
  if (!status) {
    return status;
  }
  return status_from_napi(env, napi_set_named_property(env, object, name, number_value),
                          std::string("Failed to set the Hermes request property '") + name + "'");
}

Status create_runtime_request(napi_env env, const RuntimeFrameRequest& request, napi_value* result) {
  if (result == nullptr) {
    return Status::invalid_argument("Hermes runtime request creation requires a valid output value.");
  }

  napi_value request_object{};
  Status status = status_from_napi(env, napi_create_object(env, &request_object), "Failed to create the Hermes frame request object");
  if (!status) {
    return status;
  }

  status = set_named_number(env, request_object, "sequence", static_cast<double>(request.frame.frame_index));
  if (!status) {
    return status;
  }
  status = set_named_number(env, request_object, "frameIndex", static_cast<double>(request.frame.frame_index));
  if (!status) {
    return status;
  }
  status = set_named_number(env, request_object, "deltaSeconds", request.frame.delta_seconds);
  if (!status) {
    return status;
  }

  napi_value viewport{};
  status = status_from_napi(env, napi_create_object(env, &viewport), "Failed to create the Hermes frame viewport object");
  if (!status) {
    return status;
  }
  status = set_named_uint32(env, viewport, "width", request.frame.viewport.width);
  if (!status) {
    return status;
  }
  status = set_named_uint32(env, viewport, "height", request.frame.viewport.height);
  if (!status) {
    return status;
  }
  status = status_from_napi(env, napi_set_named_property(env, request_object, "viewport", viewport),
                            "Failed to attach the viewport object to the Hermes request");
  if (!status) {
    return status;
  }

  napi_value frame_object{};
  status = status_from_napi(env, napi_create_object(env, &frame_object), "Failed to create the nested Hermes frame object");
  if (!status) {
    return status;
  }
  status = set_named_number(env, frame_object, "frameIndex", static_cast<double>(request.frame.frame_index));
  if (!status) {
    return status;
  }
  status = set_named_number(env, frame_object, "deltaSeconds", request.frame.delta_seconds);
  if (!status) {
    return status;
  }
  status = status_from_napi(env, napi_set_named_property(env, frame_object, "viewport", viewport),
                            "Failed to attach the viewport object to the nested Hermes frame");
  if (!status) {
    return status;
  }
  status = status_from_napi(env, napi_set_named_property(env, request_object, "frame", frame_object),
                            "Failed to attach the nested frame object to the Hermes request");
  if (!status) {
    return status;
  }

  *result = request_object;
  return Status::success();
}

Status drain_microtasks(napi_env env) {
  bool did_run = false;
  return status_from_napi(env, jsr_drain_microtasks(env, 256, &did_run), "Failed to drain Hermes microtasks");
}

Status stringify_js_value(napi_env env, napi_value value, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("Hermes stringification requires a valid output string.");
  }

  napi_valuetype value_type = napi_undefined;
  Status status = status_from_napi(env, napi_typeof(env, value, &value_type), "Failed to inspect the Hermes return value");
  if (!status) {
    return status;
  }

  if (value_type == napi_string) {
    if (!try_read_utf8_string(env, value, output)) {
      return Status::backend_error("Failed to read the Hermes string return value.");
    }
    return Status::success();
  }

  napi_value global{};
  status = status_from_napi(env, napi_get_global(env, &global), "Failed to access Hermes globalThis during JSON serialization");
  if (!status) {
    return status;
  }

  napi_value json_object{};
  status = status_from_napi(env, napi_get_named_property(env, global, "JSON", &json_object), "Failed to access globalThis.JSON in Hermes");
  if (!status) {
    return status;
  }

  napi_value stringify_function{};
  status = status_from_napi(env, napi_get_named_property(env, json_object, "stringify", &stringify_function),
                            "Failed to access JSON.stringify in Hermes");
  if (!status) {
    return status;
  }

  napi_value serialized{};
  status = status_from_napi(env, napi_call_function(env, json_object, stringify_function, 1, &value, &serialized),
                            "Failed to serialize the Hermes return value with JSON.stringify");
  if (!status) {
    return status;
  }

  if (!try_read_utf8_string(env, serialized, output)) {
    return Status::backend_error("JSON.stringify returned a non-string Hermes value.");
  }
  return Status::success();
}

Status run_source_bundle(napi_env env, const std::filesystem::path& source_path) {
  std::string source_bundle;
  Status status = read_text_file(source_path, &source_bundle);
  if (!status) {
    return status;
  }

  napi_value script_source{};
  status = status_from_napi(env, napi_create_string_utf8(env, source_bundle.c_str(), source_bundle.size(), &script_source),
                            "Failed to create the Hermes source bundle string");
  if (!status) {
    return status;
  }

  napi_value ignored_result{};
  const std::string source_url = source_path.string();
  status = status_from_napi(env, jsr_run_script(env, script_source, source_url.c_str(), &ignored_result),
                            "Failed to execute the Hermes source bundle");
  if (!status) {
    return status;
  }

  return drain_microtasks(env);
}

Status resolve_entrypoint_reference(napi_env env, std::string_view configured_name, napi_ref* reference) {
  if (reference == nullptr) {
    return Status::invalid_argument("Hermes entrypoint resolution requires a valid reference output.");
  }

  napi_value global{};
  Status status = status_from_napi(env, napi_get_global(env, &global), "Failed to access Hermes globalThis");
  if (!status) {
    return status;
  }

  auto try_function = [&](napi_value owner, const char* name) -> Status {
    bool has_property = false;
    Status property_status = status_from_napi(env, napi_has_named_property(env, owner, name, &has_property),
                                              std::string("Failed to inspect Hermes property '") + name + "'");
    if (!property_status) {
      return property_status;
    }
    if (!has_property) {
      return Status::unsupported("");
    }

    napi_value value{};
    property_status = status_from_napi(env, napi_get_named_property(env, owner, name, &value),
                                       std::string("Failed to read Hermes property '") + name + "'");
    if (!property_status) {
      return property_status;
    }

    napi_valuetype value_type = napi_undefined;
    property_status = status_from_napi(env, napi_typeof(env, value, &value_type),
                                       std::string("Failed to inspect Hermes property '") + name + "'");
    if (!property_status) {
      return property_status;
    }
    if (value_type != napi_function) {
      return Status::unsupported("");
    }

    return status_from_napi(env, napi_create_reference(env, value, 1, reference),
                            std::string("Failed to retain a reference to the Hermes entrypoint '") + name + "'");
  };

  status = try_function(global, configured_name.data());
  if (status) {
    return status;
  }
  if (status.code() != StatusCode::unsupported) {
    return status;
  }

  bool has_bundle = false;
  status = status_from_napi(env, napi_has_named_property(env, global, "igrHermesBundle", &has_bundle),
                            "Failed to inspect the generated Hermes bundle namespace");
  if (!status) {
    return status;
  }
  if (has_bundle) {
    napi_value bundle_namespace{};
    status = status_from_napi(env, napi_get_named_property(env, global, "igrHermesBundle", &bundle_namespace),
                              "Failed to access the generated Hermes bundle namespace");
    if (!status) {
      return status;
    }

    status = try_function(bundle_namespace, configured_name.data());
    if (status) {
      return status;
    }
    if (status.code() != StatusCode::unsupported) {
      return status;
    }

    status = try_function(bundle_namespace, "renderTransportForHermes");
    if (status) {
      return status;
    }
    if (status.code() != StatusCode::unsupported) {
      return status;
    }
  }

  return Status::invalid_argument("Hermes transport entrypoint is not a function: " + std::string(configured_name));
}

#endif

}  // namespace

struct HermesTransportRuntime::Impl {
#if IGR_ENABLE_HERMES
  jsr_config config{};
  jsr_runtime runtime{};
  napi_env env{};
  napi_ref entrypoint_ref{};
  bool loaded_from_bytecode{false};
  std::string loaded_bundle_path;
#endif
};

HermesTransportRuntime::HermesTransportRuntime(HermesRuntimeConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

HermesTransportRuntime::~HermesTransportRuntime() {
  shutdown();
}

Status HermesTransportRuntime::initialize() {
#if !IGR_ENABLE_HERMES
  initialized_ = false;
  return Status::unsupported(
      "Hermes transport runtime is not compiled in for this build. Enable IGR_ENABLE_HERMES and provide the packaged Hermes runtime to "
      "igr::react::HermesTransportRuntime.");
#else
  if (initialized_) {
    return Status::success();
  }
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }

  if (config_.bundle.entrypoint.empty()) {
    return Status::invalid_argument("HermesTransportRuntime requires a non-empty JS entrypoint name.");
  }

  const std::filesystem::path bytecode_path(config_.bundle.bytecode_path);
  const std::filesystem::path source_path(config_.bundle.bundle_path);
  const bool source_bundle_available = !config_.bundle.bundle_path.empty() && std::filesystem::exists(source_path);
  const bool use_bytecode = config_.bundle.prefer_bytecode && !config_.bundle.bytecode_path.empty() && std::filesystem::exists(bytecode_path);
  if (!use_bytecode && !source_bundle_available) {
    return Status::invalid_argument(
        "HermesTransportRuntime could not find a readable bundle. Build artifacts/hermes/react-native-test.bundle.js or configure a valid bundle "
        "path.");
  }

  Status status = status_from_napi(nullptr, jsr_create_config(&impl_->config), "Failed to create the Hermes runtime config");
  if (!status) {
    shutdown();
    return status;
  }

  status = status_from_napi(nullptr, hermes_config_enable_default_crash_handler(impl_->config, false),
                            "Failed to configure the Hermes crash handler policy");
  if (!status) {
    shutdown();
    return status;
  }

  if (config_.enable_inspector) {
    status = status_from_napi(nullptr, jsr_config_enable_inspector(impl_->config, true), "Failed to enable the Hermes inspector");
    if (!status) {
      shutdown();
      return status;
    }
    status = status_from_napi(nullptr, jsr_config_set_inspector_runtime_name(impl_->config, "igr-hermes-runtime"),
                              "Failed to configure the Hermes inspector runtime name");
    if (!status) {
      shutdown();
      return status;
    }
  }

  status = status_from_napi(nullptr, jsr_create_runtime(impl_->config, &impl_->runtime), "Failed to create the Hermes runtime");
  if (!status) {
    shutdown();
    return status;
  }

  status = status_from_napi(nullptr, jsr_runtime_get_node_api_env(impl_->runtime, &impl_->env), "Failed to obtain the Hermes Node-API environment");
  if (!status) {
    shutdown();
    return status;
  }

  ScopedEnvScope env_scope(impl_->env);
  if (!env_scope.status()) {
    shutdown();
    return env_scope.status();
  }

  ScopedHandleScope handle_scope(impl_->env);
  if (!handle_scope.status()) {
    shutdown();
    return handle_scope.status();
  }

  if (use_bytecode) {
    std::vector<std::uint8_t> bytecode;
    status = read_binary_file(bytecode_path, &bytecode);
    if (!status) {
      shutdown();
      return status;
    }

    jsr_prepared_script prepared_script{};
    const std::string source_url = bytecode_path.string();
    status = status_from_napi(impl_->env,
                              jsr_create_prepared_script(impl_->env, bytecode.data(), bytecode.size(), noop_script_delete, nullptr,
                                                         source_url.c_str(), &prepared_script),
                              "Failed to create the prepared Hermes bytecode script");
    if (!status) {
      shutdown();
      return status;
    }

    napi_value ignored_result{};
    status = status_from_napi(impl_->env, jsr_prepared_script_run(impl_->env, prepared_script, &ignored_result),
                              "Failed to execute the prepared Hermes bytecode bundle");
    jsr_delete_prepared_script(impl_->env, prepared_script);
    if (!status) {
      shutdown();
      return status;
    }

    impl_->loaded_from_bytecode = true;
    impl_->loaded_bundle_path = bytecode_path.string();
  } else {
    status = run_source_bundle(impl_->env, source_path);
    if (!status) {
      shutdown();
      return status;
    }

    impl_->loaded_from_bytecode = false;
    impl_->loaded_bundle_path = source_path.string();
  }

  status = resolve_entrypoint_reference(impl_->env, config_.bundle.entrypoint, &impl_->entrypoint_ref);
  if (!status && impl_->loaded_from_bytecode && source_bundle_available) {
    status = run_source_bundle(impl_->env, source_path);
    if (!status) {
      shutdown();
      return status;
    }

    status = resolve_entrypoint_reference(impl_->env, config_.bundle.entrypoint, &impl_->entrypoint_ref);
    if (status) {
      impl_->loaded_from_bytecode = false;
      impl_->loaded_bundle_path = source_path.string();
    }
  }
  if (!status) {
    shutdown();
    return status;
  }

  initialized_ = true;
  return Status::success();
#endif
}

Status HermesTransportRuntime::render_transport(const RuntimeFrameRequest& request, RuntimeFrameResponse* response) {
#if !IGR_ENABLE_HERMES
  static_cast<void>(request);
  static_cast<void>(response);
  return Status::unsupported(
      "Hermes transport runtime is unavailable in this build. Enable IGR_ENABLE_HERMES and provide the packaged Hermes runtime.");
#else
  if (response == nullptr) {
    return Status::invalid_argument("HermesTransportRuntime requires a valid response output.");
  }
  if (!initialized_ || !impl_ || impl_->env == nullptr || impl_->entrypoint_ref == nullptr) {
    return Status::not_ready("HermesTransportRuntime must be initialized before render_transport.");
  }

  ScopedEnvScope env_scope(impl_->env);
  if (!env_scope.status()) {
    return env_scope.status();
  }

  ScopedHandleScope handle_scope(impl_->env);
  if (!handle_scope.status()) {
    return handle_scope.status();
  }

  napi_value global{};
  Status status = status_from_napi(impl_->env, napi_get_global(impl_->env, &global), "Failed to access Hermes globalThis");
  if (!status) {
    return status;
  }

  napi_value entrypoint{};
  status = status_from_napi(impl_->env, napi_get_reference_value(impl_->env, impl_->entrypoint_ref, &entrypoint),
                            "Failed to load the cached Hermes transport entrypoint");
  if (!status) {
    return status;
  }

  napi_value request_object{};
  status = create_runtime_request(impl_->env, request, &request_object);
  if (!status) {
    return status;
  }

  napi_value call_result{};
  status = status_from_napi(impl_->env, napi_call_function(impl_->env, global, entrypoint, 1, &request_object, &call_result),
                            "Hermes transport entrypoint execution failed");
  if (!status) {
    return status;
  }

  status = drain_microtasks(impl_->env);
  if (!status) {
    return status;
  }

  response->sequence = request.frame.frame_index;
  return stringify_js_value(impl_->env, call_result, &response->payload);
#endif
}

void HermesTransportRuntime::shutdown() noexcept {
#if IGR_ENABLE_HERMES
  if (impl_) {
    if (impl_->env != nullptr && impl_->entrypoint_ref != nullptr) {
      ScopedEnvScope env_scope(impl_->env);
      if (env_scope.status()) {
        napi_delete_reference(impl_->env, impl_->entrypoint_ref);
      }
      impl_->entrypoint_ref = nullptr;
    }

    if (impl_->runtime != nullptr) {
      jsr_delete_runtime(impl_->runtime);
      impl_->runtime = nullptr;
      impl_->env = nullptr;
    }

    if (impl_->config != nullptr) {
      jsr_delete_config(impl_->config);
      impl_->config = nullptr;
    }

    impl_->loaded_from_bytecode = false;
    impl_->loaded_bundle_path.clear();
  }
#endif

  initialized_ = false;
}

bool HermesTransportRuntime::available() const noexcept {
  return initialized_;
}

const HermesRuntimeConfig& HermesTransportRuntime::config() const noexcept {
  return config_;
}

}  // namespace igr::react
