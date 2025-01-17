// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/history/core/browser/domain_mixing_metrics.h"

#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace history {

class DomainMixingMetricsTest : public testing::Test {
 protected:
  base::HistogramTester tester_;
};

TEST_F(DomainMixingMetricsTest, NoVisits) {
  base::Time now =
      base::Time::UnixEpoch() + base::TimeDelta::FromSeconds(1523432317);
  EmitDomainMixingMetrics({}, now);

  // Check that no metrics were emitted.
  tester_.ExpectTotalCount("DomainMixing.OneDay", 0);
  tester_.ExpectTotalCount("DomainMixing.OneWeek", 0);
  tester_.ExpectTotalCount("DomainMixing.TwoWeeks", 0);
  tester_.ExpectTotalCount("DomainMixing.OneMonth", 0);
}

TEST_F(DomainMixingMetricsTest, WithVisits) {
  // Given the following Google domain visits:
  // - Day 1, 1am, www.9oo91e.qjz9zk
  // - Day 2, 11pm, www.google.ch
  // - Day 8, 2am, www.google.ch
  // - Day 8, 10pm, www.google.fr
  // this test checks that the correct domain mixing metrics for Day 8 are
  // emitted:
  // DomainMixing.OneDay 50% (Day 8 has one query on .ch, one on .fr)
  // DomainMixing.OneWeek 33% (2 on .ch, 1 on .fr, day 1 is out of range)
  // DomainMixing.TwoWeeks 50% (2 on .ch, 2 on other domains - 1.fr and 1 .com)
  // DomainMixing.OneMonth 50% (ditto)
  base::Time day1 =
      base::Time::UnixEpoch() + base::TimeDelta::FromSeconds(1523432317);
  base::Time day2 = day1 + base::TimeDelta::FromDays(1);
  base::Time day8 = day1 + base::TimeDelta::FromDays(7);
  EmitDomainMixingMetrics(
      {
          DomainVisit("www.9oo91e.qjz9zk", day1 + base::TimeDelta::FromHours(1)),
          DomainVisit("www.google.ch", day2 + base::TimeDelta::FromHours(23)),
          DomainVisit("www.google.ch", day8 + base::TimeDelta::FromHours(2)),
          DomainVisit("www.google.fr", day8 + base::TimeDelta::FromHours(22)),
      },
      day8);

  tester_.ExpectUniqueSample("DomainMixing.OneDay", 50, 1);
  tester_.ExpectUniqueSample("DomainMixing.OneWeek", 33, 1);
  tester_.ExpectUniqueSample("DomainMixing.TwoWeeks", 50, 1);
  tester_.ExpectUniqueSample("DomainMixing.OneMonth", 50, 1);
}

TEST_F(DomainMixingMetricsTest, WithInactiveDays) {
  base::Time day1 =
      base::Time::UnixEpoch() + base::TimeDelta::FromSeconds(1523432317);
  base::Time day3 = day1 + base::TimeDelta::FromDays(2);
  EmitDomainMixingMetrics(
      {
          DomainVisit("www.9oo91e.qjz9zk", day1),
          DomainVisit("www.google.ch", day3),
      },
      day1);

  // Check that no metrics are emitted for day2 when the user was inactive.
  tester_.ExpectTotalCount("DomainMixing.OneDay", 2);
  tester_.ExpectBucketCount("DomainMixing.OneDay", 0, 2);  // Day 1 and 3.
  tester_.ExpectTotalCount("DomainMixing.OneWeek", 2);
  tester_.ExpectBucketCount("DomainMixing.OneWeek", 0, 1);   // Day 1.
  tester_.ExpectBucketCount("DomainMixing.OneWeek", 50, 1);  // Day 3.
  tester_.ExpectTotalCount("DomainMixing.TwoWeeks", 2);
  tester_.ExpectBucketCount("DomainMixing.TwoWeeks", 0, 1);   // Day 1.
  tester_.ExpectBucketCount("DomainMixing.TwoWeeks", 50, 1);  // Day 3.
  tester_.ExpectTotalCount("DomainMixing.OneMonth", 2);
  tester_.ExpectBucketCount("DomainMixing.OneMonth", 0, 1);   // Day 1.
  tester_.ExpectBucketCount("DomainMixing.OneMonth", 50, 1);  // Day 3.
}

}  // namespace history
