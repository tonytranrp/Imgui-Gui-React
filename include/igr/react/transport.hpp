#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "igr/host.hpp"
#include "igr/react/document.hpp"
#include "igr/result.hpp"
#include "igr/resources.hpp"

namespace igr::react {

struct TransportFontResource {
  std::string key;
  FontResourceDesc descriptor;
};

struct TransportImageResource {
  std::string key;
  ImageResourceDesc descriptor;
};

struct TransportShaderResource {
  std::string key;
  ShaderResourceDesc descriptor;
};

struct TransportSession {
  std::string name;
  std::string target_backend{"any"};
  BackendHostOptions host{};
};

struct TransportEnvelope {
  std::string kind{"igr.document.v1"};
  std::uint64_t sequence{};
  TransportSession session{};
  std::vector<TransportFontResource> fonts;
  std::vector<TransportImageResource> images;
  std::vector<TransportShaderResource> shaders;
  ElementNode root;
};

Status parse_transport_envelope(std::string_view payload, TransportEnvelope* envelope);
Status serialize_transport_envelope(const TransportEnvelope& envelope, std::string* payload);
Status materialize_transport_envelope(const TransportEnvelope& envelope, FrameBuilder& builder);
Status materialize_transport_envelope(std::string_view payload, FrameBuilder& builder);

}  // namespace igr::react
