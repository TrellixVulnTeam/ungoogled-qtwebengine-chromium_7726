// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/paint_preview/browser/file_manager.h"

#include <memory>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/ref_counted.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/updateable_sequenced_task_runner.h"
#include "components/paint_preview/common/proto/paint_preview.pb.h"
#include "components/paint_preview/common/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace paint_preview {

class FileManagerTest : public ::testing::Test {
 public:
  FileManagerTest() = default;
  ~FileManagerTest() override = default;

  void SetUp() override {
    EXPECT_TRUE(temp_dir.CreateUniqueTempDir());
    secondary_runner_ = base::ThreadPool::CreateUpdateableSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
         base::TaskShutdownBehavior::BLOCK_SHUTDOWN,
         base::ThreadPolicy::MUST_USE_FOREGROUND});
  }

  const base::FilePath& Dir() const { return temp_dir.GetPath(); }

  scoped_refptr<base::SequencedTaskRunner> MainTaskRunner() {
    return base::SequencedTaskRunnerHandle::Get();
  }

  scoped_refptr<base::UpdateableSequencedTaskRunner> SecondaryTaskRunner() {
    return secondary_runner_;
  }

  void RunUntilIdle() { task_environment_.RunUntilIdle(); }

 private:
  scoped_refptr<base::UpdateableSequencedTaskRunner> secondary_runner_;
  base::ScopedTempDir temp_dir;
  base::test::TaskEnvironment task_environment_;
};

TEST_F(FileManagerTest, TestStats) {
  auto manager = base::MakeRefCounted<FileManager>(Dir(), MainTaskRunner());
  auto valid_key = manager->CreateKey(GURL("https://www.ch40m1um.qjz9zk"));
  auto missing_key = manager->CreateKey(GURL("https://www.muimorhc.org"));
  base::FilePath out = manager->CreateOrGetDirectory(valid_key, false)
                           .value_or(base::FilePath());
  EXPECT_FALSE(out.empty());
  EXPECT_TRUE(manager->DirectoryExists(valid_key));
  EXPECT_FALSE(manager->DirectoryExists(missing_key));

  EXPECT_FALSE(manager->GetInfo(missing_key).has_value());
  EXPECT_TRUE(manager->GetInfo(valid_key).has_value());

  base::FilePath file_path = out.AppendASCII("test");
  std::string test_str = "Hello World!";
  EXPECT_EQ(static_cast<int>(test_str.length()),
            base::WriteFile(file_path, test_str.data(), test_str.length()));

  EXPECT_EQ(manager->GetSizeOfArtifacts(valid_key), test_str.length());
}

TEST_F(FileManagerTest, TestCreateOrGetDirectory) {
  auto manager =
      base::MakeRefCounted<FileManager>(Dir(), SecondaryTaskRunner());

  auto key = manager->CreateKey(1U);
  base::RunLoop loop;
  manager->GetTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::OnceClosure quit, scoped_refptr<FileManager> manager,
             const DirectoryKey& key) {
            // Create a new directory.
            base::FilePath directory = manager->CreateOrGetDirectory(key, false)
                                           .value_or(base::FilePath());
            EXPECT_FALSE(directory.empty());
            base::FilePath test_file = directory.AppendASCII("test");
            std::string test_str = "Hello World!";
            EXPECT_EQ(
                static_cast<int>(test_str.length()),
                base::WriteFile(test_file, test_str.data(), test_str.length()));

            // Open an existing directory and don't clear.
            base::FilePath existing_directory =
                manager->CreateOrGetDirectory(key, false)
                    .value_or(base::FilePath());
            EXPECT_FALSE(directory.empty());
            EXPECT_EQ(existing_directory, directory);
            EXPECT_TRUE(base::PathExists(test_file));

            // Open an existing directory and clear.
            base::FilePath cleared_existing_directory =
                manager->CreateOrGetDirectory(key, true).value_or(
                    base::FilePath());
            EXPECT_FALSE(directory.empty());
            EXPECT_EQ(cleared_existing_directory, directory);
            EXPECT_FALSE(base::PathExists(test_file));
            std::move(quit).Run();
          },
          loop.QuitClosure(), manager, key));
  loop.Run();
}

