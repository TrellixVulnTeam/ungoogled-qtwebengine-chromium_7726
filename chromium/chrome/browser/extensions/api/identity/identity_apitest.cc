// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/bind_test_util.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "build/build_config.h"
#include "build/buildflag.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/extensions/api/identity/gaia_remote_consent_flow.h"
#include "chrome/browser/extensions/api/identity/gaia_web_auth_flow.h"
#include "chrome/browser/extensions/api/identity/identity_api.h"
#include "chrome/browser/extensions/api/identity/identity_constants.h"
#include "chrome/browser/extensions/api/identity/identity_get_accounts_function.h"
#include "chrome/browser/extensions/api/identity/identity_get_auth_token_error.h"
#include "chrome/browser/extensions/api/identity/identity_get_auth_token_function.h"
#include "chrome/browser/extensions/api/identity/identity_get_profile_user_info_function.h"
#include "chrome/browser/extensions/api/identity/identity_launch_web_auth_flow_function.h"
#include "chrome/browser/extensions/api/identity/identity_remove_cached_auth_token_function.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/browser/extensions/extension_browsertest.h"
#include "chrome/browser/extensions/extension_function_test_utils.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/account_consistency_mode_manager.h"
#include "chrome/browser/signin/account_reconcilor_factory.h"
#include "chrome/browser/signin/chrome_signin_client_factory.h"
#include "chrome/browser/signin/chrome_signin_client_test_util.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/browser/signin/identity_test_environment_profile_adaptor.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/api/identity.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/crx_file/id_util.h"
#include "components/guest_view/browser/guest_view_base.h"
#include "components/prefs/pref_service.h"
#include "components/signin/core/browser/account_reconcilor.h"
#include "components/signin/public/base/list_accounts_test_utils.h"
#include "components/signin/public/base/signin_pref_names.h"
#include "components/signin/public/base/signin_switches.h"
#include "components/signin/public/identity_manager/accounts_mutator.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/identity_test_utils.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/api_test_utils.h"
#include "extensions/common/extension_builder.h"
#include "extensions/common/extension_features.h"
#include "google_apis/gaia/oauth2_mint_token_flow.h"
#include "net/cookies/cookie_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/login/users/mock_user_manager.h"
#include "chrome/browser/chromeos/net/network_portal_detector_test_impl.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chromeos/network/network_handler.h"
#include "chromeos/network/network_state.h"
#include "chromeos/network/network_state_handler.h"
#include "chromeos/tpm/stub_install_attributes.h"
#include "components/user_manager/scoped_user_manager.h"
#endif

using guest_view::GuestViewBase;
using testing::_;
using testing::Return;

namespace extensions {

namespace {

namespace errors = identity_constants;
namespace utils = extension_function_test_utils;

const char kAccessToken[] = "auth_token";
const char kExtensionId[] = "ext_id";

const char kGetAuthTokenResultHistogramName[] =
    "Signin.Extensions.GetAuthTokenResult";
const char kGetAuthTokenResultAfterConsentApprovedHistogramName[] =
    "Signin.Extensions.GetAuthTokenResult.RemoteConsentApproved";

#if defined(OS_CHROMEOS)
void InitNetwork() {
  const chromeos::NetworkState* default_network =
      chromeos::NetworkHandler::Get()
          ->network_state_handler()
          ->DefaultNetwork();

  auto* portal_detector = new chromeos::NetworkPortalDetectorTestImpl();
  portal_detector->SetDefaultNetworkForTesting(default_network->guid());

  chromeos::NetworkPortalDetector::CaptivePortalState online_state;
  online_state.status =
      chromeos::NetworkPortalDetector::CAPTIVE_PORTAL_STATUS_ONLINE;
  online_state.response_code = 204;
  portal_detector->SetDetectionResultsForTesting(default_network->guid(),
                                                 online_state);

  chromeos::network_portal_detector::InitializeForTesting(portal_detector);
}
#endif

// Asynchronous function runner allows tests to manipulate the browser window
// after the call happens.
class AsyncFunctionRunner {
 public:
  void RunFunctionAsync(ExtensionFunction* function,
                        const std::string& args,
                        content::BrowserContext* browser_context) {
    response_delegate_.reset(new api_test_utils::SendResponseHelper(function));
    std::unique_ptr<base::ListValue> parsed_args(utils::ParseList(args));
    ASSERT_TRUE(parsed_args.get())
        << "Could not parse extension function arguments: " << args;
    function->SetArgs(base::Value::FromUniquePtrValue(std::move(parsed_args)));

    if (!function->extension()) {
      scoped_refptr<const Extension> empty_extension(
          ExtensionBuilder("Test").Build());
      function->set_extension(empty_extension.get());
    }

    function->set_browser_context(browser_context);
    function->set_has_callback(true);
    function->RunWithValidation()->Execute();
  }

  std::string WaitForError(ExtensionFunction* function) {
    RunMessageLoopUntilResponse();
    CHECK(function->response_type());
    EXPECT_EQ(ExtensionFunction::FAILED, *function->response_type());
    return function->GetError();
  }

  void WaitForTwoResults(ExtensionFunction* function,
                         base::Value* first_result,
                         base::Value* second_result) {
    RunMessageLoopUntilResponse();
    EXPECT_TRUE(function->GetError().empty())
        << "Unexpected error: " << function->GetError();
    EXPECT_NE(nullptr, function->GetResultList());

    const auto& result_list = function->GetResultList()->GetList();
    EXPECT_EQ(2ul, result_list.size());

    *first_result = result_list[0].Clone();
    *second_result = result_list[1].Clone();
  }

 private:
  void RunMessageLoopUntilResponse() {
    response_delegate_->WaitForResponse();
    EXPECT_TRUE(response_delegate_->has_response());
  }

  std::unique_ptr<api_test_utils::SendResponseHelper> response_delegate_;
};

class AsyncExtensionBrowserTest : public ExtensionBrowserTest {
 protected:
  // Provide wrappers of AsynchronousFunctionRunner for convenience.
  void RunFunctionAsync(ExtensionFunction* function, const std::string& args) {
    async_function_runner_ = std::make_unique<AsyncFunctionRunner>();
    async_function_runner_->RunFunctionAsync(function, args,
                                             browser()->profile());
  }

  std::string WaitForError(ExtensionFunction* function) {
    return async_function_runner_->WaitForError(function);
  }

  void WaitForTwoResults(ExtensionFunction* function,
                         base::Value* first_result,
                         base::Value* second_result) {
    return async_function_runner_->WaitForTwoResults(function, first_result,
                                                     second_result);
  }

 private:
  std::unique_ptr<AsyncFunctionRunner> async_function_runner_;
};

class TestHangOAuth2MintTokenFlow : public OAuth2MintTokenFlow {
 public:
  TestHangOAuth2MintTokenFlow()
      : OAuth2MintTokenFlow(NULL, OAuth2MintTokenFlow::Parameters()) {}

  void Start(scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
             const std::string& access_token) override {
    // Do nothing, simulating a hanging network call.
  }
};

class TestOAuth2MintTokenFlow : public OAuth2MintTokenFlow {
 public:
  enum ResultType {
    ISSUE_ADVICE_SUCCESS,
    REMOTE_CONSENT_SUCCESS,
    MINT_TOKEN_SUCCESS,
    MINT_TOKEN_FAILURE,
    MINT_TOKEN_BAD_CREDENTIALS,
    MINT_TOKEN_SERVICE_ERROR
  };

  TestOAuth2MintTokenFlow(ResultType result,
                          const std::set<std::string>* requested_scopes,
                          const std::set<std::string>& granted_scopes,
                          OAuth2MintTokenFlow::Delegate* delegate)
      : OAuth2MintTokenFlow(delegate, OAuth2MintTokenFlow::Parameters()),
        result_(result),
        requested_scopes_(requested_scopes),
        granted_scopes_(granted_scopes),
        delegate_(delegate) {}

  void Start(scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
             const std::string& access_token) override {
    switch (result_) {
      case ISSUE_ADVICE_SUCCESS: {
        IssueAdviceInfo info;
        delegate_->OnIssueAdviceSuccess(info);
        break;
      }
      case REMOTE_CONSENT_SUCCESS: {
        RemoteConsentResolutionData resolution_data;
        delegate_->OnRemoteConsentSuccess(resolution_data);
        break;
      }
      case MINT_TOKEN_SUCCESS: {
        if (granted_scopes_.empty())
          delegate_->OnMintTokenSuccess(kAccessToken, *requested_scopes_, 3600);
        else
          delegate_->OnMintTokenSuccess(kAccessToken, granted_scopes_, 3600);
        break;
      }
      case MINT_TOKEN_FAILURE: {
        GoogleServiceAuthError error(GoogleServiceAuthError::CONNECTION_FAILED);
        delegate_->OnMintTokenFailure(error);
        break;
      }
      case MINT_TOKEN_BAD_CREDENTIALS: {
        GoogleServiceAuthError error(
            GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS);
        delegate_->OnMintTokenFailure(error);
        break;
      }
      case MINT_TOKEN_SERVICE_ERROR: {
        GoogleServiceAuthError error =
            GoogleServiceAuthError::FromServiceError("invalid_scope");
        delegate_->OnMintTokenFailure(error);
        break;
      }
    }
  }

 private:
  ResultType result_;
  const std::set<std::string>* requested_scopes_;
  std::set<std::string> granted_scopes_;
  OAuth2MintTokenFlow::Delegate* delegate_;
};

// Waits for a specific GURL to generate a NOTIFICATION_LOAD_STOP event and
// saves a pointer to the window embedding the WebContents, which can be later
// closed.
class WaitForGURLAndCloseWindow : public content::WindowedNotificationObserver {
 public:
  explicit WaitForGURLAndCloseWindow(GURL url)
      : WindowedNotificationObserver(
            content::NOTIFICATION_LOAD_STOP,
            content::NotificationService::AllSources()),
        url_(std::move(url)),
        embedder_web_contents_(nullptr) {}

  // NotificationObserver:
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override {
    content::NavigationController* web_auth_flow_controller =
        content::Source<content::NavigationController>(source).ptr();
    content::WebContents* web_contents =
        web_auth_flow_controller->GetWebContents();

    if (web_contents->GetLastCommittedURL() == url_) {
      // It is safe to keep the pointer here, because we know in a test, that
      // the WebContents won't go away before CloseEmbedderWebContents is
      // called. Don't copy this code to production.
      GuestViewBase* guest = GuestViewBase::FromWebContents(web_contents);
      embedder_web_contents_ = guest->embedder_web_contents();
      // Condtionally invoke parent class so that Wait will not exit
      // until the target URL arrives.
      content::WindowedNotificationObserver::Observe(type, source, details);
    }
  }

  // Closes the window embedding the WebContents. The action is separated from
  // the Observe method to make sure the list of observers is not deleted,
  // while some event is already being processed. (That causes ASAN failures.)
  void CloseEmbedderWebContents() {
    if (embedder_web_contents_)
      embedder_web_contents_->Close();
  }

 private:
  GURL url_;
  content::WebContents* embedder_web_contents_;
};

}  // namespace

class FakeGetAuthTokenFunction : public IdentityGetAuthTokenFunction {
 public:
  FakeGetAuthTokenFunction()
      : login_access_token_result_(true),
        auto_login_access_token_(true),
        login_ui_result_(true),
        scope_ui_result_(true),
        scope_ui_async_(false),
        scope_ui_failure_(GaiaWebAuthFlow::WINDOW_CLOSED),
        login_ui_shown_(false),
        scope_ui_shown_(false) {}

  void set_login_access_token_result(bool result) {
    login_access_token_result_ = result;
  }

  void set_auto_login_access_token(bool automatic) {
    auto_login_access_token_ = automatic;
  }

  void set_login_ui_result(bool result) { login_ui_result_ = result; }

  void push_mint_token_flow(std::unique_ptr<OAuth2MintTokenFlow> flow) {
    flow_queue_.push(std::move(flow));
  }

  void push_mint_token_result(
      TestOAuth2MintTokenFlow::ResultType result_type,
      const std::set<std::string>& granted_scopes = {}) {
    // If `granted_scopes` is empty, `TestOAuth2MintTokenFlow` returns the
    // requested scopes (retrieved from `token_key`) in a mint token success
    // flow by default. Since the scopes in `token_key` may be populated at a
    // later time, the requested scopes cannot be immediately copied, so a
    // pointer is passed instead.
    const ExtensionTokenKey* token_key = GetExtensionTokenKeyForTest();
    push_mint_token_flow(std::make_unique<TestOAuth2MintTokenFlow>(
        result_type, &token_key->scopes, granted_scopes, this));
  }

  // Sets scope UI to not complete immediately. Call
  // CompleteOAuthApprovalDialog() or CompleteRemoteConsentDialog() after
  // |on_scope_ui_shown| is invoked to unblock execution.
  void set_scope_ui_async(base::OnceClosure on_scope_ui_shown) {
    scope_ui_async_ = true;
    on_scope_ui_shown_ = std::move(on_scope_ui_shown);
  }

  void set_scope_ui_failure(GaiaWebAuthFlow::Failure failure) {
    scope_ui_result_ = false;
    scope_ui_failure_ = failure;
  }

  void set_scope_ui_service_error(const GoogleServiceAuthError& service_error) {
    scope_ui_result_ = false;
    scope_ui_failure_ = GaiaWebAuthFlow::SERVICE_AUTH_ERROR;
    scope_ui_service_error_ = service_error;
  }

