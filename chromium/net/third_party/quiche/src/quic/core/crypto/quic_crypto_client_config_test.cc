// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quiche/src/quic/core/crypto/quic_crypto_client_config.h"

#include <string>

#include "net/third_party/quiche/src/quic/core/crypto/proof_verifier.h"
#include "net/third_party/quiche/src/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "net/third_party/quiche/src/quic/core/quic_utils.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_expect_bug.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_test.h"
#include "net/third_party/quiche/src/quic/test_tools/crypto_test_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/mock_random.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_test_utils.h"
#include "net/third_party/quiche/src/common/platform/api/quiche_string_piece.h"

using testing::StartsWith;

namespace quic {
namespace test {
namespace {

class TestProofVerifyDetails : public ProofVerifyDetails {
  ~TestProofVerifyDetails() override {}

  // ProofVerifyDetails implementation
  ProofVerifyDetails* Clone() const override {
    return new TestProofVerifyDetails;
  }
};

class OneServerIdFilter : public QuicCryptoClientConfig::ServerIdFilter {
 public:
  explicit OneServerIdFilter(const QuicServerId* server_id)
      : server_id_(*server_id) {}

  bool Matches(const QuicServerId& server_id) const override {
    return server_id == server_id_;
  }

 private:
  const QuicServerId server_id_;
};

class AllServerIdsFilter : public QuicCryptoClientConfig::ServerIdFilter {
 public:
  bool Matches(const QuicServerId& /*server_id*/) const override {
    return true;
  }
};

}  // namespace

class QuicCryptoClientConfigTest : public QuicTest {};

TEST_F(QuicCryptoClientConfigTest, CachedState_IsEmpty) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_TRUE(state.IsEmpty());
}

TEST_F(QuicCryptoClientConfigTest, CachedState_IsComplete) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.IsComplete(QuicWallTime::FromUNIXSeconds(0)));
}

TEST_F(QuicCryptoClientConfigTest, CachedState_GenerationCounter) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_EQ(0u, state.generation_counter());
  state.SetProofInvalid();
  EXPECT_EQ(1u, state.generation_counter());
}

TEST_F(QuicCryptoClientConfigTest, CachedState_SetProofVerifyDetails) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_TRUE(state.proof_verify_details() == nullptr);
  ProofVerifyDetails* details = new TestProofVerifyDetails;
  state.SetProofVerifyDetails(details);
  EXPECT_EQ(details, state.proof_verify_details());
}

TEST_F(QuicCryptoClientConfigTest, CachedState_ServerNonce) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_nonce());

  std::string server_nonce = "nonce_1";
  state.add_server_nonce(server_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  EXPECT_FALSE(state.has_server_nonce());

  // Allow the ID to be set multiple times.  It's unusual that this would
  // happen, but not impossible.
  server_nonce = "nonce_2";
  state.add_server_nonce(server_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  server_nonce = "nonce_3";
  state.add_server_nonce(server_nonce);
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  EXPECT_FALSE(state.has_server_nonce());

  // Test FIFO behavior.
  const std::string first_nonce = "first_nonce";
  const std::string second_nonce = "second_nonce";
  state.add_server_nonce(first_nonce);
  state.add_server_nonce(second_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(first_nonce, state.GetNextServerNonce());
  EXPECT_EQ(second_nonce, state.GetNextServerNonce());
}

TEST_F(QuicCryptoClientConfigTest, CachedState_ServerNonceConsumedBeforeSet) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_nonce());
  EXPECT_QUIC_BUG(state.GetNextServerNonce(),
                  "Attempting to consume a server nonce "
                  "that was never designated.");
}

TEST_F(QuicCryptoClientConfigTest, CachedState_InitializeFrom) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig::CachedState other;
  state.set_source_address_token("TOKEN");
  // TODO(rch): Populate other fields of |state|.
  other.InitializeFrom(state);
  EXPECT_EQ(state.server_config(), other.server_config());
  EXPECT_EQ(state.source_address_token(), other.source_address_token());
  EXPECT_EQ(state.certs(), other.certs());
  EXPECT_EQ(1u, other.generation_counter());
  EXPECT_FALSE(state.has_server_nonce());
}

