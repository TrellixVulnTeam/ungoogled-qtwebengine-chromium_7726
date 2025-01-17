// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/extensions/api/webstore_private/webstore_private_api.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/browser/extensions/extension_function_test_utils.h"
#include "chrome/browser/extensions/extension_install_prompt.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/webstore_installer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/extension_test_util.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "extensions/browser/api/management/management_api.h"
#include "extensions/browser/extension_dialog_auto_confirm.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/install/extension_install_ui.h"
#include "gpu/config/gpu_feature_type.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "ui/gl/gl_switches.h"

#if BUILDFLAG(ENABLE_SUPERVISED_USERS)
// TODO(https://crbug.com/1060801): Here and elsewhere, possibly switch build
// flag to #if defined(OS_CHROMEOS)
#include "base/test/metrics/histogram_tester.h"
#include "base/test/metrics/user_action_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/supervised_user/logged_in_user_mixin.h"
#include "chrome/browser/supervised_user/supervised_user_constants.h"
#include "chrome/browser/supervised_user/supervised_user_features.h"
#include "chrome/browser/supervised_user/supervised_user_service.h"
#include "chrome/browser/supervised_user/supervised_user_service_factory.h"
#include "chrome/browser/supervised_user/supervised_user_test_util.h"
#include "chrome/browser/ui/supervised_user/parent_permission_dialog.h"
#include "chrome/browser/ui/views/supervised_user/parent_permission_dialog_view.h"
#include "components/account_id/account_id.h"
#include "components/signin/public/identity_manager/identity_test_environment.h"
#include "extensions/common/extension_builder.h"

#if defined(OS_CHROMEOS)
#include "chromeos/constants/chromeos_switches.h"
#endif

#endif  // BUILDFLAG(ENABLE_SUPERVISED_USERS)

namespace utils = extension_function_test_utils;

namespace extensions {

namespace {

class WebstoreInstallListener : public WebstoreInstaller::Delegate {
 public:
  WebstoreInstallListener()
      : received_failure_(false), received_success_(false), waiting_(false) {}

  void OnExtensionInstallSuccess(const std::string& id) override {
    received_success_ = true;
    id_ = id;

    if (waiting_) {
      waiting_ = false;
      base::RunLoop::QuitCurrentWhenIdleDeprecated();
    }
  }

  void OnExtensionInstallFailure(
      const std::string& id,
      const std::string& error,
      WebstoreInstaller::FailureReason reason) override {
    received_failure_ = true;
    id_ = id;
    error_ = error;
    last_failure_reason_ = reason;

    if (waiting_) {
      waiting_ = false;
      base::RunLoop::QuitCurrentWhenIdleDeprecated();
    }
  }

  void Wait() {
    if (received_success_ || received_failure_)
      return;

    waiting_ = true;
    content::RunMessageLoop();
  }
  bool received_success() const { return received_success_; }
  bool received_failure() const { return received_failure_; }
  const std::string& id() const { return id_; }
  WebstoreInstaller::FailureReason last_failure_reason() {
    return last_failure_reason_;
  }

 private:
  bool received_failure_;
  bool received_success_;
  bool waiting_;
  WebstoreInstaller::FailureReason last_failure_reason_;
  std::string id_;
  std::string error_;
};

}  // namespace

// A base class for tests below.
class ExtensionWebstorePrivateApiTest : public ExtensionApiTest {
 public:
  ExtensionWebstorePrivateApiTest() {}
  ~ExtensionWebstorePrivateApiTest() override {}

  void SetUpCommandLine(base::CommandLine* command_line) override {
    ExtensionApiTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(
        switches::kAppsGalleryURL,
        "http://www.example.com/extensions/api_test");
  }

  void SetUpOnMainThread() override {
    ExtensionApiTest::SetUpOnMainThread();

    // Start up the test server and get us ready for calling the install
    // API functions.
    host_resolver()->AddRule("www.example.com", "127.0.0.1");
    ASSERT_TRUE(StartEmbeddedTestServer());
    extensions::ExtensionInstallUI::set_disable_ui_for_tests();

    auto_confirm_install_.reset(
        new ScopedTestDialogAutoConfirm(ScopedTestDialogAutoConfirm::ACCEPT));

    ASSERT_TRUE(webstore_install_dir_.CreateUniqueTempDir());
    webstore_install_dir_copy_ = webstore_install_dir_.GetPath();
    WebstoreInstaller::SetDownloadDirectoryForTests(
        &webstore_install_dir_copy_);
  }