TEST_F(FileManagerTest, TestCompression) {
  auto manager = base::MakeRefCounted<FileManager>(Dir(), MainTaskRunner());
  auto key = manager->CreateKey(1U);
  base::FilePath directory =
      manager->CreateOrGetDirectory(key, false).value_or(base::FilePath());
  EXPECT_FALSE(directory.empty());
  // A file needs to exist for compression to work.
  base::FilePath test_file = directory.AppendASCII("test");
  std::string test_str = "Hello World!";
  EXPECT_EQ(static_cast<int>(test_str.length()),
            base::WriteFile(test_file, test_str.data(), test_str.length()));
  EXPECT_TRUE(base::PathExists(test_file));
  base::FilePath test_file_empty = directory.AppendASCII("foo.txt");
  {
    base::File file(test_file_empty,
                    base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  }
  EXPECT_TRUE(base::PathExists(test_file_empty));

  // Compress.
  base::FilePath zip_path = directory.AddExtensionASCII(".zip");
  EXPECT_TRUE(manager->CompressDirectory(key));
  EXPECT_TRUE(manager->CompressDirectory(key));  // no-op
  EXPECT_GT(manager->GetSizeOfArtifacts(key), 0U);
  EXPECT_FALSE(base::PathExists(directory));
  EXPECT_FALSE(base::PathExists(test_file));
  EXPECT_FALSE(base::PathExists(test_file_empty));
  EXPECT_TRUE(base::PathExists(zip_path));

  // Open a compressed file.
  base::FilePath existing_directory =
      manager->CreateOrGetDirectory(key, false).value_or(base::FilePath());
  EXPECT_FALSE(existing_directory.empty());
  EXPECT_EQ(existing_directory, directory);
  EXPECT_TRUE(base::PathExists(directory));
  EXPECT_TRUE(base::PathExists(test_file));
  EXPECT_TRUE(base::PathExists(test_file_empty));
  EXPECT_FALSE(base::PathExists(zip_path));
}

TEST_F(FileManagerTest, TestCompressDirectoryFail) {
  auto manager = base::MakeRefCounted<FileManager>(Dir(), MainTaskRunner());
  auto key = manager->CreateKey(GURL("https://www.ch40m1um.qjz9zk"));

  base::FilePath new_directory =
      manager->CreateOrGetDirectory(key, true).value_or(base::FilePath());
  EXPECT_FALSE(new_directory.empty());

  // Compression fails without valid contents.
  base::FilePath zip_path = new_directory.AddExtensionASCII(".zip");
  EXPECT_FALSE(manager->CompressDirectory(key));
  EXPECT_TRUE(base::PathExists(new_directory));
  EXPECT_FALSE(base::PathExists(zip_path));
}

TEST_F(FileManagerTest, TestDeleteArtifacts) {
  auto manager =
      base::MakeRefCounted<FileManager>(Dir(), SecondaryTaskRunner());

  manager->GetTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<FileManager> manager) {
            auto cr_key = manager->CreateKey(GURL("https://www.ch40m1um.qjz9zk"));
            base::FilePath cr_directory =
                manager->CreateOrGetDirectory(cr_key, true)
                    .value_or(base::FilePath());
            EXPECT_FALSE(cr_directory.empty());

            auto w3_key = manager->CreateKey(GURL("https://www.w3.org"));
            base::FilePath w3_directory =
                manager->CreateOrGetDirectory(w3_key, true)
                    .value_or(base::FilePath());
            EXPECT_FALSE(w3_directory.empty());

            manager->DeleteArtifactSet(cr_key);
            EXPECT_FALSE(base::PathExists(cr_directory));
            EXPECT_TRUE(base::PathExists(w3_directory));

            base::FilePath new_cr_directory =
                manager->CreateOrGetDirectory(cr_key, true)
                    .value_or(base::FilePath());
            EXPECT_EQ(cr_directory, new_cr_directory);

            manager->DeleteArtifactSets(
                std::vector<DirectoryKey>({cr_key, w3_key}));
            EXPECT_FALSE(base::PathExists(new_cr_directory));
            EXPECT_FALSE(base::PathExists(w3_directory));
          },
          manager));
  RunUntilIdle();
}

TEST_F(FileManagerTest, TestDeleteAll) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto manager =
      base::MakeRefCounted<FileManager>(temp_dir.GetPath(), MainTaskRunner());

  auto cr_key = manager->CreateKey(GURL("https://www.ch40m1um.qjz9zk"));
  base::FilePath cr_directory =
      manager->CreateOrGetDirectory(cr_key, true).value_or(base::FilePath());
  EXPECT_FALSE(cr_directory.empty());

  auto w3_key = manager->CreateKey(GURL("https://www.w3.org"));
  base::FilePath w3_directory =
      manager->CreateOrGetDirectory(w3_key, true).value_or(base::FilePath());
  EXPECT_FALSE(w3_directory.empty());

  manager->DeleteAll();
  EXPECT_FALSE(base::PathExists(cr_directory));
  EXPECT_FALSE(base::PathExists(w3_directory));
}