TEST_F(QuicCryptoClientConfigTest, InchoateChlo) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  config.set_user_agent_id("quic-tester");
  config.set_alpn("hq");
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 /* demand_x509_proof= */ true, params, &msg);

  QuicVersionLabel cver;
  EXPECT_THAT(msg.GetVersionLabel(kVER, &cver), IsQuicNoError());
  EXPECT_EQ(CreateQuicVersionLabel(QuicVersionMax()), cver);
  quiche::QuicheStringPiece proof_nonce;
  EXPECT_TRUE(msg.GetStringPiece(kNONP, &proof_nonce));
  EXPECT_EQ(std::string(32, 'r'), proof_nonce);
  quiche::QuicheStringPiece user_agent_id;
  EXPECT_TRUE(msg.GetStringPiece(kUAID, &user_agent_id));
  EXPECT_EQ("quic-tester", user_agent_id);
  quiche::QuicheStringPiece alpn;
  EXPECT_TRUE(msg.GetStringPiece(kALPN, &alpn));
  EXPECT_EQ("hq", alpn);
  EXPECT_EQ(msg.minimum_size(), 1u);
}

TEST_F(QuicCryptoClientConfigTest, InchoateChloIsNotPadded) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  config.set_pad_inchoate_hello(false);
  config.set_user_agent_id("quic-tester");
  config.set_alpn("hq");
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 /* demand_x509_proof= */ true, params, &msg);

  EXPECT_EQ(msg.minimum_size(), 1u);
}

// Make sure AES-GCM is the preferred encryption algorithm if it has hardware
// acceleration.
TEST_F(QuicCryptoClientConfigTest, PreferAesGcm) {
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  if (EVP_has_aes_hardware() == 1) {
    EXPECT_EQ(kAESG, config.aead[0]);
  } else {
    EXPECT_EQ(kCC20, config.aead[0]);
  }
}

TEST_F(QuicCryptoClientConfigTest, InchoateChloSecure) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 /* demand_x509_proof= */ true, params, &msg);

  QuicTag pdmd;
  EXPECT_THAT(msg.GetUint32(kPDMD, &pdmd), IsQuicNoError());
  EXPECT_EQ(kX509, pdmd);
  quiche::QuicheStringPiece scid;
  EXPECT_FALSE(msg.GetStringPiece(kSCID, &scid));
}

TEST_F(QuicCryptoClientConfigTest, InchoateChloSecureWithSCIDNoEXPY) {
  // Test that a config with no EXPY is still valid when a non-zero
  // expiry time is passed in.
  QuicCryptoClientConfig::CachedState state;
  CryptoHandshakeMessage scfg;
  scfg.set_tag(kSCFG);
  scfg.SetStringPiece(kSCID, "12345678");
  std::string details;
  QuicWallTime now = QuicWallTime::FromUNIXSeconds(1);
  QuicWallTime expiry = QuicWallTime::FromUNIXSeconds(2);
  state.SetServerConfig(scfg.GetSerialized().AsStringPiece(), now, expiry,
                        &details);

  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 /* demand_x509_proof= */ true, params, &msg);

  quiche::QuicheStringPiece scid;
  EXPECT_TRUE(msg.GetStringPiece(kSCID, &scid));
  EXPECT_EQ("12345678", scid);
}

TEST_F(QuicCryptoClientConfigTest, InchoateChloSecureWithSCID) {
  QuicCryptoClientConfig::CachedState state;
  CryptoHandshakeMessage scfg;
  scfg.set_tag(kSCFG);
  uint64_t future = 1;
  scfg.SetValue(kEXPY, future);
  scfg.SetStringPiece(kSCID, "12345678");
  std::string details;
  state.SetServerConfig(scfg.GetSerialized().AsStringPiece(),
                        QuicWallTime::FromUNIXSeconds(1),
                        QuicWallTime::FromUNIXSeconds(0), &details);

  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 /* demand_x509_proof= */ true, params, &msg);

  quiche::QuicheStringPiece scid;
  EXPECT_TRUE(msg.GetStringPiece(kSCID, &scid));
  EXPECT_EQ("12345678", scid);
}