 protected:
  // Returns a test server URL, but with host 'www.example.com' so it matches
  // the web store app's extent that we set up via command line flags.
  GURL DoGetTestServerURL(const std::string& path) {
    GURL url = embedded_test_server()->GetURL(path);

    // Replace the host with 'www.example.com' so it matches the web store
    // app's extent.
    GURL::Replacements replace_host;
    replace_host.SetHostStr("www.example.com");

    return url.ReplaceComponents(replace_host);
  }

  virtual GURL GetTestServerURL(const std::string& path) {
    return DoGetTestServerURL(
        std::string("/extensions/api_test/webstore_private/") + path);
  }

  // Navigates to |page| and runs the Extension API test there. Any downloads
  // of extensions will return the contents of |crx_file|.
  bool RunInstallTest(const std::string& page, const std::string& crx_file) {
    const GURL crx_url = GetTestServerURL(crx_file);
    extension_test_util::SetGalleryUpdateURL(crx_url);

    GURL page_url = GetTestServerURL(page);
    return RunPageTest(page_url.spec());
  }

  content::WebContents* GetWebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  ExtensionService* service() {
    return ExtensionSystem::Get(browser()->profile())->extension_service();
  }

 private:
  base::ScopedTempDir webstore_install_dir_;
  // WebstoreInstaller needs a reference to a FilePath when setting the download
  // directory for testing.
  base::FilePath webstore_install_dir_copy_;

  std::unique_ptr<ScopedTestDialogAutoConfirm> auto_confirm_install_;

  DISALLOW_COPY_AND_ASSIGN(ExtensionWebstorePrivateApiTest);
};

// Test cases for webstore origin frame blocking.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest,
                       FrameWebstorePageBlocked) {
  GURL url = embedded_test_server()->GetURL(
      "/extensions/api_test/webstore_private/noframe.html");
  content::WebContents* web_contents = GetWebContents();
  ui_test_utils::NavigateToURL(browser(), url);

  // Try to load the same URL, but with the current Chrome web store origin in
  // an iframe (i.e. http://www.example.com)
  content::TestNavigationObserver observer(web_contents);
  ASSERT_TRUE(content::ExecuteScript(web_contents, "dropFrame()"));
  EXPECT_TRUE(WaitForLoadStop(web_contents));
  content::RenderFrameHost* subframe =
      content::ChildFrameAt(web_contents->GetMainFrame(), 0);
  ASSERT_TRUE(subframe);

  // The subframe load should fail due to XFO.
  GURL iframe_url = embedded_test_server()->GetURL(
      "www.example.com", "/extensions/api_test/webstore_private/noframe.html");
  EXPECT_EQ(iframe_url, subframe->GetLastCommittedURL());
  EXPECT_FALSE(observer.last_navigation_succeeded());
  EXPECT_EQ(net::ERR_BLOCKED_BY_RESPONSE, observer.last_net_error_code());
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, FrameErrorPageBlocked) {
  GURL url = embedded_test_server()->GetURL(
      "/extensions/api_test/webstore_private/noframe2.html");
  content::WebContents* web_contents = GetWebContents();
  ui_test_utils::NavigateToURL(browser(), url);

  // Try to load the same URL, but with the current Chrome web store origin in
  // an iframe (i.e. http://www.example.com)
  content::TestNavigationObserver observer(web_contents);
  ASSERT_TRUE(content::ExecuteScript(web_contents, "dropFrame()"));
  EXPECT_TRUE(WaitForLoadStop(web_contents));
  content::RenderFrameHost* subframe =
      content::ChildFrameAt(web_contents->GetMainFrame(), 0);
  ASSERT_TRUE(subframe);

  // The subframe load should fail due to XFO.
  GURL iframe_url = embedded_test_server()->GetURL(
      "www.example.com",
      "/nonesuch/extensions/api_test/webstore_private/noframe2.html ");
  EXPECT_EQ(iframe_url, subframe->GetLastCommittedURL());
  EXPECT_FALSE(observer.last_navigation_succeeded());
  EXPECT_EQ(net::ERR_BLOCKED_BY_RESPONSE, observer.last_net_error_code());
}