  void set_scope_ui_oauth_error(const std::string& oauth_error) {
    scope_ui_result_ = false;
    scope_ui_failure_ = GaiaWebAuthFlow::OAUTH_ERROR;
    scope_ui_oauth_error_ = oauth_error;
  }

  void set_remote_consent_gaia_id(const std::string& gaia_id) {
    remote_consent_gaia_id_ = gaia_id;
  }

  bool login_ui_shown() const { return login_ui_shown_; }

  bool scope_ui_shown() const { return scope_ui_shown_; }

  std::vector<std::string> login_access_tokens() const {
    return login_access_tokens_;
  }

  void StartTokenKeyAccountAccessTokenRequest() override {
    if (auto_login_access_token_) {
      base::Optional<std::string> access_token("access_token");
      GoogleServiceAuthError error = GoogleServiceAuthError::AuthErrorNone();
      if (!login_access_token_result_) {
        access_token = base::nullopt;
        error = GoogleServiceAuthError(
            GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS);
      }
      OnGetAccessTokenComplete(
          access_token, base::Time::Now() + base::TimeDelta::FromHours(1LL),
          error);
    } else {
      // Make a request to the IdentityManager. The test now must tell the
      // service to issue an access token (or an error).
      IdentityGetAuthTokenFunction::StartTokenKeyAccountAccessTokenRequest();
    }
  }

#if defined(OS_CHROMEOS)
  void StartDeviceAccessTokenRequest() override {
    // In these tests requests for the device account just funnel through to
    // requests for the token key account.
    StartTokenKeyAccountAccessTokenRequest();
  }
#endif

  // Fix auth error on secondary account or add a new account.
  void FixOrAddSecondaryAccount() {
    signin::IdentityManager* identity_manager =
        IdentityManagerFactory::GetForProfile(GetProfile());
    std::vector<CoreAccountInfo> accounts =
        identity_manager->GetAccountsWithRefreshTokens();
    CoreAccountId primary_id = identity_manager->GetPrimaryAccountId();
    bool fixed_auth_error = false;
    for (const auto& account_info : accounts) {
      CoreAccountId account_id = account_info.account_id;
      if (account_id == primary_id)
        continue;
      if (identity_manager->HasAccountWithRefreshTokenInPersistentErrorState(
              account_id)) {
        identity_manager->GetAccountsMutator()->AddOrUpdateAccount(
            account_info.gaia, account_info.email, "token",
            account_info.is_under_advanced_protection,
            signin_metrics::SourceForRefreshTokenOperation::kUnknown);
        fixed_auth_error = true;
      }
    }
    if (!fixed_auth_error) {
      signin::MakeAccountAvailable(identity_manager, "secondary@example.com");
    }
  }

  // Simulate signin through a login prompt.
  void ShowExtensionLoginPrompt() override {
    EXPECT_FALSE(login_ui_shown_);
    login_ui_shown_ = true;
    if (login_ui_result_) {
      signin::IdentityManager* identity_manager =
          IdentityManagerFactory::GetForProfile(GetProfile());
      if (IdentityAPI::GetFactoryInstance()
              ->Get(GetProfile())
              ->AreExtensionsRestrictedToPrimaryAccount()) {
        // Set a primary account.
        ASSERT_FALSE(identity_manager->HasPrimaryAccount());
        signin::MakeAccountAvailable(identity_manager, "primary@example.com");
        signin::SetPrimaryAccount(identity_manager, "primary@example.com");
      } else {
        FixOrAddSecondaryAccount();
      }
    } else {
      SigninFailed();
    }
  }

  void ShowOAuthApprovalDialog(const IssueAdviceInfo& issue_advice) override {
    scope_ui_shown_ = true;
    if (!scope_ui_async_)
      CompleteOAuthApprovalDialog();
    else
      std::move(on_scope_ui_shown_).Run();
  }

  void CompleteOAuthApprovalDialog() {
    if (scope_ui_result_) {
      OnGaiaFlowCompleted(kAccessToken, "3600");
    } else if (scope_ui_failure_ == GaiaWebAuthFlow::SERVICE_AUTH_ERROR) {
      OnGaiaFlowFailure(scope_ui_failure_, scope_ui_service_error_, "");
    } else {
      GoogleServiceAuthError error(GoogleServiceAuthError::NONE);
      OnGaiaFlowFailure(scope_ui_failure_, error, scope_ui_oauth_error_);
    }
  }

  void ShowRemoteConsentDialog(
      const RemoteConsentResolutionData& resolution_data) override {
    scope_ui_shown_ = true;
    if (!scope_ui_async_)
      CompleteRemoteConsentDialog();
    else
      std::move(on_scope_ui_shown_).Run();
  }

  void CompleteRemoteConsentDialog() {
    if (scope_ui_result_) {
      OnGaiaRemoteConsentFlowApproved("fake_consent_result",
                                      remote_consent_gaia_id_);
    } else {
      OnGaiaRemoteConsentFlowFailed(GaiaRemoteConsentFlow::WINDOW_CLOSED);
    }
  }

  void StartGaiaRequest(const std::string& login_access_token) override {
    // Save the login token used in the mint token flow so tests can see
    // what account was used.
    login_access_tokens_.push_back(login_access_token);
    IdentityGetAuthTokenFunction::StartGaiaRequest(login_access_token);
  }

  std::unique_ptr<OAuth2MintTokenFlow> CreateMintTokenFlow() override {
    auto flow = std::move(flow_queue_.front());
    flow_queue_.pop();
    return flow;
  }

  bool enable_granular_permissions() const {
    return IdentityGetAuthTokenFunction::enable_granular_permissions();
  }

  std::string GetSelectedUserId() const {
    return IdentityGetAuthTokenFunction::GetSelectedUserId();
  }

 private:
  ~FakeGetAuthTokenFunction() override {}
  bool login_access_token_result_;
  bool auto_login_access_token_;
  bool login_ui_result_;
  bool scope_ui_result_;
  bool scope_ui_async_;
  base::OnceClosure on_scope_ui_shown_;
  GaiaWebAuthFlow::Failure scope_ui_failure_;
  GoogleServiceAuthError scope_ui_service_error_;
  std::string scope_ui_oauth_error_;
  bool login_ui_shown_;
  bool scope_ui_shown_;

  std::queue<std::unique_ptr<OAuth2MintTokenFlow>> flow_queue_;

  std::vector<std::string> login_access_tokens_;

  std::string remote_consent_gaia_id_;
};

class MockQueuedMintRequest : public IdentityMintRequestQueue::Request {
 public:
  MOCK_METHOD1(StartMintToken, void(IdentityMintRequestQueue::MintType));
};

class IdentityTestWithSignin : public AsyncExtensionBrowserTest {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    AsyncExtensionBrowserTest::SetUpInProcessBrowserTestFixture();

    create_services_subscription_ =
        BrowserContextDependencyManager::GetInstance()
            ->RegisterCreateServicesCallbackForTesting(base::BindRepeating(
                &IdentityTestWithSignin::OnWillCreateBrowserContextServices,
                base::Unretained(this)));
  }

  void OnWillCreateBrowserContextServices(content::BrowserContext* context) {
    IdentityTestEnvironmentProfileAdaptor::
        SetIdentityTestEnvironmentFactoriesOnBrowserContext(context);

    ChromeSigninClientFactory::GetInstance()->SetTestingFactory(
        context, base::BindRepeating(&BuildChromeSigninClientWithURLLoader,
                                     &test_url_loader_factory_));
  }

  void SetUpOnMainThread() override {
    AsyncExtensionBrowserTest::SetUpOnMainThread();

#if defined(OS_CHROMEOS)
    // Fake the network online state so that Gaia requests can come through.
    InitNetwork();
#endif

    identity_test_env_profile_adaptor_ =
        std::make_unique<IdentityTestEnvironmentProfileAdaptor>(profile());

    // This test requires these callbacks to be fired on account
    // update/removal.
    identity_test_env()->EnableRemovalOfExtendedAccountInfo();

    identity_test_env()->SetTestURLLoaderFactory(&test_url_loader_factory_);
  }

  void TearDownOnMainThread() override {
    // Must be destroyed before the Profile.
    identity_test_env_profile_adaptor_.reset();
  }

 protected:
  // Returns the account ID of the created account.
  CoreAccountId SignIn(const std::string& email) {
    auto account_info = identity_test_env()->MakePrimaryAccountAvailable(email);
    return account_info.account_id;
  }

  IdentityAPI* id_api() {
    return IdentityAPI::GetFactoryInstance()->Get(browser()->profile());
  }

  signin::IdentityTestEnvironment* identity_test_env() {
    return identity_test_env_profile_adaptor_->identity_test_env();
  }

  network::TestURLLoaderFactory test_url_loader_factory_;

  std::unique_ptr<IdentityTestEnvironmentProfileAdaptor>
      identity_test_env_profile_adaptor_;

  std::unique_ptr<
      BrowserContextDependencyManager::CreateServicesCallbackList::Subscription>
      create_services_subscription_;
};

class IdentityGetAccountsFunctionTest : public IdentityTestWithSignin {
 public:
  IdentityGetAccountsFunctionTest() = default;

 protected:
  testing::AssertionResult ExpectGetAccounts(
      const std::vector<std::string>& gaia_ids) {
    scoped_refptr<IdentityGetAccountsFunction> func(
        new IdentityGetAccountsFunction);
    func->set_extension(
        ExtensionBuilder("Test").SetID(kExtensionId).Build().get());
    if (!utils::RunFunction(func.get(), std::string("[]"), browser(),
                            api_test_utils::NONE)) {
      return GenerateFailureResult(gaia_ids, NULL)
             << "getAccounts did not return a result.";
    }
    const base::ListValue* callback_arguments = func->GetResultList();
    if (!callback_arguments)
      return GenerateFailureResult(gaia_ids, NULL) << "NULL result";

    if (callback_arguments->GetSize() != 1) {
      return GenerateFailureResult(gaia_ids, NULL)
             << "Expected 1 argument but got " << callback_arguments->GetSize();
    }

    const base::ListValue* results;
    if (!callback_arguments->GetList(0, &results))
      GenerateFailureResult(gaia_ids, NULL) << "Result was not an array";

    std::set<std::string> result_ids;
    for (const base::Value& item : *results) {
      std::unique_ptr<api::identity::AccountInfo> info =
          api::identity::AccountInfo::FromValue(item);
      if (info.get())
        result_ids.insert(info->id);
      else
        return GenerateFailureResult(gaia_ids, results);
    }

    for (const std::string& gaia_id : gaia_ids) {
      if (result_ids.find(gaia_id) == result_ids.end())
        return GenerateFailureResult(gaia_ids, results);
    }

    return testing::AssertionResult(true);
  }

  testing::AssertionResult GenerateFailureResult(
      const ::std::vector<std::string>& gaia_ids,
      const base::ListValue* results) {
    testing::Message msg("Expected: ");
    for (const std::string& gaia_id : gaia_ids) {
      msg << gaia_id << " ";
    }
    msg << "Actual: ";
    if (!results) {
      msg << "NULL";
    } else {
      for (const auto& result : *results) {
        std::unique_ptr<api::identity::AccountInfo> info =
            api::identity::AccountInfo::FromValue(result);
        if (info.get())
          msg << info->id << " ";
        else
          msg << result << "<-" << result.type() << " ";
      }
    }

    return testing::AssertionFailure(msg);
  }
};

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest, AllAccountsOn) {
  EXPECT_FALSE(id_api()->AreExtensionsRestrictedToPrimaryAccount());
}

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest, NoneSignedIn) {
  EXPECT_TRUE(ExpectGetAccounts({}));
}

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest, NoPrimaryAccount) {
  identity_test_env()->MakeAccountAvailable("secondary@example.com");
  EXPECT_TRUE(ExpectGetAccounts({}));
}

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest,
                       PrimaryAccountHasInvalidRefreshToken) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");
  identity_test_env()->SetInvalidRefreshTokenForPrimaryAccount();
  EXPECT_TRUE(ExpectGetAccounts({}));
}

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest,
                       PrimaryAccountSignedIn) {
  SignIn("primary@example.com");
  EXPECT_TRUE(ExpectGetAccounts({"gaia_id_for_primary_example.com"}));
}

IN_PROC_BROWSER_TEST_F(IdentityGetAccountsFunctionTest, TwoAccountsSignedIn) {
  SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");
  if (!id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
    EXPECT_TRUE(ExpectGetAccounts({"gaia_id_for_primary_example.com",
                                   "gaia_id_for_secondary_example.com"}));
  } else {
    EXPECT_TRUE(ExpectGetAccounts({"gaia_id_for_primary_example.com"}));
  }
}

class IdentityGetProfileUserInfoFunctionTest : public IdentityTestWithSignin {
 protected:
  std::unique_ptr<api::identity::ProfileUserInfo> RunGetProfileUserInfo() {
    scoped_refptr<IdentityGetProfileUserInfoFunction> func(
        new IdentityGetProfileUserInfoFunction);
    func->set_extension(
        ExtensionBuilder("Test").SetID(kExtensionId).Build().get());
    std::unique_ptr<base::Value> value(
        utils::RunFunctionAndReturnSingleResult(func.get(), "[]", browser()));
    return api::identity::ProfileUserInfo::FromValue(*value);
  }

  std::unique_ptr<api::identity::ProfileUserInfo>
  RunGetProfileUserInfoWithEmail() {
    scoped_refptr<IdentityGetProfileUserInfoFunction> func(
        new IdentityGetProfileUserInfoFunction);
    func->set_extension(CreateExtensionWithEmailPermission());
    std::unique_ptr<base::Value> value(
        utils::RunFunctionAndReturnSingleResult(func.get(), "[]", browser()));
    return api::identity::ProfileUserInfo::FromValue(*value);
  }