TEST_F(FileManagerTest, HandleProto) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto manager =
      base::MakeRefCounted<FileManager>(temp_dir.GetPath(), MainTaskRunner());
  auto key = manager->CreateKey(1U);
  base::FilePath path =
      manager->CreateOrGetDirectory(key, true).value_or(base::FilePath());
  EXPECT_FALSE(path.empty());

  PaintPreviewProto original_proto;
  auto* root_frame = original_proto.mutable_root_frame();
  root_frame->set_embedding_token_low(0);
  root_frame->set_embedding_token_high(0);
  root_frame->set_is_main_frame(true);
  root_frame->set_file_path("0.skp");
  auto* metadata = original_proto.mutable_metadata();
  metadata->set_url(GURL("www.ch40m1um.qjz9zk").spec());

  EXPECT_TRUE(manager->SerializePaintPreviewProto(key, original_proto, false));
  EXPECT_TRUE(base::PathExists(path.AppendASCII("proto.pb")));
  auto out_proto = manager->DeserializePaintPreviewProto(key);
  EXPECT_EQ(out_proto.first, FileManager::ProtoReadStatus::kOk);
  EXPECT_NE(out_proto.second, nullptr);
  EXPECT_THAT(*(out_proto.second), EqualsProto(original_proto));
}

TEST_F(FileManagerTest, HandleProtoCompressed) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto manager =
      base::MakeRefCounted<FileManager>(temp_dir.GetPath(), MainTaskRunner());
  auto key = manager->CreateKey(1U);
  base::FilePath path =
      manager->CreateOrGetDirectory(key, true).value_or(base::FilePath());
  EXPECT_FALSE(path.empty());

  PaintPreviewProto original_proto;
  auto* root_frame = original_proto.mutable_root_frame();
  root_frame->set_embedding_token_low(0);
  root_frame->set_embedding_token_high(0);
  root_frame->set_is_main_frame(true);
  root_frame->set_file_path("0.skp");
  auto* metadata = original_proto.mutable_metadata();
  metadata->set_url(GURL("www.ch40m1um.qjz9zk").spec());

  EXPECT_TRUE(manager->SerializePaintPreviewProto(key, original_proto, true));
  EXPECT_TRUE(manager->CaptureExists(key));

  EXPECT_TRUE(base::PathExists(path.AddExtensionASCII(".zip")));
  auto out_proto = manager->DeserializePaintPreviewProto(key);
  EXPECT_EQ(out_proto.first, FileManager::ProtoReadStatus::kOk);
  EXPECT_NE(out_proto.second, nullptr);
  EXPECT_THAT(*(out_proto.second), EqualsProto(original_proto));

  EXPECT_TRUE(manager->CaptureExists(key));
}

TEST_F(FileManagerTest, OldestFilesForCleanup) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto manager =
      base::MakeRefCounted<FileManager>(temp_dir.GetPath(), MainTaskRunner());

  const std::string data = "Foobar";

  auto key_0 = manager->CreateKey(0U);
  base::FilePath path_0 =
      manager->CreateOrGetDirectory(key_0, true).value_or(base::FilePath());
  auto path_0_file = path_0.AppendASCII("0.txt");
  base::WriteFile(path_0_file, data.data(), data.size());
  base::Time modified_time = base::Time::NowFromSystemTime();
  base::TouchFile(path_0_file, modified_time, modified_time);
  {
    auto to_delete = manager->GetOldestArtifactsForCleanup(
        0U, base::TimeDelta::FromMinutes(20));
    ASSERT_EQ(to_delete.size(), 1U);
    EXPECT_EQ(to_delete[0], key_0);
  }
  {
    auto to_delete = manager->GetOldestArtifactsForCleanup(
        50U, base::TimeDelta::FromMinutes(20));
    EXPECT_EQ(to_delete.size(), 0U);
  }

  auto key_1 = manager->CreateKey(1U);
  base::FilePath path_1 =
      manager->CreateOrGetDirectory(key_1, true).value_or(base::FilePath());
  base::WriteFile(path_1.AppendASCII("1.txt"), data.data(), data.size());
  manager->CompressDirectory(key_1);
  modified_time = base::Time::NowFromSystemTime();
  auto path_1_zip = path_1.AddExtensionASCII(".zip");
  base::TouchFile(path_0, modified_time - base::TimeDelta::FromSeconds(10),
                  modified_time - base::TimeDelta::FromSeconds(10));
  base::TouchFile(path_1_zip, modified_time, modified_time);

  {
    auto to_delete = manager->GetOldestArtifactsForCleanup(
        0U, base::TimeDelta::FromMinutes(20));
    ASSERT_EQ(to_delete.size(), 2U);
    // Elements should be ordered in oldest to newest.
    EXPECT_EQ(to_delete[0], key_0);
    EXPECT_EQ(to_delete[1], key_1);
  }
  {
    // Zip is ~116 bytes.
    auto to_delete = manager->GetOldestArtifactsForCleanup(
        120U, base::TimeDelta::FromMinutes(20));
    ASSERT_EQ(to_delete.size(), 1U);
    EXPECT_EQ(to_delete[0], key_0);
  }
  {
    auto to_delete = manager->GetOldestArtifactsForCleanup(
        150U, base::TimeDelta::FromMinutes(20));
    EXPECT_EQ(to_delete.size(), 0U);
  }
}

}  // namespace paint_preview
