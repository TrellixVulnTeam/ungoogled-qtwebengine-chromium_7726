// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_RAIL_MODE_OBSERVER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_RAIL_MODE_OBSERVER_H_

namespace blink {

// RAIL mode as defined in [1].
// [1] https://developers.9oo91e.qjz9zk/web/fundamentals/performance/rail
enum class RAILMode {
  kResponse,
  kAnimation,
  kIdle,
  kLoad,
};

class RAILModeObserver {
 public:
  virtual ~RAILModeObserver() = default;
  virtual void OnRAILModeChanged(RAILMode rail_mode) = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_RAIL_MODE_OBSERVER_H_
