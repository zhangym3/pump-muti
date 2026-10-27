#pragma once
#include "protocol.h"

namespace pump::recovery {
// Bounded capacity budget, not a key allocator or permission to reclaim keys.
// A full/quarantined table must be reconciled; never evict another binding.
constexpr size_t LegacyAppKeys = 1;
constexpr size_t PendingAppKeys = 1; // Serial recovery, including replacement at capacity.
constexpr size_t QuarantinedAppKeys = 1;
constexpr size_t RequiredMasterAppKeys =
    ProductMaxNodes + LegacyAppKeys + PendingAppKeys + QuarantinedAppKeys;
static_assert(ProductMaxNodes == 5, "Review the recovery resource budget when product capacity changes");
static_assert(RequiredMasterAppKeys == 8);
}
