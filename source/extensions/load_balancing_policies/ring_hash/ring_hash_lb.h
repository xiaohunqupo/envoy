#pragma once

#include <vector>

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/load_balancing_policies/ring_hash/v3/ring_hash.pb.h"
#include "envoy/extensions/load_balancing_policies/ring_hash/v3/ring_hash.pb.validate.h"
#include "envoy/runtime/runtime.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/upstream/load_balancer.h"

#include "source/common/common/logger.h"
#include "source/extensions/load_balancing_policies/common/thread_aware_lb_impl.h"

namespace Envoy {
namespace Upstream {

using RingHashLbProto = envoy::extensions::load_balancing_policies::ring_hash::v3::RingHash;
using ClusterProto = envoy::config::cluster::v3::Cluster;

using CommonLbConfigProto = envoy::config::cluster::v3::Cluster::CommonLbConfig;
using LegacyRingHashLbProto = envoy::config::cluster::v3::Cluster::RingHashLbConfig;

/**
 * Load balancer config that used to wrap typed ring hash config.
 */
class TypedRingHashLbConfig : public Upstream::TypedHashLbConfigBase {
public:
  TypedRingHashLbConfig(const CommonLbConfigProto& common_lb_config,
                        const LegacyRingHashLbProto& lb_config);
  TypedRingHashLbConfig(const RingHashLbProto& lb_config, Regex::Engine& regex_engine,
                        absl::Status& creation_status);

  RingHashLbProto lb_config_;
};

/**
 * All ring hash load balancer stats. @see stats_macros.h
 */
#define ALL_RING_HASH_LOAD_BALANCER_STATS(GAUGE)                                                   \
  GAUGE(max_hashes_per_host, Accumulate)                                                           \
  GAUGE(min_hashes_per_host, Accumulate)                                                           \
  GAUGE(size, Accumulate)

/**
 * Struct definition for all ring hash load balancer stats. @see stats_macros.h
 */
struct RingHashLoadBalancerStats {
  ALL_RING_HASH_LOAD_BALANCER_STATS(GENERATE_GAUGE_STRUCT)
};

/**
 * A load balancer that implements consistent modulo hashing ("ketama"). Currently, zone aware
 * routing is not supported. A ring is kept for all hosts as well as a ring for healthy hosts.
 * Unless we are in panic mode, the healthy host ring is used.
 * In the future it would be nice to support:
 * 1) Weighting.
 * 2) Per-zone rings and optional zone aware routing (not all applications will want this).
 * 3) Max request fallback to support hot shards (not all applications will want this).
 */
class RingHashLoadBalancer : public ThreadAwareLoadBalancerBase {
public:
  RingHashLoadBalancer(const PrioritySet& priority_set, ClusterLbStats& stats, Stats::Scope& scope,
                       Runtime::Loader& runtime, Random::RandomGenerator& random,
                       uint32_t healthy_panic_threshold, const RingHashLbProto& config,
                       HashPolicySharedPtr hash_policy);

  const RingHashLoadBalancerStats& stats() const { return stats_; }

private:
  using HashFunction = RingHashLbProto::HashFunction;

  struct RingEntry {
    uint64_t hash_;
    HostConstSharedPtr host_;
  };

  struct Ring : public HashingLoadBalancer {
    Ring(const NormalizedHostWeightVector& normalized_host_weights, double min_normalized_weight,
         uint64_t min_ring_size, uint64_t max_ring_size, HashFunction hash_function,
         bool use_hostname_for_hashing, RingHashLoadBalancerStats& stats);

    // ThreadAwareLoadBalancerBase::HashingLoadBalancer
    HostSelectionResponse chooseHost(uint64_t hash, uint32_t attempt) const override;

    std::vector<RingEntry> ring_;

    RingHashLoadBalancerStats& stats_;
  };
  using RingConstSharedPtr = std::shared_ptr<const Ring>;

  // ThreadAwareLoadBalancerBase
  HashingLoadBalancerSharedPtr
  createLoadBalancer(const NormalizedHostWeightVector& normalized_host_weights,
                     double min_normalized_weight, double /* max_normalized_weight */) override {
    HashingLoadBalancerSharedPtr ring_hash_lb =
        std::make_shared<Ring>(normalized_host_weights, min_normalized_weight, min_ring_size_,
                               max_ring_size_, hash_function_, use_hostname_for_hashing_, stats_);
    if (hash_balance_factor_ == 0) {
      return ring_hash_lb;
    }

    return std::make_shared<BoundedLoadHashingLoadBalancer>(
        ring_hash_lb, std::move(normalized_host_weights), hash_balance_factor_);
  }

  static RingHashLoadBalancerStats generateStats(Stats::Scope& scope);

  Stats::ScopeSharedPtr scope_;
  RingHashLoadBalancerStats stats_;

  static const uint64_t DefaultMinRingSize = 1024;
  static const uint64_t DefaultMaxRingSize = 1024 * 1024 * 8;
  const uint64_t min_ring_size_;
  const uint64_t max_ring_size_;
  const HashFunction hash_function_;
  const bool use_hostname_for_hashing_;
  const uint32_t hash_balance_factor_;
};

} // namespace Upstream
} // namespace Envoy