TEST_F(QuicCryptoClientConfigTest, FillClientHello) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  QuicConnectionId kConnectionId = TestConnectionId(1234);
  std::string error_details;
  MockRandom rand;
  CryptoHandshakeMessage chlo;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  config.FillClientHello(server_id, kConnectionId, QuicVersionMax(),
                         QuicVersionMax(), &state, QuicWallTime::Zero(), &rand,
                         params, &chlo, &error_details);

  // Verify that the version label has been set correctly in the CHLO.
  QuicVersionLabel cver;
  EXPECT_THAT(chlo.GetVersionLabel(kVER, &cver), IsQuicNoError());
  EXPECT_EQ(CreateQuicVersionLabel(QuicVersionMax()), cver);
}

TEST_F(QuicCryptoClientConfigTest, FillClientHelloNoPadding) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  config.set_pad_full_hello(false);
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> params(
      new QuicCryptoNegotiatedParameters);
  QuicConnectionId kConnectionId = TestConnectionId(1234);
  std::string error_details;
  MockRandom rand;
  CryptoHandshakeMessage chlo;
  QuicServerId server_id("www.9oo91e.qjz9zk", 443, false);
  config.FillClientHello(server_id, kConnectionId, QuicVersionMax(),
                         QuicVersionMax(), &state, QuicWallTime::Zero(), &rand,
                         params, &chlo, &error_details);

  // Verify that the version label has been set correctly in the CHLO.
  QuicVersionLabel cver;
  EXPECT_THAT(chlo.GetVersionLabel(kVER, &cver), IsQuicNoError());
  EXPECT_EQ(CreateQuicVersionLabel(QuicVersionMax()), cver);
  EXPECT_EQ(chlo.minimum_size(), 1u);
}

TEST_F(QuicCryptoClientConfigTest, ProcessServerDowngradeAttack) {
  ParsedQuicVersionVector supported_versions = AllSupportedVersions();
  if (supported_versions.size() == 1) {
    // No downgrade attack is possible if the client only supports one version.
    return;
  }

  ParsedQuicVersionVector supported_version_vector;
  for (size_t i = supported_versions.size(); i > 0; --i) {
    supported_version_vector.push_back(supported_versions[i - 1]);
  }

  CryptoHandshakeMessage msg;
  msg.set_tag(kSHLO);
  msg.SetVersionVector(kVER, supported_version_vector);

  QuicCryptoClientConfig::CachedState cached;
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> out_params(
      new QuicCryptoNegotiatedParameters);
  std::string error;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  EXPECT_THAT(config.ProcessServerHello(
                  msg, EmptyQuicConnectionId(), supported_versions.front(),
                  supported_versions, &cached, out_params, &error),
              IsError(QUIC_VERSION_NEGOTIATION_MISMATCH));
  EXPECT_THAT(error, StartsWith("Downgrade attack detected: ServerVersions"));
}

TEST_F(QuicCryptoClientConfigTest, InitializeFrom) {
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicServerId canonical_server_id("www.9oo91e.qjz9zk", 443, false);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_server_id);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");
  state->SetProofValid();

  QuicServerId other_server_id("mail.9oo91e.qjz9zk", 443, false);
  config.InitializeFrom(other_server_id, canonical_server_id, &config);
  QuicCryptoClientConfig::CachedState* other =
      config.LookupOrCreate(other_server_id);

  EXPECT_EQ(state->server_config(), other->server_config());
  EXPECT_EQ(state->source_address_token(), other->source_address_token());
  EXPECT_EQ(state->certs(), other->certs());
  EXPECT_EQ(1u, other->generation_counter());
}

