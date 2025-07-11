#pragma once

#include "envoy/config/route/v3/route_components.pb.h"
#include "envoy/http/hash_policy.h"
#include "envoy/stream_info/filter_state.h"

namespace Envoy {

namespace Regex {
class Engine;
}

namespace Http {

/**
 * Implementation of HashPolicy that reads from the proto route config.
 */
class HashPolicyImpl : public HashPolicy {
public:
  static absl::StatusOr<std::unique_ptr<HashPolicyImpl>>
  create(absl::Span<const envoy::config::route::v3::RouteAction::HashPolicy* const> hash_policy,
         Regex::Engine& regex_engine);

  // Http::HashPolicy
  absl::optional<uint64_t> generateHash(OptRef<const RequestHeaderMap> headers,
                                        OptRef<const StreamInfo::StreamInfo> info,
                                        AddCookieCallback add_cookie = nullptr) const override;

  class HashMethod {
  public:
    virtual ~HashMethod() = default;
    virtual absl::optional<uint64_t> evaluate(OptRef<const RequestHeaderMap> headers,
                                              OptRef<const StreamInfo::StreamInfo> info,
                                              AddCookieCallback add_cookie = nullptr) const PURE;

    // If the method is a terminal method, ignore rest of the hash policy chain.
    virtual bool terminal() const PURE;
  };

  using HashMethodPtr = std::unique_ptr<HashMethod>;

protected:
  explicit HashPolicyImpl(
      absl::Span<const envoy::config::route::v3::RouteAction::HashPolicy* const> hash_policy,
      Regex::Engine& regex_engine, absl::Status& creation_status);

private:
  std::vector<HashMethodPtr> hash_impls_;
};

} // namespace Http
} // namespace Envoy