  scoped_refptr<const Extension> CreateExtensionWithEmailPermission() {
    return ExtensionBuilder("Test").AddPermission("identity.email").Build();
  }
};

IN_PROC_BROWSER_TEST_F(IdentityGetProfileUserInfoFunctionTest, NotSignedIn) {
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithEmail();
  EXPECT_TRUE(info->email.empty());
  EXPECT_TRUE(info->id.empty());
}

IN_PROC_BROWSER_TEST_F(IdentityGetProfileUserInfoFunctionTest, SignedIn) {
  SignIn("president@example.com");
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithEmail();
  EXPECT_EQ("president@example.com", info->email);
  EXPECT_EQ("gaia_id_for_president_example.com", info->id);
}

IN_PROC_BROWSER_TEST_F(IdentityGetProfileUserInfoFunctionTest,
                       SignedInUnconsented) {
  identity_test_env()->MakeUnconsentedPrimaryAccountAvailable(
      "test@example.com");
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithEmail();
  EXPECT_TRUE(info->email.empty());
  EXPECT_TRUE(info->id.empty());
}

IN_PROC_BROWSER_TEST_F(IdentityGetProfileUserInfoFunctionTest,
                       NotSignedInNoEmail) {
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfo();
  EXPECT_TRUE(info->email.empty());
  EXPECT_TRUE(info->id.empty());
}

IN_PROC_BROWSER_TEST_F(IdentityGetProfileUserInfoFunctionTest,
                       SignedInNoEmail) {
  SignIn("president@example.com");
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfo();
  EXPECT_TRUE(info->email.empty());
  EXPECT_TRUE(info->id.empty());
}

class IdentityGetProfileUserInfoFunctionTestWithAccountStatusParam
    : public IdentityGetProfileUserInfoFunctionTest,
      public ::testing::WithParamInterface<std::string> {
 protected:
  std::unique_ptr<api::identity::ProfileUserInfo>
  RunGetProfileUserInfoWithAccountStatus() {
    scoped_refptr<IdentityGetProfileUserInfoFunction> func(
        new IdentityGetProfileUserInfoFunction);
    func->set_extension(CreateExtensionWithEmailPermission());
    std::string args = base::StringPrintf(R"([{"accountStatus": "%s"}])",
                                          account_status().c_str());
    std::unique_ptr<base::Value> value(
        utils::RunFunctionAndReturnSingleResult(func.get(), args, browser()));
    return api::identity::ProfileUserInfo::FromValue(*value);
  }

  std::string account_status() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(
    All,
    IdentityGetProfileUserInfoFunctionTestWithAccountStatusParam,
    ::testing::Values("SYNC", "ANY"));

IN_PROC_BROWSER_TEST_P(
    IdentityGetProfileUserInfoFunctionTestWithAccountStatusParam,
    NotSignedIn) {
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithAccountStatus();
  EXPECT_TRUE(info->email.empty());
  EXPECT_TRUE(info->id.empty());
}

IN_PROC_BROWSER_TEST_P(
    IdentityGetProfileUserInfoFunctionTestWithAccountStatusParam,
    SignedIn) {
  SignIn("test@example.com");
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithAccountStatus();
  EXPECT_EQ("test@example.com", info->email);
  EXPECT_EQ("gaia_id_for_test_example.com", info->id);
}

IN_PROC_BROWSER_TEST_P(
    IdentityGetProfileUserInfoFunctionTestWithAccountStatusParam,
    SignedInUnconsented) {
  identity_test_env()->MakeUnconsentedPrimaryAccountAvailable(
      "test@example.com");
  std::unique_ptr<api::identity::ProfileUserInfo> info =
      RunGetProfileUserInfoWithAccountStatus();
  // The unconsented (Sync off) primary account is returned conditionally,
  // depending on the accountStatus parameter.
  if (account_status() == "ANY") {
    EXPECT_EQ("test@example.com", info->email);
    EXPECT_EQ("gaia_id_for_test_example.com", info->id);
  } else {
    // accountStatus is SYNC or unspecified.
    EXPECT_TRUE(info->email.empty());
    EXPECT_TRUE(info->id.empty());
  }
}

class GetAuthTokenFunctionTest
    : public IdentityTestWithSignin,
      public signin::IdentityManager::DiagnosticsObserver {
 public:
  explicit GetAuthTokenFunctionTest(bool is_return_scopes_enabled = true,
                                    bool is_selected_user_id_enabled = true) {
    std::vector<base::Feature> enabled_features;
    std::vector<base::Feature> disabled_features;
    if (is_return_scopes_enabled) {
      enabled_features.push_back(
          extensions_features::kReturnScopesInGetAuthToken);
    } else {
      disabled_features.push_back(
          extensions_features::kReturnScopesInGetAuthToken);
    }

    if (is_selected_user_id_enabled) {
      enabled_features.push_back(
          extensions_features::kSelectedUserIdInGetAuthToken);
    } else {
      disabled_features.push_back(
          extensions_features::kSelectedUserIdInGetAuthToken);
    }

    feature_list_.InitWithFeatures(enabled_features, disabled_features);
  }

  std::string IssueLoginAccessTokenForAccount(const CoreAccountId& account_id) {
    std::string access_token = "access_token-" + account_id.ToString();
    identity_test_env()
        ->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
            account_id, access_token,
            base::Time::Now() + base::TimeDelta::FromSeconds(3600));
    return access_token;
  }

 protected:
  enum OAuth2Fields { NONE = 0, CLIENT_ID = 1, SCOPES = 2, AS_COMPONENT = 4 };

  void SetUpOnMainThread() override {
    IdentityTestWithSignin::SetUpOnMainThread();
    identity_test_env()->identity_manager()->AddDiagnosticsObserver(this);
    signin::SetListAccountsResponseNoAccounts(&test_url_loader_factory_);
  }

  void TearDownOnMainThread() override {
    identity_test_env()->identity_manager()->RemoveDiagnosticsObserver(this);
    IdentityTestWithSignin::TearDownOnMainThread();
  }

  ~GetAuthTokenFunctionTest() override {}

  // Helper to create an extension with specific OAuth2Info fields set.
  // |fields_to_set| should be computed by using fields of Oauth2Fields enum.
  const Extension* CreateExtension(int fields_to_set) {
    const Extension* ext;
    base::FilePath manifest_path =
        test_data_dir_.AppendASCII("platform_apps/oauth2");
    base::FilePath component_manifest_path =
        test_data_dir_.AppendASCII("packaged_app/component_oauth2");
    if ((fields_to_set & AS_COMPONENT) == 0)
      ext = LoadExtension(manifest_path);
    else
      ext = LoadExtensionAsComponent(component_manifest_path);
    OAuth2Info& oauth2_info =
        const_cast<OAuth2Info&>(OAuth2Info::GetOAuth2Info(ext));
    if ((fields_to_set & CLIENT_ID) != 0)
      oauth2_info.client_id = "client1";
    if ((fields_to_set & SCOPES) != 0) {
      oauth2_info.scopes.push_back("scope1");
      oauth2_info.scopes.push_back("scope2");
    }

    extension_id_ = ext->id();
    oauth_scopes_ = std::set<std::string>(oauth2_info.scopes.begin(),
                                          oauth2_info.scopes.end());
    return ext;
  }

  CoreAccountInfo GetPrimaryAccountInfo() {
    return identity_test_env()->identity_manager()->GetPrimaryAccountInfo();
  }

  CoreAccountId GetPrimaryAccountId() {
    return identity_test_env()->identity_manager()->GetPrimaryAccountId();
  }

  IdentityTokenCacheValue CreateToken(const std::string& token,
                                      base::TimeDelta time_to_live) {
    return IdentityTokenCacheValue::CreateToken(token, oauth_scopes_,
                                                time_to_live);
  }

  // Sets a cached token for the primary account.
  void SetCachedToken(const IdentityTokenCacheValue& token_data) {
    SetCachedTokenForAccount(GetPrimaryAccountInfo(), token_data);
  }

  void SetCachedTokenForAccount(const CoreAccountInfo account_info,
                                const IdentityTokenCacheValue& token_data) {
    ExtensionTokenKey key(extension_id_, account_info, oauth_scopes_);
    id_api()->token_cache()->SetToken(key, token_data);
  }

  void SetCachedGaiaId(const std::string& gaia_id) {
    id_api()->SetGaiaIdForExtension(extension_id_, gaia_id);
  }

  const IdentityTokenCacheValue& GetCachedToken(
      const CoreAccountInfo& account_info,
      const std::set<std::string>& scopes) {
    ExtensionTokenKey key(
        extension_id_,
        account_info.IsEmpty() ? GetPrimaryAccountInfo() : account_info,
        scopes);
    return id_api()->token_cache()->GetToken(key);
  }

  const IdentityTokenCacheValue& GetCachedToken(
      const CoreAccountInfo& account_info) {
    return GetCachedToken(account_info, oauth_scopes_);
  }

  base::Optional<std::string> GetCachedGaiaId() {
    return id_api()->GetGaiaIdForExtension(extension_id_);
  }

  void QueueRequestStart(IdentityMintRequestQueue::MintType type,
                         IdentityMintRequestQueue::Request* request) {
    ExtensionTokenKey key(extension_id_, GetPrimaryAccountInfo(),
                          oauth_scopes_);
    id_api()->mint_queue()->RequestStart(type, key, request);
  }

  void QueueRequestComplete(IdentityMintRequestQueue::MintType type,
                            IdentityMintRequestQueue::Request* request) {
    ExtensionTokenKey key(extension_id_, GetPrimaryAccountInfo(),
                          oauth_scopes_);
    id_api()->mint_queue()->RequestComplete(type, key, request);
  }

  base::HistogramTester* histogram_tester() { return &histogram_tester_; }

  base::OnceClosure on_access_token_requested_;

  void RunGetAuthTokenFunction(ExtensionFunction* function,
                               const std::string& args,
                               Browser* browser,
                               std::string* access_token,
                               std::set<std::string>* granted_scopes) {
    EXPECT_TRUE(
        utils::RunFunction(function, args, browser, api_test_utils::NONE));

    EXPECT_TRUE(function->GetError().empty())
        << "Unexpected error: " << function->GetError();
    EXPECT_NE(nullptr, function->GetResultList());

    const auto& result_list = function->GetResultList()->GetList();
    EXPECT_EQ(2ul, result_list.size());

    const auto& access_token_value = result_list[0];
    const auto& granted_scopes_value = result_list[1];
    EXPECT_TRUE(access_token_value.is_string());
    EXPECT_TRUE(granted_scopes_value.is_list());

    std::set<std::string> scopes;
    for (const auto& scope : granted_scopes_value.GetList()) {
      EXPECT_TRUE(scope.is_string());
      scopes.insert(scope.GetString());
    }

    *access_token = access_token_value.GetString();
    *granted_scopes = std::move(scopes);
  }

  void WaitForGetAuthTokenResults(
      ExtensionFunction* function,
      std::string* access_token,
      std::set<std::string>* granted_scopes,
      AsyncFunctionRunner* function_runner = nullptr) {
    base::Value access_token_value;
    base::Value granted_scopes_value;
    if (function_runner == nullptr) {
      WaitForTwoResults(function, &access_token_value, &granted_scopes_value);
    } else {
      function_runner->WaitForTwoResults(function, &access_token_value,
                                         &granted_scopes_value);
    }
    EXPECT_TRUE(access_token_value.is_string());
    EXPECT_TRUE(granted_scopes_value.is_list());

    std::set<std::string> scopes;
    for (const auto& scope : granted_scopes_value.GetList()) {
      EXPECT_TRUE(scope.is_string());
      scopes.insert(scope.GetString());
    }

    *access_token = access_token_value.GetString();
    *granted_scopes = std::move(scopes);
  }

 private:
  // signin::IdentityManager::DiagnosticsObserver:
  void OnAccessTokenRequested(const CoreAccountId& account_id,
                              const std::string& consumer_id,
                              const signin::ScopeSet& scopes) override {
    if (on_access_token_requested_.is_null())
      return;
    std::move(on_access_token_requested_).Run();
  }

  base::test::ScopedFeatureList feature_list_;
  base::HistogramTester histogram_tester_;
  std::string extension_id_;
  std::set<std::string> oauth_scopes_;
};

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NoClientId) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(SCOPES));
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kInvalidClientId), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kInvalidClientId, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NoScopes) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID));
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kInvalidScopes), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kEmptyScopes, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NonInteractiveNotSignedIn) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kUserNotSignedIn), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kUserNotSignedIn, 1);
}

// The signin flow is simply not used on ChromeOS.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveNotSignedInShowSigninOnlyOnce) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(false);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kUserNotSignedIn), error);
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kSignInFailed, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       PRE_InteractiveNotSignedAndSigninNotAllowed) {
  // kSigninAllowed cannot be set after the profile creation. Use
  // kSigninAllowedOnNextStartup instead.
  browser()->profile()->GetPrefs()->SetBoolean(
      prefs::kSigninAllowedOnNextStartup, false);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveNotSignedAndSigninNotAllowed) {
  ASSERT_FALSE(
      browser()->profile()->GetPrefs()->GetBoolean(prefs::kSigninAllowed));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(false);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kBrowserSigninNotAllowed), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kBrowserSigninNotAllowed, 1);
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NonInteractiveMintFailure) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_FAILURE);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveLoginAccessTokenFailure) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_access_token_result(false);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGetAccessTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveMintAdviceSuccess) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kNoGrant), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_ADVICE,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaConsentInteractionRequired, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveMintBadCredentials) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_BAD_CREDENTIALS);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveMintServiceError) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_SERVICE_ERROR);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveMintServiceErrorAccountValid) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_SERVICE_ERROR);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));

  // The login UI should not have been shown, as the user's primary account is
  // in a valid state.
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