// Test cases where the user accepts the install confirmation dialog.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, InstallAccepted) {
  ASSERT_TRUE(RunInstallTest("accepted.html", "extension.crx"));
}

// Test having the default download directory missing.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, MissingDownloadDir) {
  // Set a non-existent directory as the download path.
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir temp_dir;
  EXPECT_TRUE(temp_dir.CreateUniqueTempDir());
  base::FilePath missing_directory = temp_dir.Take();
  EXPECT_TRUE(base::DeletePathRecursively(missing_directory));
  WebstoreInstaller::SetDownloadDirectoryForTests(&missing_directory);

  // Now run the install test, which should succeed.
  ASSERT_TRUE(RunInstallTest("accepted.html", "extension.crx"));

  // Cleanup.
  if (base::DirectoryExists(missing_directory))
    EXPECT_TRUE(base::DeletePathRecursively(missing_directory));
}

// Tests passing a localized name.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, InstallLocalized) {
  ASSERT_TRUE(RunInstallTest("localized.html", "localized_extension.crx"));
}

// Now test the case where the user cancels the confirmation dialog.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, InstallCancelled) {
  ScopedTestDialogAutoConfirm auto_cancel(ScopedTestDialogAutoConfirm::CANCEL);
  ASSERT_TRUE(RunInstallTest("cancelled.html", "extension.crx"));
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, IncorrectManifest1) {
  ASSERT_TRUE(RunInstallTest("incorrect_manifest1.html", "extension.crx"));
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, IncorrectManifest2) {
  ASSERT_TRUE(RunInstallTest("incorrect_manifest2.html", "extension.crx"));
}

// Tests that we can request an app installed bubble (instead of the default
// UI when an app is installed).
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, AppInstallBubble) {
  WebstoreInstallListener listener;
  WebstorePrivateApi::SetWebstoreInstallerDelegateForTesting(&listener);
  ASSERT_TRUE(RunInstallTest("app_install_bubble.html", "app.crx"));
  listener.Wait();
  ASSERT_TRUE(listener.received_success());
  ASSERT_EQ("iladmdjkfniedhfhcfoefgojhgaiaccc", listener.id());
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, IsInIncognitoMode) {
  GURL page_url = GetTestServerURL("incognito.html");
  ASSERT_TRUE(RunPageTest(page_url.spec(), kFlagNone, kFlagUseIncognito));
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, IsNotInIncognitoMode) {
  GURL page_url = GetTestServerURL("not_incognito.html");
  ASSERT_TRUE(RunPageTest(page_url.spec()));
}

// Tests using the iconUrl parameter to the install function.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, IconUrl) {
  ASSERT_TRUE(RunInstallTest("icon_url.html", "extension.crx"));
}

// Tests that the Approvals are properly created in beginInstall.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, BeginInstall) {
  std::string appId = "iladmdjkfniedhfhcfoefgojhgaiaccc";
  std::string extensionId = "enfkhcelefdadlmkffamgdlgplcionje";
  ASSERT_TRUE(RunInstallTest("begin_install.html", "extension.crx"));

  std::unique_ptr<WebstoreInstaller::Approval> approval =
      WebstorePrivateApi::PopApprovalForTesting(browser()->profile(), appId);
  EXPECT_EQ(appId, approval->extension_id);
  EXPECT_TRUE(approval->use_app_installed_bubble);
  EXPECT_FALSE(approval->skip_post_install_ui);
  EXPECT_EQ("2", approval->authuser);
  EXPECT_EQ(browser()->profile(), approval->profile);

  approval = WebstorePrivateApi::PopApprovalForTesting(
      browser()->profile(), extensionId);
  EXPECT_EQ(extensionId, approval->extension_id);
  EXPECT_FALSE(approval->use_app_installed_bubble);
  EXPECT_FALSE(approval->skip_post_install_ui);
  EXPECT_TRUE(approval->authuser.empty());
  EXPECT_EQ(browser()->profile(), approval->profile);
}

