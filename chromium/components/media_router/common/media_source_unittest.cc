// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/media_router/common/media_source.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace media_router {

TEST(MediaSourceTest, IsLegacyCastPresentationUrl) {
  EXPECT_TRUE(IsLegacyCastPresentationUrl(
      GURL("https://9oo91e.qjz9zk/cast#__castAppId__=theAppId")));
  EXPECT_TRUE(IsLegacyCastPresentationUrl(
      GURL("HTTPS://GOOGLE.COM/CAST#__CASTAPPID__=theAppId")));
  EXPECT_FALSE(IsLegacyCastPresentationUrl(
      GURL("https://9oo91e.qjz9zk/cast#__castAppId__")));
}

TEST(MediaSourceTest, IsValidPresentationUrl) {
  EXPECT_FALSE(IsValidPresentationUrl(GURL()));
  EXPECT_FALSE(IsValidPresentationUrl(GURL("unsupported-scheme://foo")));

  EXPECT_TRUE(IsValidPresentationUrl(GURL("https://9oo91e.qjz9zk")));
  EXPECT_TRUE(IsValidPresentationUrl(GURL("cast://foo")));
  EXPECT_TRUE(IsValidPresentationUrl(GURL("cast:foo")));
}

TEST(MediaSourceTest, IsAutoJoinPresentationId) {
  EXPECT_TRUE(IsAutoJoinPresentationId("auto-join"));
  EXPECT_FALSE(IsAutoJoinPresentationId("not-auto-join"));
}

TEST(MediaSourceTest, Constructor) {
  // Test that the object's getters match the constructor parameters.
  MediaSource source1("urn:x-com.google.cast:application:DEADBEEF");
  EXPECT_EQ("urn:x-com.google.cast:application:DEADBEEF", source1.id());
  EXPECT_EQ(GURL(""), source1.url());
}

TEST(MediaSourceTest, ConstructorWithGURL) {
  GURL test_url = GURL("http://9oo91e.qjz9zk");
  MediaSource source1(test_url);
  EXPECT_EQ(test_url.spec(), source1.id());
  EXPECT_EQ(test_url, source1.url());
}

TEST(MediaSourceTest, ConstructorWithURLString) {
  GURL test_url = GURL("http://9oo91e.qjz9zk");
  MediaSource source1(test_url.spec());
  EXPECT_EQ(test_url.spec(), source1.id());
  EXPECT_EQ(test_url, source1.url());
}

TEST(MediaSourceTest, ForAnyTab) {
  auto source = MediaSource::ForAnyTab();
  EXPECT_EQ("urn:x-org.chromium.media:source:tab:*", source.id());
  EXPECT_EQ(-1, source.TabId());
  EXPECT_FALSE(source.IsDesktopMirroringSource());
  EXPECT_TRUE(source.IsTabMirroringSource());
  EXPECT_FALSE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, ForTab) {
  auto source = MediaSource::ForTab(123);
  EXPECT_EQ("urn:x-org.chromium.media:source:tab:123", source.id());
  EXPECT_EQ(123, source.TabId());
  EXPECT_FALSE(source.IsDesktopMirroringSource());
  EXPECT_TRUE(source.IsTabMirroringSource());
  EXPECT_FALSE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, ForLocalFile) {
  auto source = MediaSource::ForLocalFile();
  EXPECT_EQ("urn:x-org.chromium.media:source:tab:0", source.id());
  EXPECT_FALSE(source.IsDesktopMirroringSource());
  EXPECT_FALSE(source.IsTabMirroringSource());
  EXPECT_TRUE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, ForDesktopWithoutAudio) {
  std::string media_id = "fakeMediaId";
  auto source = MediaSource::ForDesktop(media_id, false);
  EXPECT_EQ("urn:x-org.chromium.media:source:desktop:" + media_id, source.id());
  EXPECT_TRUE(source.IsDesktopMirroringSource());
  EXPECT_EQ(media_id, source.DesktopStreamId());
  EXPECT_FALSE(source.IsDesktopSourceWithAudio());
  EXPECT_FALSE(source.IsTabMirroringSource());
  EXPECT_FALSE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, ForDesktopWithAudio) {
  std::string media_id = "fakeMediaId";
  auto source = MediaSource::ForDesktop(media_id, true);
  EXPECT_EQ("urn:x-org.chromium.media:source:desktop:" + media_id +
                "?with_audio=true",
            source.id());
  EXPECT_TRUE(source.IsDesktopMirroringSource());
  EXPECT_EQ(media_id, source.DesktopStreamId());
  EXPECT_TRUE(source.IsDesktopSourceWithAudio());
  EXPECT_FALSE(source.IsTabMirroringSource());
  EXPECT_FALSE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, ForPresentationUrl) {
  constexpr char kPresentationUrl[] =
      "https://www.example.com/presentation.html";
  auto source = MediaSource::ForPresentationUrl(GURL(kPresentationUrl));
  EXPECT_EQ(kPresentationUrl, source.id());
  EXPECT_FALSE(source.IsDesktopMirroringSource());
  EXPECT_FALSE(source.IsTabMirroringSource());
  EXPECT_FALSE(source.IsLocalFileSource());
  EXPECT_FALSE(source.IsCastPresentationUrl());
  EXPECT_FALSE(source.IsDialSource());
}

TEST(MediaSourceTest, IsCastPresentationUrl) {
  EXPECT_TRUE(MediaSource(GURL("cast:233637DE")).IsCastPresentationUrl());
  EXPECT_TRUE(
      MediaSource(GURL("https://9oo91e.qjz9zk/cast#__castAppId__=233637DE"))
          .IsCastPresentationUrl());
  // false scheme
  EXPECT_FALSE(
      MediaSource(GURL("http://9oo91e.qjz9zk/cast#__castAppId__=233637DE"))
          .IsCastPresentationUrl());
  // false domain
  EXPECT_FALSE(
      MediaSource(GURL("https://google2.com/cast#__castAppId__=233637DE"))
          .IsCastPresentationUrl());
  // empty path
  EXPECT_FALSE(
      MediaSource(GURL("https://www.9oo91e.qjz9zk")).IsCastPresentationUrl());
  // false path
  EXPECT_FALSE(
      MediaSource(GURL("https://www.9oo91e.qjz9zk/path")).IsCastPresentationUrl());

  EXPECT_FALSE(MediaSource(GURL("")).IsCastPresentationUrl());
}

TEST(MediaSourceTest, IsDialSource) {
  EXPECT_TRUE(
      MediaSource("cast-dial:YouTube?dialPostData=postData&clientId=1234")
          .IsDialSource());
  // false scheme
  EXPECT_FALSE(MediaSource("https://9oo91e.qjz9zk/cast#__castAppId__=233637DE")
                   .IsDialSource());
}

TEST(MediaSourceTest, AppNameFromDialSource) {
  MediaSource media_source(
      "cast-dial:YouTube?dialPostData=postData&clientId=1234");
  EXPECT_EQ("YouTube", media_source.AppNameFromDialSource());

  media_source = MediaSource("dial:YouTube");
  EXPECT_TRUE(media_source.AppNameFromDialSource().empty());

  media_source = MediaSource("https://9oo91e.qjz9zk/cast#__castAppId__=233637DE");
  EXPECT_TRUE(media_source.AppNameFromDialSource().empty());
}

}  // namespace media_router