TEST_F(QuicCryptoClientConfigTest, Canonical) {
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  config.AddCanonicalSuffix(".9oo91e.qjz9zk");
  QuicServerId canonical_id1("www.9oo91e.qjz9zk", 443, false);
  QuicServerId canonical_id2("mail.9oo91e.qjz9zk", 443, false);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_id1);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");
  state->SetProofValid();

  QuicCryptoClientConfig::CachedState* other =
      config.LookupOrCreate(canonical_id2);

  EXPECT_TRUE(state->IsEmpty());
  EXPECT_EQ(state->server_config(), other->server_config());
  EXPECT_EQ(state->source_address_token(), other->source_address_token());
  EXPECT_EQ(state->certs(), other->certs());
  EXPECT_EQ(1u, other->generation_counter());

  QuicServerId different_id("mail.google.org", 443, false);
  EXPECT_TRUE(config.LookupOrCreate(different_id)->IsEmpty());
}

TEST_F(QuicCryptoClientConfigTest, CanonicalNotUsedIfNotValid) {
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  config.AddCanonicalSuffix(".9oo91e.qjz9zk");
  QuicServerId canonical_id1("www.9oo91e.qjz9zk", 443, false);
  QuicServerId canonical_id2("mail.9oo91e.qjz9zk", 443, false);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_id1);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");

  // Do not set the proof as valid, and check that it is not used
  // as a canonical entry.
  EXPECT_TRUE(config.LookupOrCreate(canonical_id2)->IsEmpty());
}

TEST_F(QuicCryptoClientConfigTest, ClearCachedStates) {
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());

  // Create two states on different origins.
  struct TestCase {
    TestCase(const std::string& host, QuicCryptoClientConfig* config)
        : server_id(host, 443, false),
          state(config->LookupOrCreate(server_id)) {
      // TODO(rch): Populate other fields of |state|.
      CryptoHandshakeMessage scfg;
      scfg.set_tag(kSCFG);
      uint64_t future = 1;
      scfg.SetValue(kEXPY, future);
      scfg.SetStringPiece(kSCID, "12345678");
      std::string details;
      state->SetServerConfig(scfg.GetSerialized().AsStringPiece(),
                             QuicWallTime::FromUNIXSeconds(0),
                             QuicWallTime::FromUNIXSeconds(future), &details);

      std::vector<std::string> certs(1);
      certs[0] = "Hello Cert for " + host;
      state->SetProof(certs, "cert_sct", "chlo_hash", "signature");
      state->set_source_address_token("TOKEN");
      state->SetProofValid();

      // The generation counter starts at 2, because proof has been once
      // invalidated in SetServerConfig().
      EXPECT_EQ(2u, state->generation_counter());
    }

    QuicServerId server_id;
    QuicCryptoClientConfig::CachedState* state;
  } test_cases[] = {TestCase("www.9oo91e.qjz9zk", &config),
                    TestCase("www.example.com", &config)};

  // Verify LookupOrCreate returns the same data.
  for (const TestCase& test_case : test_cases) {
    QuicCryptoClientConfig::CachedState* other =
        config.LookupOrCreate(test_case.server_id);
    EXPECT_EQ(test_case.state, other);
    EXPECT_EQ(2u, other->generation_counter());
  }

  // Clear the cached state for www.9oo91e.qjz9zk.
  OneServerIdFilter google_com_filter(&test_cases[0].server_id);
  config.ClearCachedStates(google_com_filter);

  // Verify LookupOrCreate doesn't have any data for 9oo91e.qjz9zk.
  QuicCryptoClientConfig::CachedState* cleared_cache =
      config.LookupOrCreate(test_cases[0].server_id);

  EXPECT_EQ(test_cases[0].state, cleared_cache);
  EXPECT_FALSE(cleared_cache->proof_valid());
  EXPECT_TRUE(cleared_cache->server_config().empty());
  EXPECT_TRUE(cleared_cache->certs().empty());
  EXPECT_TRUE(cleared_cache->cert_sct().empty());
  EXPECT_TRUE(cleared_cache->signature().empty());
  EXPECT_EQ(3u, cleared_cache->generation_counter());

  // But it still does for www.example.com.
  QuicCryptoClientConfig::CachedState* existing_cache =
      config.LookupOrCreate(test_cases[1].server_id);

  EXPECT_EQ(test_cases[1].state, existing_cache);
  EXPECT_TRUE(existing_cache->proof_valid());
  EXPECT_FALSE(existing_cache->server_config().empty());
  EXPECT_FALSE(existing_cache->certs().empty());
  EXPECT_FALSE(existing_cache->cert_sct().empty());
  EXPECT_FALSE(existing_cache->signature().empty());
  EXPECT_EQ(2u, existing_cache->generation_counter());

  // Clear all cached states.
  AllServerIdsFilter all_server_ids;
  config.ClearCachedStates(all_server_ids);

  // The data for www.example.com should now be cleared as well.
  cleared_cache = config.LookupOrCreate(test_cases[1].server_id);

  EXPECT_EQ(test_cases[1].state, cleared_cache);
  EXPECT_FALSE(cleared_cache->proof_valid());
  EXPECT_TRUE(cleared_cache->server_config().empty());
  EXPECT_TRUE(cleared_cache->certs().empty());
  EXPECT_TRUE(cleared_cache->cert_sct().empty());
  EXPECT_TRUE(cleared_cache->signature().empty());
  EXPECT_EQ(3u, cleared_cache->generation_counter());
}