// Tests that themes are installed without an install prompt.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, InstallTheme) {
  WebstoreInstallListener listener;
  WebstorePrivateApi::SetWebstoreInstallerDelegateForTesting(&listener);
  ASSERT_TRUE(RunInstallTest("theme.html", "../../theme.crx"));
  listener.Wait();
  ASSERT_TRUE(listener.received_success());
  ASSERT_EQ("idlfhncioikpdnlhnmcjogambnefbbfp", listener.id());
}

// Tests that an error is properly reported when an empty crx is returned.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest, EmptyCrx) {
  ASSERT_TRUE(RunInstallTest("empty.html", "empty.crx"));
}

#if BUILDFLAG(ENABLE_SUPERVISED_USERS)
static constexpr char kTestChildEmail[] = "test_child_user@9oo91e.qjz9zk";
static constexpr char kTestChildGaiaId[] = "8u8tuw09sufncmnaos";

class ExtensionWebstorePrivateApiTestChild
    : public ExtensionWebstorePrivateApiTest {
 public:
  ExtensionWebstorePrivateApiTestChild()
      : embedded_test_server_(std::make_unique<net::EmbeddedTestServer>()),
        logged_in_user_mixin_(
            &mixin_host_,
            chromeos::LoggedInUserMixin::LogInType::kChild,
            embedded_test_server_.get(),
            this,
            true /* should_launch_browser */,
            AccountId::FromUserEmailGaiaId(kTestChildEmail, kTestChildGaiaId)) {
    // Suppress regular user login to enable child user login.
    set_chromeos_user_ = false;
  }

  void SetUp() override {
    mixin_host_.SetUp();
    ExtensionWebstorePrivateApiTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    mixin_host_.SetUpCommandLine(command_line);
    ExtensionWebstorePrivateApiTest::SetUpCommandLine(command_line);
    // Shortens the merge session timeout from 20 to 1 seconds to speed up the
    // test by about 19 seconds.
    // TODO (crbug.com/995575): figure out why this switch speeds up the test,
    // and fix the test setup so this is not required.
    command_line->AppendSwitch(switches::kShortMergeSessionTimeoutForTest);
  }

  void SetUpDefaultCommandLine(base::CommandLine* command_line) override {
    mixin_host_.SetUpDefaultCommandLine(command_line);
    ExtensionWebstorePrivateApiTest::SetUpDefaultCommandLine(command_line);
  }

  bool SetUpUserDataDirectory() override {
    return mixin_host_.SetUpUserDataDirectory() &&
           ExtensionWebstorePrivateApiTest::SetUpUserDataDirectory();
  }

  void SetUpInProcessBrowserTestFixture() override {
    mixin_host_.SetUpInProcessBrowserTestFixture();
    ExtensionWebstorePrivateApiTest::SetUpInProcessBrowserTestFixture();
  }

  void CreatedBrowserMainParts(
      content::BrowserMainParts* browser_main_parts) override {
    mixin_host_.CreatedBrowserMainParts(browser_main_parts);
    ExtensionWebstorePrivateApiTest::CreatedBrowserMainParts(
        browser_main_parts);
  }

  void InitializeFamilyData() {
    // Set up the child user's custodians (i.e. parents).
    ASSERT_TRUE(browser());
    supervised_user_test_util::AddCustodians(browser()->profile());

    // Set up the identity test environment, which provides fake
    // OAuth refresh tokens.
    identity_test_env_ = std::make_unique<signin::IdentityTestEnvironment>();
    identity_test_env_->MakeAccountAvailable(kTestChildEmail);
    identity_test_env_->SetPrimaryAccount(kTestChildEmail);
    identity_test_env_->SetRefreshTokenForPrimaryAccount();
    identity_test_env_->SetAutomaticIssueOfAccessTokens(true);
  }

  void SetUpOnMainThread() override {
    mixin_host_.SetUpOnMainThread();
    logged_in_user_mixin_.LogInUser(true /* issue_any_scope_token */);
    ExtensionWebstorePrivateApiTest::SetUpOnMainThread();

    InitializeFamilyData();
    SupervisedUserService* service =
        SupervisedUserServiceFactory::GetForProfile(profile());
    service->SetSupervisedUserExtensionsMayRequestPermissionsPrefForTesting(
        true);
  }

  void TearDownOnMainThread() override {
    mixin_host_.TearDownOnMainThread();
    ExtensionWebstorePrivateApiTest::TearDownOnMainThread();
  }

  void TearDownInProcessBrowserTestFixture() override {
    mixin_host_.TearDownInProcessBrowserTestFixture();
    ExtensionWebstorePrivateApiTest::TearDownInProcessBrowserTestFixture();
  }

  void TearDown() override {
    mixin_host_.TearDown();
    ExtensionWebstorePrivateApiTest::TearDown();
  }

  chromeos::LoggedInUserMixin* GetLoggedInUserMixin() {
    return &logged_in_user_mixin_;
  }

  void SetNextReAuthStatus(
      const GaiaAuthConsumer::ReAuthProofTokenStatus next_status) {
    GetLoggedInUserMixin()
        ->GetFakeGaiaMixin()
        ->fake_gaia()
        ->SetNextReAuthStatus(next_status);
  }

 protected:
  std::unique_ptr<signin::IdentityTestEnvironment> identity_test_env_;

 private:
  // Replicate what MixinBasedInProcessBrowserTest does since inheriting from
  // that class is inconvenient here.
  InProcessBrowserTestMixinHost mixin_host_;
  // Create another embedded test server to avoid starting the same one twice.
  std::unique_ptr<net::EmbeddedTestServer> embedded_test_server_;
  chromeos::LoggedInUserMixin logged_in_user_mixin_;
};