// The signin flow is simply not used on ChromeOS.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveMintServiceErrorShowSigninOnlyOnce) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_SERVICE_ERROR);

  // The function should complete with an error, showing the signin UI only
  // once for the initial signin.
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NoOptionsSuccess) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                          &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NonInteractiveSuccess) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                          &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveLoginCanceled) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(false);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kUserNotSignedIn), error);
// ChromeOS does not support the interactive login flow, so the login UI will
// never be shown on that platform.
#if !defined(OS_CHROMEOS)
  EXPECT_TRUE(func->login_ui_shown());
#endif
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kSignInFailed, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveMintBadCredentialsAccountValid) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_BAD_CREDENTIALS);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  // The login UI should not be shown as the account is in a valid state.
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

// The interactive login flow is always short-circuited out with failure on
// ChromeOS, so the tests of the interactive login flow being successful are not
// relevant on that platform.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessMintFailure) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_FAILURE);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessMintBadCredentials) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->push_mint_token_result(
      TestOAuth2MintTokenFlow::MINT_TOKEN_BAD_CREDENTIALS);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessLoginAccessTokenFailure) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->set_login_access_token_result(false);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGetAccessTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessMintSuccess) {
  // TODO(courage): verify that account_id in token service requests
  // is correct once manual token minting for tests is implemented.
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"interactive\": true}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessApprovalAborted) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::WINDOW_CLOSED);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kUserRejected), error);
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowRejected, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveLoginSuccessApprovalSuccess) {
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"interactive\": true}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveApprovalAborted) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::WINDOW_CLOSED);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kUserRejected), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowRejected, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalLoadFailed) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::LOAD_FAILED);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kPageLoadFailure), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kPageLoadFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalInvalidRedirect) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::INVALID_REDIRECT);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kInvalidRedirect), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kInvalidRedirect, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalConnectionFailure) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_service_error(
      GoogleServiceAuthError(GoogleServiceAuthError::CONNECTION_FAILED));
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalServiceErrorAccountValid) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_service_error(
      GoogleServiceAuthError(GoogleServiceAuthError::SERVICE_ERROR));
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));

  // The login UI should not be shown as the account is in a valid state.
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowAuthFailure, 1);
}

// The signin flow is simply not used on ChromeOS.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalServiceErrorShowSigninUIOnlyOnce) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_login_ui_result(true);
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_service_error(
      GoogleServiceAuthError(GoogleServiceAuthError::SERVICE_ERROR));

  // The function should complete with an error, showing the signin UI only
  // once for the initial signin.
  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));

  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowAuthFailure, 1);
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveApprovalOAuthErrors) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));

  struct TestCase {
    std::string oauth_error;
    std::string error_message;
    IdentityGetAuthTokenError::State error_state;
  };

  std::vector<TestCase> test_cases;
  test_cases.push_back({"access_denied", errors::kUserRejected,
                        IdentityGetAuthTokenError::State::kOAuth2AccessDenied});
  test_cases.push_back(
      {"invalid_scope", errors::kInvalidScopes,
       IdentityGetAuthTokenError::State::kOAuth2InvalidScopes});
  test_cases.push_back({"unmapped_error",
                        std::string(errors::kAuthFailure) + "unmapped_error",
                        IdentityGetAuthTokenError::State::kOAuth2Failure});

  for (const auto& test_case : test_cases) {
    base::HistogramTester histogram_tester;
    scoped_refptr<FakeGetAuthTokenFunction> func(
        new FakeGetAuthTokenFunction());
    func->set_extension(extension.get());
    // Make sure we don't get a cached issue_advice result, which would cause
    // flow to be leaked.
    id_api()->token_cache()->EraseAllTokens();
    func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
    func->set_scope_ui_oauth_error(test_case.oauth_error);
    std::string error = utils::RunFunctionAndReturnError(
        func.get(), "[{\"interactive\": true}]", browser());
    EXPECT_EQ(test_case.error_message, error);
    EXPECT_FALSE(func->login_ui_shown());
    EXPECT_TRUE(func->scope_ui_shown());
    histogram_tester.ExpectUniqueSample(kGetAuthTokenResultHistogramName,
                                        test_case.error_state, 1);
  }
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveApprovalSuccess) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"interactive\": true}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

#if !defined(OS_MAC)
// Test for http://crbug.com/753014
//
// On macOS, closing all browsers does not shut down the browser process.
// TODO(http://crbug.com/756462): Figure out how to shut down the browser
// process on macOS and enable this test on macOS as well.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveSigninFailedDuringBrowserProcessShutDown) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_service_error(
      GoogleServiceAuthError(GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS));
  func->set_login_ui_result(false);

  // Closing all browsers ensures that the browser process is shutting down.
  CloseAllBrowsers();

  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  // Check that the OAuth approval dialog is shown to ensure that the Gaia flow
  // fails with an |SERVICE_AUTH_ERROR| error (with |INVALID_GAIA_CREDENTIALS|
  // service error). This reproduces the crash conditions in bug
  // http://crbug.com/753014.
  // This condition may be fragile as it depends on the identity manager not
  // being destroyed before the OAuth approval dialog is shown.
  EXPECT_TRUE(func->scope_ui_shown());

  // The login screen should not be shown when the browser process is shutting
  // down.
  EXPECT_FALSE(func->login_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowAuthFailure, 1);
}
#endif  // !defined(OS_MAC)

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NoninteractiveQueue) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // Create a fake request to block the queue.
  MockQueuedMintRequest queued_request;
  IdentityMintRequestQueue::MintType type =
      IdentityMintRequestQueue::MINT_TYPE_NONINTERACTIVE;

  EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
  QueueRequestStart(type, &queued_request);

  // The real request will start processing, but wait in the queue behind
  // the blocker.
  RunFunctionAsync(func.get(), "[{}]");
  // Verify that we have fetched the login token at this point.
  testing::Mock::VerifyAndClearExpectations(func.get());

  // The flow will be created after the first queued request clears.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  QueueRequestComplete(type, &queued_request);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveQueue) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // Create a fake request to block the queue.
  MockQueuedMintRequest queued_request;
  IdentityMintRequestQueue::MintType type =
      IdentityMintRequestQueue::MINT_TYPE_INTERACTIVE;

  EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
  QueueRequestStart(type, &queued_request);

  // The real request will start processing, but wait in the queue behind
  // the blocker.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  RunFunctionAsync(func.get(), "[{\"interactive\": true}]");
  // Verify that we have fetched the login token and run the first flow.
  testing::Mock::VerifyAndClearExpectations(func.get());
  EXPECT_FALSE(func->scope_ui_shown());

  // The UI will be displayed and a token retrieved after the first
  // queued request clears.
  QueueRequestComplete(type, &queued_request);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveQueueShutdown) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // Create a fake request to block the queue.
  MockQueuedMintRequest queued_request;
  IdentityMintRequestQueue::MintType type =
      IdentityMintRequestQueue::MINT_TYPE_INTERACTIVE;

  EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
  QueueRequestStart(type, &queued_request);

  // The real request will start processing, but wait in the queue behind
  // the blocker.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  RunFunctionAsync(func.get(), "[{\"interactive\": true}]");
  // Verify that we have fetched the login token and run the first flow.
  testing::Mock::VerifyAndClearExpectations(func.get());
  EXPECT_FALSE(func->scope_ui_shown());

  // After the request is canceled, the function will complete.
  func->OnIdentityAPIShutdown();
  EXPECT_EQ(std::string(errors::kCanceled), WaitForError(func.get()));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());

  QueueRequestComplete(type, &queued_request);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kCanceled, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NoninteractiveShutdown) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  func->push_mint_token_flow(std::make_unique<TestHangOAuth2MintTokenFlow>());
  RunFunctionAsync(func.get(), "[{\"interactive\": false}]");

  // After the request is canceled, the function will complete.
  func->OnIdentityAPIShutdown();
  EXPECT_EQ(std::string(errors::kCanceled), WaitForError(func.get()));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kCanceled, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       InteractiveQueuedNoninteractiveFails) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // Create a fake request to block the interactive queue.
  MockQueuedMintRequest queued_request;
  IdentityMintRequestQueue::MintType type =
      IdentityMintRequestQueue::MINT_TYPE_INTERACTIVE;

  EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
  QueueRequestStart(type, &queued_request);

  // Non-interactive requests fail without hitting GAIA, because a
  // consent UI is known to be up.
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kNoGrant), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());

  QueueRequestComplete(type, &queued_request);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaConsentInteractionAlreadyRunning,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, NonInteractiveCacheHit) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // pre-populate the cache with a token
  IdentityTokenCacheValue token =
      CreateToken(kAccessToken, base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);

  // Get a token. Should not require a GAIA request.
  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                          &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Checks that the first account in Gaia cookie can be used when extensions are
// not restricted to the primary account.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveCacheHitSecondary) {
  Profile* profile = browser()->profile();
  // Lock the reconcilor so that Google cookies can be configured manually.
  AccountReconcilor::Lock reconcilor_lock(
      AccountReconcilorFactory::GetForProfile(profile));
  // Add a secondary account in Chrome and in cookies.
  AccountInfo account_info =
      identity_test_env()->MakeAccountAvailable("email@example.com");
  content::RunAllTasksUntilIdle();  // Flush pending ListAccounts calls.
  signin::SetListAccountsResponseOneAccount(
      account_info.email, account_info.gaia, &test_url_loader_factory_);
  auto* identity_manager = IdentityManagerFactory::GetForProfile(profile);
  signin::SetFreshnessOfAccountsInGaiaCookie(identity_manager, false);

  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // pre-populate the cache with a token
  IdentityTokenCacheValue token =
      CreateToken(kAccessToken, base::TimeDelta::FromSeconds(3600));
  SetCachedTokenForAccount(account_info, token);

  if (id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
    // Fail when there is no primary account.
    std::string error =
        utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
    EXPECT_EQ(std::string(errors::kUserNotSignedIn), error);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kUserNotSignedIn, 1);
  } else {
    // Use the account from Gaia cookies.
    std::string access_token;
    std::set<std::string> granted_scopes;
    RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                            &granted_scopes);
    EXPECT_EQ(std::string(kAccessToken), access_token);
    EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kNone, 1);
  }

  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       NonInteractiveIssueAdviceCacheHit) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // pre-populate the cache with advice
  IssueAdviceInfo info;
  IdentityTokenCacheValue token =
      IdentityTokenCacheValue::CreateIssueAdvice(info);
  SetCachedToken(token);

  // Should return an error without a GAIA request.
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_EQ(std::string(errors::kNoGrant), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaConsentInteractionRequired, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, InteractiveCacheHit) {
  SignIn("primary@example.com");
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());

  // Create a fake request to block the queue.
  MockQueuedMintRequest queued_request;
  IdentityMintRequestQueue::MintType type =
      IdentityMintRequestQueue::MINT_TYPE_INTERACTIVE;

  EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
  QueueRequestStart(type, &queued_request);

  // The real request will start processing, but wait in the queue behind
  // the blocker.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  RunFunctionAsync(func.get(), "[{\"interactive\": true}]");
  base::RunLoop().RunUntilIdle();

  // Populate the cache with a token while the request is blocked.
  IdentityTokenCacheValue token =
      CreateToken(kAccessToken, base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);

  // When we wake up the request, it returns the cached token without
  // displaying a UI, or hitting GAIA.
  QueueRequestComplete(type, &queued_request);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// The interactive login UI is never shown on ChromeOS, so tests of the
// interactive login flow being successful are not relevant on that platform.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, LoginInvalidatesTokenCache) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  // pre-populate the cache with a token
  IdentityTokenCacheValue token =
      CreateToken(kAccessToken, base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);

  // Because the user is not signed in, the token will be removed,
  // and we'll hit GAIA for new tokens.
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"interactive\": true}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_TRUE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_NOTFOUND,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       IssueAdviceInvalidatesGaiaIdCache) {
  SignIn("primary@example.com");
  AccountInfo secondary_account_info =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  // Pre-populate the gaia id cache.
  SetCachedGaiaId(secondary_account_info.gaia);

  // The user revoked their token and must give a consent again. Gaia disabled
  // the new flow for the secondary account.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"interactive\": true}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  EXPECT_TRUE(func->scope_ui_shown());
  EXPECT_FALSE(GetCachedGaiaId().has_value());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       IssueAdviceFailureInvalidatesGaiaIdCache) {
  SignIn("primary@example.com");
  AccountInfo secondary_account_info =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  // Pre-populate the gaia id cache.
  SetCachedGaiaId(secondary_account_info.gaia);

  // The user revoked their token and must give a consent again. Gaia disabled
  // the new flow for the secondary account.
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::WINDOW_CLOSED);

  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"interactive\": true}]", browser());
  EXPECT_EQ(std::string(errors::kUserRejected), error);
  EXPECT_TRUE(func->scope_ui_shown());
  EXPECT_FALSE(GetCachedGaiaId().has_value());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowRejected, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ComponentWithChromeClientId) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->ignore_did_respond_for_testing();
  scoped_refptr<const Extension> extension(
      CreateExtension(SCOPES | AS_COMPONENT));
  func->set_extension(extension.get());
  const OAuth2Info& oauth2_info = OAuth2Info::GetOAuth2Info(extension.get());
  EXPECT_TRUE(oauth2_info.client_id.empty());
  EXPECT_FALSE(func->GetOAuth2ClientId().empty());
  EXPECT_NE("client1", func->GetOAuth2ClientId());
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ComponentWithNormalClientId) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->ignore_did_respond_for_testing();
  scoped_refptr<const Extension> extension(
      CreateExtension(CLIENT_ID | SCOPES | AS_COMPONENT));
  func->set_extension(extension.get());
  EXPECT_EQ("client1", func->GetOAuth2ClientId());
}

