// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <algorithm>

#include "base/stl_util.h"
#include "extensions/common/error_utils.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_handlers/externally_connectable.h"
#include "extensions/common/manifest_test.h"
#include "extensions/common/permissions/permissions_data.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::ElementsAre;

namespace extensions {

namespace errors = externally_connectable_errors;

class ExternallyConnectableTest : public ManifestTest {
 public:
  ExternallyConnectableTest() {}
  ~ExternallyConnectableTest() override {}

 protected:
  ExternallyConnectableInfo* GetExternallyConnectableInfo(
      scoped_refptr<Extension> extension) {
    return static_cast<ExternallyConnectableInfo*>(
        extension->GetManifestData(manifest_keys::kExternallyConnectable));
  }
};

TEST_F(ExternallyConnectableTest, IDsAndMatches) {
  scoped_refptr<Extension> extension =
      LoadAndExpectSuccess("externally_connectable_ids_and_matches.json");
  ASSERT_TRUE(extension.get());

  ExternallyConnectableInfo* info =
      ExternallyConnectableInfo::Get(extension.get());
  ASSERT_TRUE(info);

  EXPECT_THAT(info->ids,
              ElementsAre("abcdefghijklmnopabcdefghijklmnop",
                          "ponmlkjihgfedcbaponmlkjihgfedcba"));

  EXPECT_FALSE(info->all_ids);

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://example.com/")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://example.com/index.html")));

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/index.html")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://www.9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://www.9oo91e.qjz9zk/")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://9oo91e.qjz9zk/")));

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk/")));
  EXPECT_TRUE(
      info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk/index.html")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("https://build.ch40m1um.qjz9zk")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("https://build.ch40m1um.qjz9zk/")));
  EXPECT_FALSE(
      info->matches.MatchesURL(GURL("http://foo.ch40m1um.qjz9zk/index.html")));

  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://yahoo.com")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://yahoo.com/")));

  // TLD-style patterns should match just the TLD.
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://8pp2p8t.qjz9zk/foo.html")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://go/here")));

  // TLD-style patterns should *not* match any subdomains of the TLD.
  EXPECT_FALSE(
      info->matches.MatchesURL(GURL("http://codereview.8pp2p8t.qjz9zk/foo.html")));
  EXPECT_FALSE(
      info->matches.MatchesURL(GURL("http://chromium.com/index.html")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://here.go/somewhere")));

  // Paths that don't have any wildcards should match the exact domain, but
  // ignore the trailing slash. This is kind of a corner case, so let's test it.
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://no.wildcard.path")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://no.wildcard.path/")));
  EXPECT_FALSE(
      info->matches.MatchesURL(GURL("http://no.wildcard.path/index.html")));
}

TEST_F(ExternallyConnectableTest, IDs) {
  scoped_refptr<Extension> extension =
      LoadAndExpectSuccess("externally_connectable_ids.json");
  ASSERT_TRUE(extension.get());

  ExternallyConnectableInfo* info =
      ExternallyConnectableInfo::Get(extension.get());
  ASSERT_TRUE(info);

  EXPECT_THAT(info->ids,
              ElementsAre("abcdefghijklmnopabcdefghijklmnop",
                          "ponmlkjihgfedcbaponmlkjihgfedcba"));

  EXPECT_FALSE(info->all_ids);

  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/index.html")));
}

TEST_F(ExternallyConnectableTest, Matches) {
  scoped_refptr<Extension> extension =
      LoadAndExpectSuccess("externally_connectable_matches.json");
  ASSERT_TRUE(extension.get());

  ExternallyConnectableInfo* info =
      ExternallyConnectableInfo::Get(extension.get());
  ASSERT_TRUE(info);

  EXPECT_THAT(info->ids, ElementsAre());

  EXPECT_FALSE(info->all_ids);

  EXPECT_FALSE(info->accepts_tls_channel_id);

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://example.com/")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://example.com/index.html")));

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/index.html")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://www.9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://www.9oo91e.qjz9zk/")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://9oo91e.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://9oo91e.qjz9zk/")));

  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk/")));
  EXPECT_TRUE(
      info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk/index.html")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("https://build.ch40m1um.qjz9zk")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("https://build.ch40m1um.qjz9zk/")));
  EXPECT_FALSE(
      info->matches.MatchesURL(GURL("http://foo.ch40m1um.qjz9zk/index.html")));

  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://yahoo.com")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://yahoo.com/")));
}

TEST_F(ExternallyConnectableTest, MatchesWithTlsChannelId) {
  scoped_refptr<Extension> extension = LoadAndExpectSuccess(
      "externally_connectable_matches_tls_channel_id.json");
  ASSERT_TRUE(extension.get());

  ExternallyConnectableInfo* info =
      ExternallyConnectableInfo::Get(extension.get());
  ASSERT_TRUE(info);

  EXPECT_THAT(info->ids, ElementsAre());

  EXPECT_FALSE(info->all_ids);

  EXPECT_TRUE(info->accepts_tls_channel_id);

  // The matches portion of the manifest is identical to those in
  // externally_connectable_matches, so only a subset of the Matches tests is
  // repeated here.
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://example.com")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://example.com/index.html")));
}

TEST_F(ExternallyConnectableTest, AllIDs) {
  scoped_refptr<Extension> extension =
      LoadAndExpectSuccess("externally_connectable_all_ids.json");
  ASSERT_TRUE(extension.get());

  ExternallyConnectableInfo* info =
      ExternallyConnectableInfo::Get(extension.get());
  ASSERT_TRUE(info);

  EXPECT_THAT(info->ids,
              ElementsAre("abcdefghijklmnopabcdefghijklmnop",
                          "ponmlkjihgfedcbaponmlkjihgfedcba"));

  EXPECT_TRUE(info->all_ids);

  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://9oo91e.qjz9zk/index.html")));
}

TEST_F(ExternallyConnectableTest, IdCanConnect) {
  // Not in order to test that ExternallyConnectableInfo sorts it.
  std::string matches_ids_array[] = {"g", "h", "c", "i", "a", "z", "b"};
  std::vector<std::string> matches_ids(
      matches_ids_array, matches_ids_array + base::size(matches_ids_array));

  std::string nomatches_ids_array[] = {"2", "3", "1"};

  // all_ids = false.
  {
    ExternallyConnectableInfo info(URLPatternSet(), matches_ids, false, false);
    for (size_t i = 0; i < matches_ids.size(); ++i)
      EXPECT_TRUE(info.IdCanConnect(matches_ids[i]));
    for (size_t i = 0; i < base::size(nomatches_ids_array); ++i)
      EXPECT_FALSE(info.IdCanConnect(nomatches_ids_array[i]));
  }

  // all_ids = true.
  {
    ExternallyConnectableInfo info(URLPatternSet(), matches_ids, true, false);
    for (size_t i = 0; i < matches_ids.size(); ++i)
      EXPECT_TRUE(info.IdCanConnect(matches_ids[i]));
    for (size_t i = 0; i < base::size(nomatches_ids_array); ++i)
      EXPECT_TRUE(info.IdCanConnect(nomatches_ids_array[i]));
  }
}

TEST_F(ExternallyConnectableTest, ErrorWrongFormat) {
  LoadAndExpectError("externally_connectable_error_wrong_format.json",
                     "expected dictionary, got string");
}

TEST_F(ExternallyConnectableTest, ErrorBadID) {
  LoadAndExpectError(
      "externally_connectable_bad_id.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorInvalidId, "badid"));
}

TEST_F(ExternallyConnectableTest, ErrorBadMatches) {
  LoadAndExpectError("externally_connectable_error_bad_matches.json",
                     ErrorUtils::FormatErrorMessage(
                         errors::kErrorInvalidMatchPattern, "www.yahoo.com"));
}

TEST_F(ExternallyConnectableTest, WarningNoAllURLs) {
  scoped_refptr<Extension> extension = LoadAndExpectWarning(
      "externally_connectable_error_all_urls.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorWildcardHostsNotAllowed,
                                     "<all_urls>"));
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.MatchesAllURLs());
  EXPECT_FALSE(info->matches.ContainsPattern(
      URLPattern(URLPattern::SCHEME_ALL, "<all_urls>")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, AllURLsNotWhitelisted) {
  scoped_refptr<Extension> extension = LoadAndExpectSuccess(
      "externally_connectable_all_urls_not_whitelisted.json");
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.MatchesAllURLs());
}

TEST_F(ExternallyConnectableTest, AllHttpsURLsNotWhitelisted) {
  scoped_refptr<Extension> extension = LoadAndExpectSuccess(
      "externally_connectable_all_https_urls_not_whitelisted.json");
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.MatchesAllURLs());
  EXPECT_FALSE(info->matches.MatchesURL(GURL("https://example.com")));
}

TEST_F(ExternallyConnectableTest, AllURLsWhitelisted) {
  scoped_refptr<Extension> extension =
      LoadAndExpectSuccess("externally_connectable_all_urls_whitelisted.json");
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_TRUE(info->matches.MatchesAllURLs());
  URLPattern pattern(URLPattern::SCHEME_ALL, "<all_urls>");
  EXPECT_TRUE(info->matches.ContainsPattern(pattern));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, AllHttpsURLsWhitelisted) {
  scoped_refptr<Extension> extension = LoadAndExpectSuccess(
      "externally_connectable_all_https_urls_whitelisted.json");
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);

  URLPattern all_urls_pattern(URLPattern::SCHEME_ALL, "<all_urls>");
  EXPECT_FALSE(info->matches.ContainsPattern(all_urls_pattern));

  URLPattern https_urls_pattern(URLPattern::SCHEME_ALL, "https://*/*");
  EXPECT_TRUE(info->matches.ContainsPattern(https_urls_pattern));

  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_FALSE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, WarningWildcardHost) {
  scoped_refptr<Extension> extension = LoadAndExpectWarning(
      "externally_connectable_error_wildcard_host.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorWildcardHostsNotAllowed,
                                     "http://*/*"));
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.ContainsPattern(
      URLPattern(URLPattern::SCHEME_ALL, "http://*/*")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, WarningNoTLD) {
  scoped_refptr<Extension> extension = LoadAndExpectWarning(
      "externally_connectable_error_tld.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorTopLevelDomainsNotAllowed,
                                     "co.uk",
                                     "http://*.co.uk/*"));
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.ContainsPattern(
      URLPattern(URLPattern::SCHEME_ALL, "http://*.co.uk/*")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, WarningNoEffectiveTLD) {
  scoped_refptr<Extension> extension = LoadAndExpectWarning(
      "externally_connectable_error_effective_tld.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorTopLevelDomainsNotAllowed,
                                     "8pp2p8t.qjz9zk",
                                     "http://*.8pp2p8t.qjz9zk/*"));
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.ContainsPattern(
      URLPattern(URLPattern::SCHEME_ALL, "http://*.8pp2p8t.qjz9zk/*")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, WarningUnknownTLD) {
  scoped_refptr<Extension> extension = LoadAndExpectWarning(
      "externally_connectable_error_unknown_tld.json",
      ErrorUtils::FormatErrorMessage(errors::kErrorTopLevelDomainsNotAllowed,
                                     "notatld",
                                     "http://*.notatld/*"));
  ExternallyConnectableInfo* info = GetExternallyConnectableInfo(extension);
  EXPECT_FALSE(info->matches.ContainsPattern(
      URLPattern(URLPattern::SCHEME_ALL, "http://*.notatld/*")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("https://example.com")));
  EXPECT_TRUE(info->matches.MatchesURL(GURL("http://build.ch40m1um.qjz9zk")));
}

TEST_F(ExternallyConnectableTest, WarningNothingSpecified) {
  LoadAndExpectWarning("externally_connectable_nothing_specified.json",
                       errors::kErrorNothingSpecified);
}

}  // namespace extensions
