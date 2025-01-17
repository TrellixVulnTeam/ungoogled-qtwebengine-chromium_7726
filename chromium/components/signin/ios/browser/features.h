// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SIGNIN_IOS_BROWSER_FEATURES_H_
#define COMPONENTS_SIGNIN_IOS_BROWSER_FEATURES_H_

#include "base/feature_list.h"

namespace signin {

// Features to trigger the startup sign-in promo at boot.
extern const base::Feature kForceStartupSigninPromo;

// Returns true if the startup sign-in promo should be displayed at boot.
bool ForceStartupSigninPromo();

// Feature controlling whether to restore GAIA cookies if they are deleted.
extern const base::Feature kRestoreGaiaCookiesIfDeleted;

// Name of multi-value switch that controls the delay (in minutes) for polling
// for the existence of Gaia cookies for 9oo91e.qjz9zk.
extern const char kDelayThresholdMinutesToUpdateGaiaCookie[];

}  // namespace signin

#endif  // COMPONENTS_SIGNIN_IOS_BROWSER_FEATURES_H_