class ExtensionWebstorePrivateApiTestChildInstallDisabled
    : public ExtensionWebstorePrivateApiTestChild {
 public:
  ExtensionWebstorePrivateApiTestChildInstallDisabled() {
    feature_list_.InitWithFeatures(
        {}, {supervised_users::kSupervisedUserInitiatedExtensionInstall});
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

// Tests that extension installation is blocked for child accounts when
// the feature flag is disabled.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTestChildInstallDisabled,
                       InstallBlocked) {
  base::HistogramTester histogram_tester;
  base::UserActionTester user_action_tester;
  ASSERT_TRUE(RunInstallTest("install_blocked_child.html", "app.crx"));
  histogram_tester.ExpectUniqueSample(
      SupervisedUserExtensionsMetricsRecorder::kEnablementHistogramName,
      SupervisedUserExtensionsMetricsRecorder::EnablementState::kFailedToEnable,
      1);
  histogram_tester.ExpectTotalCount(
      SupervisedUserExtensionsMetricsRecorder::kEnablementHistogramName, 1);
  EXPECT_EQ(
      1,
      user_action_tester.GetActionCount(
          SupervisedUserExtensionsMetricsRecorder::kFailedToEnableActionName));
}

static constexpr char kTestAppId[] = "iladmdjkfniedhfhcfoefgojhgaiaccc";
static constexpr char kTestAppVersion[] = "0.1";

// Test fixture for various cases of installation for child accounts
// when the feature flag is enabled.
class ExtensionWebstorePrivateApiTestChildInstallEnabled
    : public ExtensionWebstorePrivateApiTestChild,
      public TestParentPermissionDialogViewObserver {
 public:
  // The next dialog action to take.
  enum class NextDialogAction {
    kCancel,
    kAccept,
  };

  ExtensionWebstorePrivateApiTestChildInstallEnabled()
      : TestParentPermissionDialogViewObserver(this) {
    feature_list_.InitWithFeatures(
        {supervised_users::kSupervisedUserInitiatedExtensionInstall}, {});
  }

  // TestParentPermissionDialogViewObserver override:
  void OnTestParentPermissionDialogViewCreated(
      ParentPermissionDialogView* view) override {
    view->SetRepromptAfterIncorrectCredential(false);
    view->SetIdentityManagerForTesting(identity_test_env_->identity_manager());
    // Everything is set up, so take the next action.
    if (next_dialog_action_) {
      switch (next_dialog_action_.value()) {
        case NextDialogAction::kCancel:
          view->CancelDialog();
          break;
        case NextDialogAction::kAccept:
          view->AcceptDialog();
          break;
      }
    }
  }

  void set_next_dialog_action(NextDialogAction action) {
    next_dialog_action_ = action;
  }

 private:
  base::test::ScopedFeatureList feature_list_;
  base::Optional<NextDialogAction> next_dialog_action_;
};

// Tests install for a child when parent permission is granted.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTestChildInstallEnabled,
                       ParentPermissionGranted) {
  WebstoreInstallListener listener;
  WebstorePrivateApi::SetWebstoreInstallerDelegateForTesting(&listener);
  set_next_dialog_action(NextDialogAction::kAccept);

  // Tell the Reauth API client to return a success for the next reauth
  // request.
  SetNextReAuthStatus(GaiaAuthConsumer::ReAuthProofTokenStatus::kSuccess);
  ASSERT_TRUE(RunInstallTest("install_child.html", "app.crx"));
  listener.Wait();
  ASSERT_TRUE(listener.received_success());
  ASSERT_EQ(kTestAppId, listener.id());

  scoped_refptr<const Extension> extension =
      extensions::ExtensionBuilder("test extension")
          .SetID(kTestAppId)
          .SetVersion(kTestAppVersion)
          .Build();
  SupervisedUserService* service =
      SupervisedUserServiceFactory::GetForProfile(profile());
  ASSERT_TRUE(service->IsExtensionAllowed(*extension));
}