// Ensure that IdentityAPI shutdown triggers an active function call to return
// with an error.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, IdentityAPIShutdown) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  // Have GetAuthTokenFunction actually make the request for the access token to
  // ensure that the function doesn't immediately succeed.
  func->set_auto_login_access_token(false);
  RunFunctionAsync(func.get(), "[{}]");

  id_api()->Shutdown();
  EXPECT_EQ(std::string(errors::kCanceled), WaitForError(func.get()));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kCanceled, 1);
}

// Ensure that when there are multiple active function calls, IdentityAPI
// shutdown triggers them all to return with errors.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       IdentityAPIShutdownWithMultipleActiveTokenRequests) {
  // Set up two extension functions, having them actually make the request for
  // the access token to ensure that they don't immediately succeed.
  scoped_refptr<FakeGetAuthTokenFunction> func1(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension1(
      CreateExtension(CLIENT_ID | SCOPES));
  func1->set_extension(extension1.get());
  func1->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func1->set_auto_login_access_token(false);

  scoped_refptr<FakeGetAuthTokenFunction> func2(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension2(
      CreateExtension(CLIENT_ID | SCOPES));
  func2->set_extension(extension2.get());
  func2->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func2->set_auto_login_access_token(false);

  // Run both functions. Note that it's necessary to use AsyncFunctionRunner
  // directly here rather than the AsyncExtensionBrowserTest instance methods
  // that wrap it, as each AsyncFunctionRunner instance sets itself as the
  // delegate of exactly one function.
  AsyncFunctionRunner func1_runner;
  func1_runner.RunFunctionAsync(func1.get(), "[{}]", browser()->profile());

  AsyncFunctionRunner func2_runner;
  func2_runner.RunFunctionAsync(func2.get(), "[{}]", browser()->profile());

  // Shut down IdentityAPI and ensure that both functions complete with an
  // error.
  id_api()->Shutdown();
  EXPECT_EQ(std::string(errors::kCanceled),
            func1_runner.WaitForError(func1.get()));
  EXPECT_EQ(std::string(errors::kCanceled),
            func2_runner.WaitForError(func2.get()));
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ManuallyIssueToken) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  // Have GetAuthTokenFunction actually make the request for the access token.
  func->set_auto_login_access_token(false);

  base::RunLoop run_loop;
  on_access_token_requested_ = run_loop.QuitClosure();
  RunFunctionAsync(func.get(), "[{}]");
  run_loop.Run();

  std::string primary_account_access_token =
      IssueLoginAccessTokenForAccount(primary_account_id);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  EXPECT_THAT(func->login_access_tokens(),
              testing::ElementsAre(primary_account_access_token));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ManuallyIssueTokenFailure) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  // Have GetAuthTokenFunction actually make the request for the access token.
  func->set_auto_login_access_token(false);

  base::RunLoop run_loop;
  on_access_token_requested_ = run_loop.QuitClosure();
  RunFunctionAsync(func.get(), "[{}]");
  run_loop.Run();

  identity_test_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
      primary_account_id,
      GoogleServiceAuthError(GoogleServiceAuthError::SERVICE_UNAVAILABLE));

  EXPECT_EQ(
      std::string(errors::kAuthFailure) +
          GoogleServiceAuthError(GoogleServiceAuthError::SERVICE_UNAVAILABLE)
              .ToString(),
      WaitForError(func.get()));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGetAccessTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiDefaultUserManuallyIssueToken) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->set_auto_login_access_token(false);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  base::RunLoop run_loop;
  on_access_token_requested_ = run_loop.QuitClosure();
  RunFunctionAsync(func.get(), "[{}]");
  run_loop.Run();

  std::string primary_account_access_token =
      IssueLoginAccessTokenForAccount(primary_account_id);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  EXPECT_THAT(func->login_access_tokens(),
              testing::ElementsAre(primary_account_access_token));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiPrimaryUserManuallyIssueToken) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->set_auto_login_access_token(false);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  base::RunLoop run_loop;
  on_access_token_requested_ = run_loop.QuitClosure();
  RunFunctionAsync(
      func.get(),
      "[{\"account\": { \"id\": \"gaia_id_for_primary_example.com\" } }]");
  run_loop.Run();

  std::string primary_account_access_token =
      IssueLoginAccessTokenForAccount(primary_account_id);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  EXPECT_THAT(func->login_access_tokens(),
              testing::ElementsAre(primary_account_access_token));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryUserManuallyIssueToken) {
  SignIn("primary@example.com");
  CoreAccountInfo secondary_account =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->set_auto_login_access_token(false);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  const char kFunctionParams[] =
      "[{\"account\": { \"id\": \"gaia_id_for_secondary_example.com\" } }]";

  if (id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
    // Fail if extensions are restricted to the primary account.
    std::string error = utils::RunFunctionAndReturnError(
        func.get(), kFunctionParams, browser());
    EXPECT_EQ(std::string(errors::kUserNonPrimary), error);
    EXPECT_FALSE(func->login_ui_shown());
    EXPECT_FALSE(func->scope_ui_shown());
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kUserNonPrimary, 1);
    return;
  }

  base::RunLoop run_loop;
  on_access_token_requested_ = run_loop.QuitClosure();
  RunFunctionAsync(func.get(), kFunctionParams);
  run_loop.Run();

  std::string secondary_account_access_token =
      IssueLoginAccessTokenForAccount(secondary_account.account_id);

  std::string access_token;
  std::set<std::string> granted_scopes;
  WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(secondary_account).status());
  EXPECT_THAT(func->login_access_tokens(),
              testing::ElementsAre(secondary_account_access_token));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiUnknownUserGetTokenFromTokenServiceFailure) {
  SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->set_auto_login_access_token(false);

  std::string error = utils::RunFunctionAndReturnError(
      func.get(), "[{\"account\": { \"id\": \"unknown@example.com\" } }]",
      browser());
  std::string expected_error;
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
    EXPECT_EQ(errors::kUserNonPrimary, error);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kUserNonPrimary, 1);
  } else {
    EXPECT_EQ(errors::kUserNotSignedIn, error);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kUserNotSignedIn, 1);
  }
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryNonInteractiveMintFailure) {
  // This test is only relevant if extensions see all accounts.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_FAILURE);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(),
      "[{\"account\": { \"id\": \"gaia_id_for_secondary_example.com\" } }]",
      browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryNonInteractiveLoginAccessTokenFailure) {
  // This test is only relevant if extensions see all accounts.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_login_access_token_result(false);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(),
      "[{\"account\": { \"id\": \"gaia_id_for_secondary_example.com\" } }]",
      browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGetAccessTokenAuthFailure, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryInteractiveApprovalAborted) {
  // This test is only relevant if extensions see all accounts.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  SignIn("primary@example.com");
  identity_test_env()->MakeAccountAvailable("secondary@example.com");

  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);
  func->set_scope_ui_failure(GaiaWebAuthFlow::WINDOW_CLOSED);
  std::string error = utils::RunFunctionAndReturnError(
      func.get(),
      "[{\"account\": { \"id\": \"gaia_id_for_secondary_example.com\" }, "
      "\"interactive\": true}]",
      browser());
  EXPECT_EQ(std::string(errors::kUserRejected), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_TRUE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kGaiaFlowRejected, 1);
}

// Tests that Chrome remembers user's choice of an account at the end of the
// remote consent flow. Chrome should reuse this account in the next
// getAuthToken() call for the same extension.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryInteractiveRemoteConsent) {
  CoreAccountId primary_account_id = SignIn("primary@example.com");
  AccountInfo secondary_account =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");
  const extensions::Extension* extension = CreateExtension(CLIENT_ID | SCOPES);

  {
    scoped_refptr<FakeGetAuthTokenFunction> func(
        new FakeGetAuthTokenFunction());
    func->set_extension(extension);
    func->push_mint_token_result(
        TestOAuth2MintTokenFlow::REMOTE_CONSENT_SUCCESS);
    func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
    func->set_remote_consent_gaia_id(secondary_account.gaia);
    // Have GetAuthTokenFunction actually make the request for the access token.
    func->set_auto_login_access_token(false);

    base::RunLoop run_loop;
    on_access_token_requested_ = run_loop.QuitClosure();
    RunFunctionAsync(func.get(), "[{\"interactive\": true}]");
    run_loop.Run();

    // The first request will be for the primary account and the second one for
    // the account that has been returned in result of the remote consent.
    std::string primary_account_access_token =
        IssueLoginAccessTokenForAccount(primary_account_id);

    if (id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
      EXPECT_EQ(std::string(errors::kUserNonPrimary), WaitForError(func.get()));
      histogram_tester()->ExpectUniqueSample(
          kGetAuthTokenResultHistogramName,
          IdentityGetAuthTokenError::State::kRemoteConsentUserNonPrimary, 1);
      histogram_tester()->ExpectUniqueSample(
          kGetAuthTokenResultAfterConsentApprovedHistogramName,
          IdentityGetAuthTokenError::State::kRemoteConsentUserNonPrimary, 1);
      return;
    }

    std::string secondary_account_access_token =
        IssueLoginAccessTokenForAccount(secondary_account.account_id);

    std::string access_token;
    std::set<std::string> granted_scopes;
    WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
    EXPECT_EQ(std::string(kAccessToken), access_token);
    EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

    EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
              GetCachedToken(secondary_account).status());
    EXPECT_EQ(secondary_account.gaia,
              id_api()->GetGaiaIdForExtension(extension->id()));
    EXPECT_THAT(func->login_access_tokens(),
                testing::ElementsAre(primary_account_access_token,
                                     secondary_account_access_token));
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kNone, 1);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultAfterConsentApprovedHistogramName,
        IdentityGetAuthTokenError::State::kNone, 1);
  }

  {
    // Check that the next function call returns a token for the same account
    // from the cache.
    scoped_refptr<FakeGetAuthTokenFunction> func(
        new FakeGetAuthTokenFunction());
    func->set_extension(extension);

    std::string access_token;
    std::set<std::string> granted_scopes;
    RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                            &granted_scopes);
    EXPECT_EQ(std::string(kAccessToken), access_token);
    EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
    EXPECT_FALSE(func->login_ui_shown());
    EXPECT_FALSE(func->scope_ui_shown());
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kNone, 2);
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultAfterConsentApprovedHistogramName,
        IdentityGetAuthTokenError::State::kNone, 1);
  }
}

// Tests two concurrent remote consent flows. Both of them should succeed.
// The second flow starts while the first one is blocked on an interactive mint
// token flow. This is a regression test for https://crbug.com/1091423.
IN_PROC_BROWSER_TEST_F(
    GetAuthTokenFunctionTest,
    RemoteConsentMultipleActiveRequests_BlockedOnInteractive) {
  SignIn("primary@example.com");
  CoreAccountInfo account = GetPrimaryAccountInfo();
  const extensions::Extension* extension = CreateExtension(CLIENT_ID | SCOPES);

  scoped_refptr<FakeGetAuthTokenFunction> func1(new FakeGetAuthTokenFunction());
  func1->set_extension(extension);
  func1->push_mint_token_result(
      TestOAuth2MintTokenFlow::REMOTE_CONSENT_SUCCESS);
  func1->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func1->set_remote_consent_gaia_id(account.gaia);
  base::RunLoop scope_ui_shown_loop;
  func1->set_scope_ui_async(scope_ui_shown_loop.QuitClosure());

  scoped_refptr<FakeGetAuthTokenFunction> func2(new FakeGetAuthTokenFunction());
  func2->set_extension(extension);
  func2->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func2->set_remote_consent_gaia_id(account.gaia);

  AsyncFunctionRunner func1_runner;
  func1_runner.RunFunctionAsync(func1.get(), "[{\"interactive\": true}]",
                                browser()->profile());

  AsyncFunctionRunner func2_runner;
  func2_runner.RunFunctionAsync(func2.get(), "[{\"interactive\": true}]",
                                browser()->profile());

  // Allows func2 to put a task in the queue.
  base::RunLoop().RunUntilIdle();

  scope_ui_shown_loop.Run();
  func1->CompleteRemoteConsentDialog();

  std::string access_token1;
  std::set<std::string> granted_scopes1;
  WaitForGetAuthTokenResults(func1.get(), &access_token1, &granted_scopes1,
                             &func1_runner);
  EXPECT_EQ(std::string(kAccessToken), access_token1);
  EXPECT_EQ(func1->GetExtensionTokenKeyForTest()->scopes, granted_scopes1);

  std::string access_token2;
  std::set<std::string> granted_scopes2;
  WaitForGetAuthTokenResults(func2.get(), &access_token2, &granted_scopes2,
                             &func2_runner);
  EXPECT_EQ(std::string(kAccessToken), access_token2);
  EXPECT_EQ(func2->GetExtensionTokenKeyForTest()->scopes, granted_scopes2);

  // Only one consent ui should be shown.
  int total_scope_ui_shown = func1->scope_ui_shown() + func2->scope_ui_shown();
  EXPECT_EQ(1, total_scope_ui_shown);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(account).status());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      2);
}