TEST_F(QuicCryptoClientConfigTest, ProcessReject) {
  CryptoHandshakeMessage rej;
  crypto_test_utils::FillInDummyReject(&rej);

  // Now process the rejection.
  QuicCryptoClientConfig::CachedState cached;
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> out_params(
      new QuicCryptoNegotiatedParameters);
  std::string error;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  EXPECT_THAT(config.ProcessRejection(rej, QuicWallTime::FromUNIXSeconds(0),
                                      AllSupportedTransportVersions().front(),
                                      "", &cached, out_params, &error),
              IsQuicNoError());
  EXPECT_FALSE(cached.has_server_nonce());
}

TEST_F(QuicCryptoClientConfigTest, ProcessRejectWithLongTTL) {
  CryptoHandshakeMessage rej;
  crypto_test_utils::FillInDummyReject(&rej);
  QuicTime::Delta one_week = QuicTime::Delta::FromSeconds(kNumSecondsPerWeek);
  int64_t long_ttl = 3 * one_week.ToSeconds();
  rej.SetValue(kSTTL, long_ttl);

  // Now process the rejection.
  QuicCryptoClientConfig::CachedState cached;
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> out_params(
      new QuicCryptoNegotiatedParameters);
  std::string error;
  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  EXPECT_THAT(config.ProcessRejection(rej, QuicWallTime::FromUNIXSeconds(0),
                                      AllSupportedTransportVersions().front(),
                                      "", &cached, out_params, &error),
              IsQuicNoError());
  cached.SetProofValid();
  EXPECT_FALSE(cached.IsComplete(QuicWallTime::FromUNIXSeconds(long_ttl)));
  EXPECT_FALSE(
      cached.IsComplete(QuicWallTime::FromUNIXSeconds(one_week.ToSeconds())));
  EXPECT_TRUE(cached.IsComplete(
      QuicWallTime::FromUNIXSeconds(one_week.ToSeconds() - 1)));
}

TEST_F(QuicCryptoClientConfigTest, ServerNonceinSHLO) {
  // Test that the server must include a nonce in the SHLO.
  CryptoHandshakeMessage msg;
  msg.set_tag(kSHLO);
  // Choose the latest version.
  ParsedQuicVersionVector supported_versions;
  ParsedQuicVersion version = AllSupportedVersions().front();
  supported_versions.push_back(version);
  msg.SetVersionVector(kVER, supported_versions);

  QuicCryptoClientConfig config(crypto_test_utils::ProofVerifierForTesting());
  QuicCryptoClientConfig::CachedState cached;
  QuicReferenceCountedPointer<QuicCryptoNegotiatedParameters> out_params(
      new QuicCryptoNegotiatedParameters);
  std::string error_details;
  EXPECT_THAT(config.ProcessServerHello(msg, EmptyQuicConnectionId(), version,
                                        supported_versions, &cached, out_params,
                                        &error_details),
              IsError(QUIC_INVALID_CRYPTO_MESSAGE_PARAMETER));
  EXPECT_EQ("server hello missing server nonce", error_details);
}

}  // namespace test
}  // namespace quic