// Tests no install occurs for a child when the parent permission
// dialog is canceled.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTestChildInstallEnabled,
                       ParentPermissionCanceled) {
  WebstoreInstallListener listener;
  set_next_dialog_action(NextDialogAction::kCancel);
  WebstorePrivateApi::SetWebstoreInstallerDelegateForTesting(&listener);
  ASSERT_TRUE(RunInstallTest("install_cancel_child.html", "app.crx"));
  listener.Wait();
  ASSERT_TRUE(listener.received_failure());
  ASSERT_EQ(kTestAppId, listener.id());
  ASSERT_EQ(listener.last_failure_reason(),
            WebstoreInstaller::FailureReason::FAILURE_REASON_CANCELLED);
  scoped_refptr<const Extension> extension =
      extensions::ExtensionBuilder("test extension")
          .SetID(kTestAppId)
          .SetVersion(kTestAppVersion)
          .Build();
  SupervisedUserService* service =
      SupervisedUserServiceFactory::GetForProfile(profile());
  ASSERT_FALSE(service->IsExtensionAllowed(*extension));
}

// Tests that no parent permission is required for a child to install a theme.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTestChildInstallEnabled,
                       NoParentPermissionRequiredForTheme) {
  WebstoreInstallListener listener;
  WebstorePrivateApi::SetWebstoreInstallerDelegateForTesting(&listener);
  ASSERT_TRUE(RunInstallTest("theme.html", "../../theme.crx"));
  listener.Wait();
  ASSERT_TRUE(listener.received_success());
  ASSERT_EQ("idlfhncioikpdnlhnmcjogambnefbbfp", listener.id());
}

// Tests that even if the kSupervisedUserInitiatedExtensionInstall feature flag
// is enabled, supervised user extension installs are blocked if the
// "Permissions for sites, apps and extensions" toggle is off.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTestChildInstallEnabled,
                       InstallBlockedWhenPermissionsToggleOff) {
  base::HistogramTester histogram_tester;
  base::UserActionTester user_action_tester;

  SupervisedUserService* service =
      SupervisedUserServiceFactory::GetForProfile(profile());
  service->SetSupervisedUserExtensionsMayRequestPermissionsPrefForTesting(
      false);

  set_next_dialog_action(NextDialogAction::kAccept);
  // Tell the Reauth API client to return a success for the next reauth
  // request.
  SetNextReAuthStatus(GaiaAuthConsumer::ReAuthProofTokenStatus::kSuccess);
  ASSERT_TRUE(RunInstallTest("install_blocked_child.html", "app.crx"));
  histogram_tester.ExpectUniqueSample(
      SupervisedUserExtensionsMetricsRecorder::kEnablementHistogramName,
      SupervisedUserExtensionsMetricsRecorder::EnablementState::kFailedToEnable,
      1);
  histogram_tester.ExpectTotalCount(
      SupervisedUserExtensionsMetricsRecorder::kEnablementHistogramName, 1);
  EXPECT_EQ(
      1,
      user_action_tester.GetActionCount(
          SupervisedUserExtensionsMetricsRecorder::kFailedToEnableActionName));
}