// Tests two concurrent remote consent flows. Both of them should succeed.
// The second flow starts while the first one is blocked on a non-interactive
// mint token flow. This is a regression test for https://crbug.com/1091423.
IN_PROC_BROWSER_TEST_F(
    GetAuthTokenFunctionTest,
    RemoteConsentMultipleActiveRequests_BlockedOnNoninteractive) {
  SignIn("primary@example.com");
  CoreAccountInfo account = GetPrimaryAccountInfo();
  const extensions::Extension* extension = CreateExtension(CLIENT_ID | SCOPES);

  scoped_refptr<FakeGetAuthTokenFunction> func1(new FakeGetAuthTokenFunction());
  func1->set_extension(extension);
  func1->push_mint_token_result(
      TestOAuth2MintTokenFlow::REMOTE_CONSENT_SUCCESS);
  func1->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func1->set_remote_consent_gaia_id(account.gaia);
  func1->set_auto_login_access_token(false);

  scoped_refptr<FakeGetAuthTokenFunction> func2(new FakeGetAuthTokenFunction());
  func2->set_extension(extension);
  func2->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);
  func2->set_remote_consent_gaia_id(account.gaia);
  base::RunLoop scope_ui_shown_loop;
  func2->set_scope_ui_async(scope_ui_shown_loop.QuitClosure());

  base::RunLoop access_token_run_loop;
  on_access_token_requested_ = access_token_run_loop.QuitClosure();
  AsyncFunctionRunner func1_runner;
  func1_runner.RunFunctionAsync(func1.get(), "[{\"interactive\": true}]",
                                browser()->profile());

  AsyncFunctionRunner func2_runner;
  func2_runner.RunFunctionAsync(func2.get(), "[{\"interactive\": true}]",
                                browser()->profile());

  // Allows func2 to put a task in the queue.
  base::RunLoop().RunUntilIdle();

  access_token_run_loop.Run();
  // Let subsequent requests pass automatically.
  func1->set_auto_login_access_token(true);
  IssueLoginAccessTokenForAccount(account.account_id);

  scope_ui_shown_loop.Run();
  func2->CompleteRemoteConsentDialog();

  std::string access_token1;
  std::set<std::string> granted_scopes1;
  WaitForGetAuthTokenResults(func1.get(), &access_token1, &granted_scopes1,
                             &func1_runner);
  EXPECT_EQ(std::string(kAccessToken), access_token1);
  EXPECT_EQ(func1->GetExtensionTokenKeyForTest()->scopes, granted_scopes1);

  std::string access_token2;
  std::set<std::string> granted_scopes2;
  WaitForGetAuthTokenResults(func2.get(), &access_token2, &granted_scopes2,
                             &func2_runner);
  EXPECT_EQ(std::string(kAccessToken), access_token2);
  EXPECT_EQ(func2->GetExtensionTokenKeyForTest()->scopes, granted_scopes2);

  // Only one consent ui should be shown.
  int total_scope_ui_shown = func1->scope_ui_shown() + func2->scope_ui_shown();
  EXPECT_EQ(1, total_scope_ui_shown);

  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(account).status());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      2);
}

// The signin flow is simply not used on ChromeOS.
#if !defined(OS_CHROMEOS)
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest,
                       MultiSecondaryInteractiveInvalidToken) {
  // Setup a secondary account with no valid refresh token, and try to get a
  // auth token for it.
  SignIn("primary@example.com");
  AccountInfo secondary_account =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");
  identity_test_env()->SetInvalidRefreshTokenForAccount(
      secondary_account.account_id);

  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(extension.get());
  func->set_login_ui_result(true);
  func->push_mint_token_result(TestOAuth2MintTokenFlow::ISSUE_ADVICE_SUCCESS);

  const char kFunctionParams[] =
      "[{\"account\": { \"id\": \"gaia_id_for_secondary@example.com\" }, "
      "\"interactive\": true}]";

  if (id_api()->AreExtensionsRestrictedToPrimaryAccount()) {
    // Fail if extensions are restricted to the primary account.
    std::string error = utils::RunFunctionAndReturnError(
        func.get(), kFunctionParams, browser());
    EXPECT_EQ(std::string(errors::kUserNonPrimary), error);
    EXPECT_FALSE(func->login_ui_shown());
    EXPECT_FALSE(func->scope_ui_shown());
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kUserNonPrimary, 1);
  } else {
    // Extensions can show the login UI for secondary accounts, and get the auth
    // token.
    std::string access_token;
    std::set<std::string> granted_scopes;
    RunGetAuthTokenFunction(func.get(), kFunctionParams, browser(),
                            &access_token, &granted_scopes);
    EXPECT_EQ(std::string(kAccessToken), access_token);
    EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
    EXPECT_TRUE(func->login_ui_shown());
    EXPECT_TRUE(func->scope_ui_shown());
    histogram_tester()->ExpectUniqueSample(
        kGetAuthTokenResultHistogramName,
        IdentityGetAuthTokenError::State::kNone, 1);
  }
}
#endif

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ScopesDefault) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                          &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);

  const ExtensionTokenKey* token_key = func->GetExtensionTokenKeyForTest();
  EXPECT_EQ(token_key->scopes, granted_scopes);
  EXPECT_EQ(2ul, token_key->scopes.size());
  EXPECT_TRUE(base::Contains(token_key->scopes, "scope1"));
  EXPECT_TRUE(base::Contains(token_key->scopes, "scope2"));
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ScopesEmpty) {
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::string error(utils::RunFunctionAndReturnError(
      func.get(), "[{\"scopes\": []}]", browser()));

  EXPECT_EQ(errors::kInvalidScopes, error);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kEmptyScopes, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ScopesEmail) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::set<std::string> scopes = {"email"};
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS,
                               scopes);
  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"scopes\": [\"email\"]}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);

  const ExtensionTokenKey* token_key = func->GetExtensionTokenKeyForTest();
  EXPECT_EQ(token_key->scopes, granted_scopes);
  EXPECT_EQ(scopes, token_key->scopes);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, ScopesEmailFooBar) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::set<std::string> scopes = {"email", "foo", "bar"};
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS,
                               scopes);
  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(),
                          "[{\"scopes\": [\"email\", \"foo\", \"bar\"]}]",
                          browser(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);

  const ExtensionTokenKey* token_key = func->GetExtensionTokenKeyForTest();
  EXPECT_EQ(token_key->scopes, granted_scopes);
  EXPECT_EQ(scopes, token_key->scopes);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Ensure that the returned scopes from the function is the cached scopes and
// not the requested scopes.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, SubsetMatchCacheHit) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::set<std::string> scopes = {"email", "foo", "bar"};
  IdentityTokenCacheValue token = IdentityTokenCacheValue::CreateToken(
      kAccessToken, scopes, base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"scopes\": [\"email\", \"foo\"]}]",
                          browser(), &access_token, &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(scopes, granted_scopes);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Ensure that the newly cached token uses the granted scopes and not the
// requested scopes.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, SubsetMatchCachePopulate) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::set<std::string> scopes = {"foo", "bar"};
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS,
                               scopes);
  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{\"scopes\": [\"email\", \"foo\"]}]",
                          browser(), &access_token, &granted_scopes);

  const IdentityTokenCacheValue& token =
      GetCachedToken(CoreAccountInfo(), scopes);
  EXPECT_EQ(std::string(kAccessToken), token.token());
  EXPECT_EQ(scopes, token.granted_scopes());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN, token.status());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Ensure that the scopes returned by the function reflects the granted scopes
// and not the requested scopes.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionTest, GranularPermissionsResponse) {
  SignIn("primary@example.com");
  auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());

  std::set<std::string> scopes = {"email", "foobar"};
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS,
                               scopes);
  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(),
                          "[{\"enableGranularPermissions\": true,"
                          "\"scopes\": [\"email\", \"bar\"]}]",
                          browser(), &access_token, &granted_scopes);
  EXPECT_EQ(kAccessToken, access_token);
  EXPECT_EQ(scopes, granted_scopes);

  EXPECT_TRUE(func->enable_granular_permissions());
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

#if defined(OS_CHROMEOS)
class GetAuthTokenFunctionPublicSessionTest : public GetAuthTokenFunctionTest {
 public:
  GetAuthTokenFunctionPublicSessionTest()
      : user_manager_(new chromeos::MockUserManager) {}

 protected:
  void SetUpInProcessBrowserTestFixture() override {
    GetAuthTokenFunctionTest::SetUpInProcessBrowserTestFixture();

    // Set up the user manager to fake a public session.
    EXPECT_CALL(*user_manager_, IsLoggedInAsKioskApp())
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*user_manager_, IsLoggedInAsPublicAccount())
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*user_manager_, GetLoggedInUsers())
        .WillRepeatedly(testing::Invoke(user_manager_,
                                        &chromeos::MockUserManager::GetUsers));
  }

  scoped_refptr<const Extension> CreateTestExtension(const std::string& id) {
    return ExtensionBuilder("Test")
        .SetManifestKey(
            "oauth2", DictionaryBuilder()
                          .Set("client_id", "clientId")
                          .Set("scopes", ListBuilder().Append("scope1").Build())
                          .Build())
        .SetID(id)
        .Build();
  }

  // Set up fake install attributes to make the device appeared as
  // enterprise-managed.
  chromeos::ScopedStubInstallAttributes test_install_attributes_{
      chromeos::StubInstallAttributes::CreateCloudManaged("example.com",
                                                          "fake-id")};

  // Owned by |user_manager_enabler|.
  chromeos::MockUserManager* user_manager_;
};

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionPublicSessionTest, NonAllowlisted) {
  // GetAuthToken() should return UserNotSignedIn in public sessions for
  // non-allowlisted extensions.
  user_manager::ScopedUserManager user_manager_enabler(
      base::WrapUnique(user_manager_));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateTestExtension("test-id"));
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[]", browser());
  EXPECT_EQ(std::string(errors::kUserNotSignedIn), error);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kNotAllowlistedInPublicSession, 1);
}

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionPublicSessionTest, Allowlisted) {
  // GetAuthToken() should return a token for allowlisted extensions.
  user_manager::ScopedUserManager user_manager_enabler(
      base::WrapUnique(user_manager_));
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateTestExtension("ljacajndfccfgnfohlgkdphmbnpkjflk"));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{}]", browser(), &access_token,
                          &granted_scopes);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

#endif

// There are two parameters, which are stored in a std::pair, for these tests.
//
// std::string: the GetAuthToken arguments
// bool: the expected value of GetAuthToken's enable_granular_permissions
class GetAuthTokenFunctionEnableGranularPermissionsTest
    : public GetAuthTokenFunctionTest,
      public testing::WithParamInterface<std::pair<std::string, bool>> {};

// Provided with the arguments for GetAuthToken, ensures that GetAuthToken's
// enable_granular_permissions is some expected value when the
// 'ReturnScopesInGetAuthToken' feature flag is enabled.
IN_PROC_BROWSER_TEST_P(GetAuthTokenFunctionEnableGranularPermissionsTest,
                       EnableGranularPermissions) {
  const std::string& args = GetParam().first;
  bool expected_enable_granular_permissions = GetParam().second;

  SignIn("primary@example.com");
  auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  std::set<std::string> granted_scopes;
  RunGetAuthTokenFunction(func.get(), "[{" + args + "}]", browser(),
                          &access_token, &granted_scopes);
  EXPECT_EQ(kAccessToken, access_token);
  EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);

  EXPECT_EQ(expected_enable_granular_permissions,
            func->enable_granular_permissions());
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

INSTANTIATE_TEST_SUITE_P(
    All,
    GetAuthTokenFunctionEnableGranularPermissionsTest,
    testing::Values(std::make_pair("\"enableGranularPermissions\": true", true),
                    std::make_pair("\"enableGranularPermissions\": false",
                                   false),
                    std::make_pair("", false)));

class GetAuthTokenFunctionReturnScopesDisabledTest
    : public GetAuthTokenFunctionTest {
 public:
  GetAuthTokenFunctionReturnScopesDisabledTest()
      : GetAuthTokenFunctionTest(false) {}

  void RunGetAuthTokenFunctionReturnScopesDisabled(ExtensionFunction* function,
                                                   const std::string& args,
                                                   Browser* browser,
                                                   std::string* access_token) {
    EXPECT_TRUE(
        utils::RunFunction(function, args, browser, api_test_utils::NONE));

    EXPECT_TRUE(function->GetError().empty())
        << "Unexpected error: " << function->GetError();
    EXPECT_NE(nullptr, function->GetResultList());

    const auto& result_list = function->GetResultList()->GetList();
    EXPECT_EQ(1ul, result_list.size());

    const auto& access_token_value = result_list[0];
    EXPECT_TRUE(access_token_value.is_string());

    *access_token = access_token_value.GetString();
  }
};

IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionReturnScopesDisabledTest,
                       NoOptionsSuccess) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  scoped_refptr<const Extension> extension(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  RunGetAuthTokenFunctionReturnScopesDisabled(func.get(), "[{}]", browser(),
                                              &access_token);
  EXPECT_EQ(std::string(kAccessToken), access_token);
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken(CoreAccountInfo()).status());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Whether or not returning scopes is enabled should not affect error handling.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionReturnScopesDisabledTest,
                       NonInteractiveMintFailure) {
  SignIn("primary@example.com");
  scoped_refptr<FakeGetAuthTokenFunction> func(new FakeGetAuthTokenFunction());
  func->set_extension(CreateExtension(CLIENT_ID | SCOPES));
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_FAILURE);
  std::string error =
      utils::RunFunctionAndReturnError(func.get(), "[{}]", browser());
  EXPECT_TRUE(base::StartsWith(error, errors::kAuthFailure,
                               base::CompareCase::INSENSITIVE_ASCII));
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName,
      IdentityGetAuthTokenError::State::kMintTokenAuthFailure, 1);
}

// There are two parameters, which are stored in a std::pair, for these tests.
//
// std::string: the GetAuthToken arguments
// bool: the expected value of GetAuthToken's enable_granular_permissions
class GetAuthTokenFunctionReturnScopesDisabledEnableGranularPermissionsTest
    : public GetAuthTokenFunctionReturnScopesDisabledTest,
      public testing::WithParamInterface<std::pair<std::string, bool>> {};

// Provided with the arguments for GetAuthToken, ensures that GetAuthToken's
// enable_granular_permissions is some expected value when the
// 'ReturnScopesInGetAuthToken' feature flag is disabled.
IN_PROC_BROWSER_TEST_P(
    GetAuthTokenFunctionReturnScopesDisabledEnableGranularPermissionsTest,
    EnableGranularPermissions) {
  const std::string& args = GetParam().first;
  bool expected_enable_granular_permissions = GetParam().second;

  SignIn("primary@example.com");
  auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  func->set_extension(extension.get());
  func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

  std::string access_token;
  RunGetAuthTokenFunctionReturnScopesDisabled(func.get(), "[{" + args + "}]",
                                              browser(), &access_token);
  EXPECT_EQ(kAccessToken, access_token);

  EXPECT_EQ(expected_enable_granular_permissions,
            func->enable_granular_permissions());
  EXPECT_FALSE(func->login_ui_shown());
  EXPECT_FALSE(func->scope_ui_shown());
  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

INSTANTIATE_TEST_SUITE_P(
    All,
    GetAuthTokenFunctionReturnScopesDisabledEnableGranularPermissionsTest,
    testing::Values(
        std::make_pair("\"enableGranularPermissions\": true", false),
        std::make_pair("\"enableGranularPermissions\": false", false),
        std::make_pair("", false)));

class RemoveCachedAuthTokenFunctionTest : public ExtensionBrowserTest {
 protected:
  bool InvalidateDefaultToken() {
    scoped_refptr<IdentityRemoveCachedAuthTokenFunction> func(
        new IdentityRemoveCachedAuthTokenFunction);
    func->set_extension(
        ExtensionBuilder("Test").SetID(kExtensionId).Build().get());
    return utils::RunFunction(
        func.get(), std::string("[{\"token\": \"") + kAccessToken + "\"}]",
        browser(), api_test_utils::NONE);
  }

  IdentityAPI* id_api() {
    return IdentityAPI::GetFactoryInstance()->Get(browser()->profile());
  }

  IdentityTokenCacheValue CreateToken(const std::string& token,
                                      base::TimeDelta time_to_live) {
    return IdentityTokenCacheValue::CreateToken(
        token, std::set<std::string>({"foo"}), time_to_live);
  }

  void SetCachedToken(const IdentityTokenCacheValue& token_data) {
    CoreAccountInfo account_info;
    account_info.account_id = CoreAccountId("test@example.com");
    account_info.gaia = "test_gaia";
    account_info.email = "test@example.com";
    ExtensionTokenKey key(kExtensionId, account_info,
                          std::set<std::string>({"foo"}));
    id_api()->token_cache()->SetToken(key, token_data);
  }

  const IdentityTokenCacheValue& GetCachedToken() {
    CoreAccountInfo account_info;
    account_info.account_id = CoreAccountId("test@example.com");
    account_info.gaia = "test_gaia";
    account_info.email = "test@example.com";
    ExtensionTokenKey key(kExtensionId, account_info,
                          std::set<std::string>({"foo"}));
    return id_api()->token_cache()->GetToken(key);
  }
};

class GetAuthTokenFunctionSelectedUserIdTest : public GetAuthTokenFunctionTest {
 public:
  explicit GetAuthTokenFunctionSelectedUserIdTest(
      bool is_selected_user_id_enabled = true)
      : GetAuthTokenFunctionTest(true, is_selected_user_id_enabled) {}

  // Executes a new function and checks that the selected_user_id is the
  // expected value. The interactive and scopes field are predefined.
  // The account id specified by the extension is optional.
  void RunNewFunctionAndExpectSelectedUserId(
      const scoped_refptr<const extensions::Extension>& extension,
      const std::string& expected_selected_user_id,
      const base::Optional<std::string> requested_account = base::nullopt) {
    auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
    func->set_extension(extension);
    RunFunctionAndExpectSelectedUserId(func, expected_selected_user_id,
                                       requested_account);
  }

  void RunFunctionAndExpectSelectedUserId(
      const scoped_refptr<FakeGetAuthTokenFunction>& func,
      const std::string& expected_selected_user_id,
      const base::Optional<std::string> requested_account = base::nullopt) {
    // Stops the function right before selected_user_id would be used.
    MockQueuedMintRequest queued_request;
    IdentityMintRequestQueue::MintType type =
        IdentityMintRequestQueue::MINT_TYPE_INTERACTIVE;
    EXPECT_CALL(queued_request, StartMintToken(type)).Times(1);
    QueueRequestStart(type, &queued_request);

    func->push_mint_token_result(TestOAuth2MintTokenFlow::MINT_TOKEN_SUCCESS);

    std::string requested_account_arg =
        requested_account.has_value()
            ? ", \"account\": {\"id\": \"" + requested_account.value() + "\"}"
            : "";
    RunFunctionAsync(func.get(),
                     "[{\"interactive\": true" + requested_account_arg + "}]");
    base::RunLoop().RunUntilIdle();

    EXPECT_EQ(expected_selected_user_id, func->GetSelectedUserId());

    // Resume the function
    QueueRequestComplete(type, &queued_request);

    // Complete function and do some basic checks.
    std::string access_token;
    std::set<std::string> granted_scopes;
    WaitForGetAuthTokenResults(func.get(), &access_token, &granted_scopes);
    EXPECT_EQ(kAccessToken, access_token);
    EXPECT_EQ(func->GetExtensionTokenKeyForTest()->scopes, granted_scopes);
  }
};

// Tests that Chrome uses the correct selected user id value when a gaia id was
// cached and only the primary account is signed in.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdTest, SingleAccount) {
  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");
  CoreAccountInfo primary_account = GetPrimaryAccountInfo();

  SetCachedGaiaId(primary_account.gaia);
  RunNewFunctionAndExpectSelectedUserId(extension, primary_account.gaia);

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Tests that Chrome uses the correct selected user id value when a gaia id was
// cached for a secondary account.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdTest,
                       MultipleAccounts) {
  // This test requires the use of a secondary account. If extensions are
  // restricted to primary account only, this test wouldn't make too much sense.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");
  AccountInfo secondary_account =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");

  SetCachedGaiaId(secondary_account.gaia);
  RunNewFunctionAndExpectSelectedUserId(extension, secondary_account.gaia);

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Tests that Chrome uses the correct selected user id value when a gaia id was
// cached but the extension specifies an account id for a different available
// account.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdTest,
                       RequestedAccountAvailable) {
  // This test requires the use of a secondary account. If extensions are
  // restricted to primary account only, this test wouldn't make too much sense.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");
  CoreAccountInfo primary_account = GetPrimaryAccountInfo();
  AccountInfo secondary_account =
      identity_test_env()->MakeAccountAvailable("secondary@example.com");

  SetCachedGaiaId(primary_account.gaia);
  // Run a new function with an account id specified in the arguments.
  RunNewFunctionAndExpectSelectedUserId(extension, secondary_account.gaia,
                                        secondary_account.gaia);

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// The signin flow is not used on ChromeOS.
#if !defined(OS_CHROMEOS)
// Tests that Chrome does not have any selected user id value if the account
// specified by the extension is not available.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdTest,
                       RequestedAccountUnavailable) {
  // This test requires the use of a secondary account. If extensions are
  // restricted to primary account only, this test wouldn't make too much sense.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");

  // Run a new function with an account id specified. Since this account is not
  // signed in, the login screen will be shown.
  auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
  func->set_extension(extension);
  func->set_login_ui_result(true);
  RunFunctionAndExpectSelectedUserId(func, "",
                                     "gaia_id_for_unavailable_example.com");
  // The login ui still showed but another account was logged in instead.
  EXPECT_TRUE(func->login_ui_shown());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

// Tests that Chrome uses the correct selected user id value after logging into
// the account requested by the extension.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdTest,
                       RequestedAccountLogin) {
  // This test requires the use of a secondary account. If extensions are
  // restricted to primary account only, this test wouldn't make too much sense.
  if (id_api()->AreExtensionsRestrictedToPrimaryAccount())
    return;

  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");

  // Run a new function with an account id specified. Since this account is not
  // signed in, the login screen will be shown.
  auto func = base::MakeRefCounted<FakeGetAuthTokenFunction>();
  func->set_extension(extension);
  func->set_login_ui_result(true);
  RunFunctionAndExpectSelectedUserId(func, "gaia_id_for_secondary_example.com",
                                     "gaia_id_for_secondary_example.com");
  EXPECT_TRUE(func->login_ui_shown());

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}
#endif

class GetAuthTokenFunctionSelectedUserIdDisabledTest
    : public GetAuthTokenFunctionSelectedUserIdTest {
 public:
  GetAuthTokenFunctionSelectedUserIdDisabledTest()
      : GetAuthTokenFunctionSelectedUserIdTest(false) {}
};

// Tests that Chrome does not use any selected user id value if the
// 'SelectedUserIdInGetAuthToken' flag is disabled.
IN_PROC_BROWSER_TEST_F(GetAuthTokenFunctionSelectedUserIdDisabledTest,
                       SingleAccount) {
  auto extension = base::WrapRefCounted(CreateExtension(CLIENT_ID | SCOPES));
  SignIn("primary@example.com");
  CoreAccountInfo primary_account = GetPrimaryAccountInfo();

  SetCachedGaiaId(primary_account.gaia);
  RunNewFunctionAndExpectSelectedUserId(extension, "");

  histogram_tester()->ExpectUniqueSample(
      kGetAuthTokenResultHistogramName, IdentityGetAuthTokenError::State::kNone,
      1);
}

IN_PROC_BROWSER_TEST_F(RemoveCachedAuthTokenFunctionTest, NotFound) {
  EXPECT_TRUE(InvalidateDefaultToken());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_NOTFOUND,
            GetCachedToken().status());
}

IN_PROC_BROWSER_TEST_F(RemoveCachedAuthTokenFunctionTest, Advice) {
  IssueAdviceInfo info;
  IdentityTokenCacheValue advice =
      IdentityTokenCacheValue::CreateIssueAdvice(info);
  SetCachedToken(advice);
  EXPECT_TRUE(InvalidateDefaultToken());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_ADVICE,
            GetCachedToken().status());
}

IN_PROC_BROWSER_TEST_F(RemoveCachedAuthTokenFunctionTest, NonMatchingToken) {
  IdentityTokenCacheValue token =
      CreateToken("non_matching_token", base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);
  EXPECT_TRUE(InvalidateDefaultToken());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken().status());
  EXPECT_EQ("non_matching_token", GetCachedToken().token());
}

IN_PROC_BROWSER_TEST_F(RemoveCachedAuthTokenFunctionTest, MatchingToken) {
  IdentityTokenCacheValue token =
      CreateToken(kAccessToken, base::TimeDelta::FromSeconds(3600));
  SetCachedToken(token);
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_TOKEN,
            GetCachedToken().status());
  EXPECT_TRUE(InvalidateDefaultToken());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_NOTFOUND,
            GetCachedToken().status());
}

class LaunchWebAuthFlowFunctionTest : public AsyncExtensionBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    AsyncExtensionBrowserTest::SetUpCommandLine(command_line);
    // Reduce performance test variance by disabling background networking.
    command_line->AppendSwitch(switches::kDisableBackgroundNetworking);
  }
};

#if defined(OS_LINUX) || defined(OS_CHROMEOS)
// This test times out on Linux MSan Tests.
// See https://crbug.com/831848 .
#define MAYBE_UserCloseWindow DISABLED_UserCloseWindow
#else
#define MAYBE_UserCloseWindow UserCloseWindow
#endif
IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest, MAYBE_UserCloseWindow) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory(
      "chrome/test/data/extensions/api_test/identity");
  ASSERT_TRUE(https_server.Start());
  GURL auth_url(https_server.GetURL("/interaction_required.html"));

  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  WaitForGURLAndCloseWindow popup_observer(auth_url);

  std::string args =
      "[{\"interactive\": true, \"url\": \"" + auth_url.spec() + "\"}]";
  RunFunctionAsync(function.get(), args);

  popup_observer.Wait();
  popup_observer.CloseEmbedderWebContents();

  EXPECT_EQ(std::string(errors::kUserRejected), WaitForError(function.get()));
}

IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest, InteractionRequired) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory(
      "chrome/test/data/extensions/api_test/identity");
  ASSERT_TRUE(https_server.Start());
  GURL auth_url(https_server.GetURL("/interaction_required.html"));

  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  std::string args =
      "[{\"interactive\": false, \"url\": \"" + auth_url.spec() + "\"}]";
  std::string error =
      utils::RunFunctionAndReturnError(function.get(), args, browser());

  EXPECT_EQ(std::string(errors::kInteractionRequired), error);
}

IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest, LoadFailed) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory(
      "chrome/test/data/extensions/api_test/identity");
  ASSERT_TRUE(https_server.Start());
  GURL auth_url(https_server.GetURL("/five_hundred.html"));

  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  std::string args =
      "[{\"interactive\": true, \"url\": \"" + auth_url.spec() + "\"}]";
  std::string error =
      utils::RunFunctionAndReturnError(function.get(), args, browser());

  EXPECT_EQ(std::string(errors::kPageLoadFailure), error);
}

IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest, NonInteractiveSuccess) {
  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  function->InitFinalRedirectURLPrefixForTest("abcdefghij");
  std::unique_ptr<base::Value> value(utils::RunFunctionAndReturnSingleResult(
      function.get(),
      "[{\"interactive\": false,"
      "\"url\": \"https://abcdefghij.ch40m1umapp.qjz9zk/callback#test\"}]",
      browser()));

  std::string url;
  EXPECT_TRUE(value->GetAsString(&url));
  EXPECT_EQ(std::string("https://abcdefghij.ch40m1umapp.qjz9zk/callback#test"),
            url);
}

IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest,
                       InteractiveFirstNavigationSuccess) {
  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  function->InitFinalRedirectURLPrefixForTest("abcdefghij");
  std::unique_ptr<base::Value> value(utils::RunFunctionAndReturnSingleResult(
      function.get(),
      "[{\"interactive\": true,"
      "\"url\": \"https://abcdefghij.ch40m1umapp.qjz9zk/callback#test\"}]",
      browser()));

  std::string url;
  EXPECT_TRUE(value->GetAsString(&url));
  EXPECT_EQ(std::string("https://abcdefghij.ch40m1umapp.qjz9zk/callback#test"),
            url);
}

IN_PROC_BROWSER_TEST_F(LaunchWebAuthFlowFunctionTest,
                       InteractiveSecondNavigationSuccess) {
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory(
      "chrome/test/data/extensions/api_test/identity");
  ASSERT_TRUE(https_server.Start());
  GURL auth_url(https_server.GetURL("/redirect_to_chromiumapp.html"));

  scoped_refptr<IdentityLaunchWebAuthFlowFunction> function(
      new IdentityLaunchWebAuthFlowFunction());
  scoped_refptr<const Extension> empty_extension(
      ExtensionBuilder("Test").Build());
  function->set_extension(empty_extension.get());

  function->InitFinalRedirectURLPrefixForTest("abcdefghij");
  std::string args =
      "[{\"interactive\": true, \"url\": \"" + auth_url.spec() + "\"}]";
  std::unique_ptr<base::Value> value(
      utils::RunFunctionAndReturnSingleResult(function.get(), args, browser()));

  std::string url;
  EXPECT_TRUE(value->GetAsString(&url));
  EXPECT_EQ(std::string("https://abcdefghij.ch40m1umapp.qjz9zk/callback#test"),
            url);
}

class ClearAllCachedAuthTokensFunctionTest : public AsyncExtensionBrowserTest {
 public:
  void SetUpOnMainThread() override {
    AsyncExtensionBrowserTest::SetUpOnMainThread();
    base::FilePath manifest_path =
        test_data_dir_.AppendASCII("platform_apps/oauth2");
    extension_ = LoadExtension(manifest_path);
  }

  const Extension* extension() { return extension_; }

  bool RunClearAllCachedAuthTokensFunction() {
    auto function =
        base::MakeRefCounted<IdentityClearAllCachedAuthTokensFunction>();
    function->set_extension(extension_);
    return utils::RunFunction(function.get(), "[]", browser(),
                              api_test_utils::NONE);
  }

  IdentityAPI* id_api() {
    return IdentityAPI::GetFactoryInstance()->Get(browser()->profile());
  }

 private:
  base::test::ScopedFeatureList feature_list_;
  const Extension* extension_ = nullptr;
};

IN_PROC_BROWSER_TEST_F(ClearAllCachedAuthTokensFunctionTest,
                       EraseCachedGaiaId) {
  id_api()->SetGaiaIdForExtension(extension()->id(), "test_gaia");
  EXPECT_EQ("test_gaia", id_api()->GetGaiaIdForExtension(extension()->id()));
  ASSERT_TRUE(RunClearAllCachedAuthTokensFunction());
  EXPECT_FALSE(id_api()->GetGaiaIdForExtension(extension()->id()).has_value());
}

IN_PROC_BROWSER_TEST_F(ClearAllCachedAuthTokensFunctionTest,
                       EraseCachedTokens) {
  ExtensionTokenKey token_key(extension()->id(), CoreAccountInfo(), {"foo"});
  id_api()->token_cache()->SetToken(
      token_key,
      IdentityTokenCacheValue::CreateToken("access_token", {"foo"},
                                           base::TimeDelta::FromSeconds(3600)));
  EXPECT_NE(IdentityTokenCacheValue::CACHE_STATUS_NOTFOUND,
            id_api()->token_cache()->GetToken(token_key).status());
  ASSERT_TRUE(RunClearAllCachedAuthTokensFunction());
  EXPECT_EQ(IdentityTokenCacheValue::CACHE_STATUS_NOTFOUND,
            id_api()->token_cache()->GetToken(token_key).status());
}

class ClearAllCachedAuthTokensFunctionTestWithPartitionParam
    : public ClearAllCachedAuthTokensFunctionTest,
      public testing::WithParamInterface<WebAuthFlow::Partition> {
 public:
  network::mojom::CookieManager* GetCookieManager() {
    Profile* profile = browser()->profile();
    return content::BrowserContext::GetStoragePartition(
               profile,
               WebAuthFlow::GetWebViewPartitionConfig(GetParam(), profile))
        ->GetCookieManagerForBrowserProcess();
  }

  // Returns the list of cookies in the cookie manager.
  net::CookieList GetCookies() {
    net::CookieList result;
    base::RunLoop get_all_cookies_loop;
    GetCookieManager()->GetAllCookies(base::BindLambdaForTesting(
        [&get_all_cookies_loop, &result](const net::CookieList& cookie_list) {
          result = cookie_list;
          get_all_cookies_loop.Quit();
        }));
    get_all_cookies_loop.Run();
    return result;
  }
};

IN_PROC_BROWSER_TEST_P(ClearAllCachedAuthTokensFunctionTestWithPartitionParam,
                       CleanWebAuthFlowCookies) {
  net::CanonicalCookie test_cookie(
      "test_name", "test_value", "test.com", "/", base::Time(), base::Time(),
      base::Time(), true, false, net::CookieSameSite::NO_RESTRICTION,
      net::COOKIE_PRIORITY_DEFAULT);
  base::RunLoop set_cookie_loop;
  GetCookieManager()->SetCanonicalCookie(
      test_cookie,
      net::cookie_util::SimulatedCookieSource(test_cookie, url::kHttpsScheme),
      net::CookieOptions(),
      net::cookie_util::AdaptCookieAccessResultToBool(
          base::BindLambdaForTesting([&](bool include) {
            set_cookie_loop.Quit();
            EXPECT_TRUE(include);
          })));
  set_cookie_loop.Run();

  EXPECT_FALSE(GetCookies().empty());
  ASSERT_TRUE(RunClearAllCachedAuthTokensFunction());
  EXPECT_TRUE(GetCookies().empty());
}

INSTANTIATE_TEST_SUITE_P(
    All,
    ClearAllCachedAuthTokensFunctionTestWithPartitionParam,
    ::testing::Values(WebAuthFlow::Partition::LAUNCH_WEB_AUTH_FLOW,
                      WebAuthFlow::Partition::GET_AUTH_TOKEN));

class OnSignInChangedEventTest : public IdentityTestWithSignin {
 protected:
  void SetUpOnMainThread() override {
    // TODO(blundell): Ideally we would test fully end-to-end by injecting a
    // JavaScript extension listener and having that listener do the
    // verification, but it's not clear how to set that up.
    id_api()->set_on_signin_changed_callback_for_testing(
        base::BindRepeating(&OnSignInChangedEventTest::OnSignInEventChanged,
                            base::Unretained(this)));

    IdentityTestWithSignin::SetUpOnMainThread();
  }

  IdentityAPI* id_api() {
    return IdentityAPI::GetFactoryInstance()->Get(browser()->profile());
  }

  // Adds an event that is expected to fire. Events are unordered, i.e., when an
  // event fires it will be checked against all of the expected events that have
  // been added. This is because the order of multiple events firing due to the
  // same underlying state change is undefined in the
  // chrome.identity.onSignInEventChanged() API.
  void AddExpectedEvent(std::unique_ptr<base::ListValue> args) {
    expected_events_.insert(
        std::make_unique<Event>(events::IDENTITY_ON_SIGN_IN_CHANGED,
                                api::identity::OnSignInChanged::kEventName,
                                std::move(args), browser()->profile()));
  }

  bool HasExpectedEvent() { return !expected_events_.empty(); }

 private:
  void OnSignInEventChanged(Event* event) {
    ASSERT_TRUE(HasExpectedEvent());

    // Search for |event| in the set of expected events.
    bool found_event = false;
    const auto* event_args = event->event_args.get();
    for (const auto& expected_event : expected_events_) {
      EXPECT_EQ(expected_event->histogram_value, event->histogram_value);
      EXPECT_EQ(expected_event->event_name, event->event_name);

      const auto* expected_event_args = expected_event->event_args.get();
      if (*event_args != *expected_event_args)
        continue;

      expected_events_.erase(expected_event);
      found_event = true;
      break;
    }

    if (!found_event) {
      EXPECT_TRUE(false) << "Received bad event:";

      LOG(INFO) << "Was expecting events with these args:";

      for (const auto& expected_event : expected_events_) {
        LOG(INFO) << *(expected_event->event_args.get());
      }

      LOG(INFO) << "But received event with different args:";
      LOG(INFO) << *event_args;
    }
  }

  std::set<std::unique_ptr<Event>> expected_events_;
};

// Test that an event is fired when the primary account signs in.
IN_PROC_BROWSER_TEST_F(OnSignInChangedEventTest, FireOnPrimaryAccountSignIn) {
  api::identity::AccountInfo account_info;
  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  // Sign in and verify that the callback fires.
  SignIn("primary@example.com");

  EXPECT_FALSE(HasExpectedEvent());
}

#if !defined(OS_CHROMEOS)
// Test that an event is fired when the primary account signs out. Only
// applicable in non-DICE mode, as when DICE is enabled clearing the primary
// account does not result in its refresh token being removed and hence does
// not trigger an event to fire.
IN_PROC_BROWSER_TEST_F(OnSignInChangedEventTest, FireOnPrimaryAccountSignOut) {
  if (AccountConsistencyModeManager::IsDiceEnabledForProfile(profile()))
    return;

  api::identity::AccountInfo account_info;
  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  SignIn("primary@example.com");

  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, false));

  // Sign out and verify that the callback fires.
  identity_test_env()->ClearPrimaryAccount();

  EXPECT_FALSE(HasExpectedEvent());
}
#endif  // !defined(OS_CHROMEOS)

// Test that an event is fired when the primary account has a refresh token
// invalidated.
IN_PROC_BROWSER_TEST_F(OnSignInChangedEventTest,
                       FireOnPrimaryAccountRefreshTokenInvalidated) {
  api::identity::AccountInfo account_info;
  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  CoreAccountId primary_account_id = SignIn("primary@example.com");

  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  // Revoke the refresh token and verify that the callback fires.
  identity_test_env()->SetInvalidRefreshTokenForPrimaryAccount();

  EXPECT_FALSE(HasExpectedEvent());
}

// Test that an event is fired when the primary account has a refresh token
// newly available.
IN_PROC_BROWSER_TEST_F(OnSignInChangedEventTest,
                       FireOnPrimaryAccountRefreshTokenAvailable) {
  api::identity::AccountInfo account_info;
  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  CoreAccountId primary_account_id = SignIn("primary@example.com");

  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));
  identity_test_env()->SetInvalidRefreshTokenForPrimaryAccount();

  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  // Make the primary account available again and check that the callback fires.
  identity_test_env()->SetRefreshTokenForPrimaryAccount();
  EXPECT_FALSE(HasExpectedEvent());
}

// Test that an event is fired for changes to a secondary account.
IN_PROC_BROWSER_TEST_F(OnSignInChangedEventTest, FireForSecondaryAccount) {
  api::identity::AccountInfo account_info;
  account_info.id = "gaia_id_for_primary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));
  SignIn("primary@example.com");

  account_info.id = "gaia_id_for_secondary_example.com";
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, true));

  // Make a secondary account available again and check that the callback fires.
  CoreAccountId secondary_account_id =
      identity_test_env()
          ->MakeAccountAvailable("secondary@example.com")
          .account_id;
  EXPECT_FALSE(HasExpectedEvent());

  // Revoke the secondary account's refresh token and check that the callback
  // fires.
  AddExpectedEvent(api::identity::OnSignInChanged::Create(account_info, false));

  identity_test_env()->RemoveRefreshTokenForAccount(secondary_account_id);
  EXPECT_FALSE(HasExpectedEvent());
}

// Tests the chrome.identity API implemented by custom JS bindings.
IN_PROC_BROWSER_TEST_F(ExtensionApiTest, ChromeIdentityJsBindings) {
  ASSERT_TRUE(RunExtensionTest("identity/js_bindings")) << message_;
}

}  // namespace extensions
