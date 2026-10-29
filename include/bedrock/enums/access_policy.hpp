#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class CandidatePolicy { VerifiedOnly, AllowCandidates };

enum class UpdatePolicy { CacheFirst, RefreshIfNeeded, ForceRemote };

enum class ReadPolicy { Cached, Fresh };

enum class CommandDecision { Accepted, Rejected };
} // namespace bedrock