#endif  // BUILDFLAG(ENABLE_SUPERVISED_USERS)

class ExtensionWebstoreGetWebGLStatusTest : public InProcessBrowserTest {
 protected:
  void RunTest(bool webgl_allowed) {
    // If Gpu access is disallowed then WebGL will not be available.
    if (!content::GpuDataManager::GetInstance()->GpuAccessAllowed(NULL))
      webgl_allowed = false;

    static const char kEmptyArgs[] = "[]";
    static const char kWebGLStatusAllowed[] = "webgl_allowed";
    static const char kWebGLStatusBlocked[] = "webgl_blocked";
    scoped_refptr<WebstorePrivateGetWebGLStatusFunction> function =
        new WebstorePrivateGetWebGLStatusFunction();
    std::unique_ptr<base::Value> result(utils::RunFunctionAndReturnSingleResult(
        function.get(), kEmptyArgs, browser()));
    ASSERT_TRUE(result);
    EXPECT_EQ(base::Value::Type::STRING, result->type());
    std::string webgl_status;
    EXPECT_TRUE(result->GetAsString(&webgl_status));
    EXPECT_STREQ(webgl_allowed ? kWebGLStatusAllowed : kWebGLStatusBlocked,
                 webgl_status.c_str());
  }
};

// Tests getWebGLStatus function when WebGL is allowed.
IN_PROC_BROWSER_TEST_F(ExtensionWebstoreGetWebGLStatusTest, Allowed) {
  bool webgl_allowed = true;
  RunTest(webgl_allowed);
}

// Tests getWebGLStatus function when WebGL is blocklisted.
IN_PROC_BROWSER_TEST_F(ExtensionWebstoreGetWebGLStatusTest, Blocked) {
  content::GpuDataManager::GetInstance()->BlocklistWebGLForTesting();

  bool webgl_allowed = false;
  RunTest(webgl_allowed);
}

class ExtensionWebstorePrivateGetReferrerChainApiTest
    : public ExtensionWebstorePrivateApiTest {
 public:
  void SetUpOnMainThread() override {
    host_resolver()->AddRule("redirect1.com", "127.0.0.1");
    host_resolver()->AddRule("redirect2.com", "127.0.0.1");

    ExtensionWebstorePrivateApiTest::SetUpOnMainThread();
  }

  GURL GetTestServerURLWithReferrers(const std::string& path) {
    // Hand craft a url that will cause the test server to issue redirects.
    const std::vector<std::string> redirects = {"redirect1.com",
                                                "redirect2.com"};
    net::HostPortPair host_port = embedded_test_server()->host_port_pair();
    std::string redirect_chain;
    for (const auto& redirect : redirects) {
      std::string redirect_url = base::StringPrintf(
          "http://%s:%d/server-redirect?", redirect.c_str(), host_port.port());
      redirect_chain += redirect_url;
    }

    return GURL(redirect_chain + GetTestServerURL(path).spec());
  }
};

// Tests that the GetReferrerChain API returns the redirect information.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateGetReferrerChainApiTest,
                       GetReferrerChain) {
  GURL page_url = GetTestServerURLWithReferrers("referrer_chain.html");
  ASSERT_TRUE(RunPageTest(page_url.spec()));
}

// Tests that the GetReferrerChain API returns an empty string for profiles
// opted out of SafeBrowsing.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateGetReferrerChainApiTest,
                       GetReferrerChainForNonSafeBrowsingUser) {
  PrefService* pref_service = browser()->profile()->GetPrefs();
  EXPECT_TRUE(pref_service->GetBoolean(prefs::kSafeBrowsingEnabled));
  // Disable SafeBrowsing.
  pref_service->SetBoolean(prefs::kSafeBrowsingEnabled, false);

  GURL page_url = GetTestServerURLWithReferrers("empty_referrer_chain.html");
  ASSERT_TRUE(RunPageTest(page_url.spec()));
}

}  // namespace extensions
