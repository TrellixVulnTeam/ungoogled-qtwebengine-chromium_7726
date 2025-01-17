// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_NATIVE_FILE_SYSTEM_FILE_WRITER_IMPL_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_NATIVE_FILE_SYSTEM_FILE_WRITER_IMPL_H_

#include "base/memory/weak_ptr.h"
#include "components/download/public/common/quarantine_connection.h"
#include "components/download/quarantine/quarantine.h"
#include "components/services/filesystem/public/mojom/types.mojom.h"
#include "content/browser/file_system_access/native_file_system_file_handle_impl.h"
#include "content/browser/file_system_access/native_file_system_handle_base.h"
#include "content/common/content_export.h"
#include "content/public/browser/native_file_system_permission_context.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "storage/browser/file_system/file_system_url.h"
#include "third_party/blink/public/mojom/file_system_access/native_file_system_file_writer.mojom.h"

namespace content {

// This is the browser side implementation of the
// NativeFileSystemFileWriter mojom interface. Instances of this class are
// owned by the NativeFileSystemManagerImpl instance passed in to the
// constructor.
//
// This class is not thread safe, all methods must be called from the same
// sequence.
class CONTENT_EXPORT NativeFileSystemFileWriterImpl
    : public NativeFileSystemHandleBase,
      public blink::mojom::NativeFileSystemFileWriter {
 public:
  // Creates a FileWriter that writes in a swap file URL and
  // materializes the changes in the target file URL only after `Close`
  // is invoked and successfully completes. Assumes that swap_url represents a
  // file, and is valid.
  // If no |quarantine_connection_callback| is passed in no quarantine is done,
  // other than setting source information directly if on windows.
  NativeFileSystemFileWriterImpl(
      NativeFileSystemManagerImpl* manager,
      const BindingContext& context,
      const storage::FileSystemURL& url,
      const storage::FileSystemURL& swap_url,
      const SharedHandleState& handle_state,
      bool has_transient_user_activation,
      download::QuarantineConnectionCallback quarantine_connection_callback);
  ~NativeFileSystemFileWriterImpl() override;

  const storage::FileSystemURL& swap_url() const { return swap_url_; }

  void Write(uint64_t offset,
             mojo::PendingRemote<blink::mojom::Blob> data,
             WriteCallback callback) override;
  void WriteStream(uint64_t offset,
                   mojo::ScopedDataPipeConsumerHandle stream,
                   WriteStreamCallback callback) override;

  void Truncate(uint64_t length, TruncateCallback callback) override;
  void Close(CloseCallback callback) override;

  using HashCallback = base::OnceCallback<
      void(base::File::Error error, const std::string& hash, int64_t size)>;
  void ComputeHashForSwapFileForTesting(HashCallback callback) {
    ComputeHashForSwapFile(std::move(callback));
  }

 private:
  // State that is kept for the duration of a write operation, to keep track of
  // progress until the write completes.
  struct WriteState;

  void WriteImpl(uint64_t offset,
                 mojo::PendingRemote<blink::mojom::Blob> data,
                 WriteCallback callback);
  void WriteStreamImpl(uint64_t offset,
                       mojo::ScopedDataPipeConsumerHandle stream,
                       WriteStreamCallback callback);
  void DidWrite(WriteState* state,
                base::File::Error result,
                int64_t bytes,
                bool complete);
  void TruncateImpl(uint64_t length, TruncateCallback callback);
  void CloseImpl(CloseCallback callback);
  // The following method is static, because it needs to be invoked to
  // perform cleanup even if the writer was deleted before it was invoked.
  static void DidAfterWriteCheck(
      base::WeakPtr<NativeFileSystemFileWriterImpl> file_writer,
      scoped_refptr<NativeFileSystemManagerImpl> manager,
      const storage::FileSystemURL& swap_url,
      NativeFileSystemFileWriterImpl::CloseCallback callback,
      NativeFileSystemPermissionContext::AfterWriteCheckResult result);
  void DidPassAfterWriteCheck(CloseCallback callback);
  void DidSwapFileSkipQuarantine(CloseCallback callback,
                                 base::File::Error result);
  static void DidSwapFileDoQuarantine(
      base::WeakPtr<NativeFileSystemFileWriterImpl> file_writer,
      const storage::FileSystemURL& target_url,
      const GURL& referrer_url,
      mojo::Remote<quarantine::mojom::Quarantine> quarantine_remote,
      CloseCallback callback,
      base::File::Error result);
  void DidAnnotateFile(
      CloseCallback callback,
      mojo::Remote<quarantine::mojom::Quarantine> quarantine_remote,
      quarantine::mojom::QuarantineFileResult result);

  // After write and quarantine checks should apply to paths on all filesystems
  // except temporary file systems.
  // TOOD(crbug.com/1103076): Extend this check to non-native paths.
  bool RequireSecurityChecks() const {
    return url().type() != storage::kFileSystemTypeTemporary;
  }

  void ComputeHashForSwapFile(HashCallback callback);

  enum class State {
    // The writer accepts write operations.
    kOpen,
    // The writer does not accept write operations and is in the process of
    // closing.
    kClosePending,
    // The writer does not accept write operations and has entered an error
    // state. A swap file may need to be purged.
    kCloseError,
    // The writer does not accept write operations. There should be no more swap
    // file.
    kClosed,
  };
  bool is_closed() const { return state_ != State::kOpen; }
  // Returns whether the File Writer is in a state where any files can be
  // deleted. We do not want to delete the files if there are clean-up
  // operations in-flight.
  bool can_purge() const {
    return state_ == State::kOpen || state_ == State::kCloseError;
  }

  // We write using this file URL. When `Close()` is invoked, we
  // execute a move operation from the swap URL to the target URL at `url_`. In
  // most filesystems, this move operation is atomic.
  storage::FileSystemURL swap_url_;
  State state_ = State::kOpen;

  download::QuarantineConnectionCallback quarantine_connection_callback_;

  // Keeps track of user activation state at creation time for after write
  // checks.
  bool has_transient_user_activation_ = false;

  base::WeakPtr<NativeFileSystemHandleBase> AsWeakPtr() override;

  base::WeakPtrFactory<NativeFileSystemFileWriterImpl> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(NativeFileSystemFileWriterImpl);
};

}  // namespace content

#endif  // CONTENT_BROWSER_FILE_SYSTEM_ACCESS_NATIVE_FILE_SYSTEM_FILE_WRITER_IMPL_H_
