/**
 * @file tests/unit/test_webui_cert.cpp
 * @brief Test src/webui_cert.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <openssl/x509v3.h>

// local includes
#include <src/crypto.h>
#include <src/webui_cert.h>

using namespace std::literals;

namespace {
  using x509_store_t = crypto::x509_store_t;
  using x509_store_ctx_t = crypto::x509_store_ctx_t;

  const auto now = std::chrono::system_clock::now();

  std::string read_text(const std::filesystem::path &path) {
    std::ifstream in {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }

  /**
   * @brief Verify a certificate against a single trusted authority the way a client that trusts it would.
   */
  int verify(const crypto::x509_t &cert, const crypto::x509_t &authority) {
    x509_store_t store {X509_STORE_new()};
    X509_STORE_add_cert(store.get(), authority.get());
    x509_store_ctx_t ctx {X509_STORE_CTX_new()};
    X509_STORE_CTX_init(ctx.get(), store.get(), cert.get(), nullptr);
    X509_STORE_CTX_set_purpose(ctx.get(), X509_PURPOSE_SSL_SERVER);
    if (X509_verify_cert(ctx.get()) == 1) {
      return X509_V_OK;
    }
    return X509_STORE_CTX_get_error(ctx.get());
  }

  webui_cert::names_t mac_mini_names() {
    return webui_cert::host_names("Weis-Mac-Mini", {"192.168.50.241"});
  }

  class WebUICertFilesTest: public BaseTest {
  protected:
    std::filesystem::path dir;

    void SetUp() override {
      BaseTest::SetUp();
      dir = std::filesystem::temp_directory_path() / "sunshine_test_webui_cert";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove_all(dir);
    }

    void TearDown() override {
      std::filesystem::remove_all(dir);
      BaseTest::TearDown();
    }
  };
}  // namespace

TEST(WebUICertTest, PrivateAddressesAreAccepted) {
  for (const auto *address : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.50.241", "100.64.0.1", "100.127.255.254", "::1", "fd7a:115c:a1e0::1", "fc00::1"}) {
    EXPECT_TRUE(webui_cert::is_private_address(address)) << address;
  }
}

TEST(WebUICertTest, PublicAndInvalidAddressesAreRejected) {
  for (const auto *address : {"8.8.8.8", "172.32.0.1", "192.169.0.1", "100.128.0.1", "2001:db8::1", "fe80::1", "::ffff:192.168.1.1", "", "not-an-address", "weis-mac-mini.local"}) {
    EXPECT_FALSE(webui_cert::is_private_address(address)) << address;
  }
}

TEST(WebUICertTest, HostLabelMatchesTheLocalHostName) {
  EXPECT_EQ(webui_cert::host_label("Weis-Mac-Mini"), "weis-mac-mini");
  EXPECT_EQ(webui_cert::host_label("Weis-Mac-Mini.local"), "weis-mac-mini");
  EXPECT_EQ(webui_cert::host_label("Wei\xE2\x80\x99s Mac Mini"), "weis-mac-mini");
  EXPECT_EQ(webui_cert::host_label("DESKTOP_42  PC"), "desktop-42-pc");
  EXPECT_EQ(webui_cert::host_label("-edge-"), "edge");
  EXPECT_EQ(webui_cert::host_label("\xE2\x80\x99"), "");
  EXPECT_EQ(webui_cert::host_label(std::string(80, 'a')).size(), 63U);
}

TEST(WebUICertTest, HostNamesCoverLocalNamesAndPrivateAddressesOnly) {
  const auto names = webui_cert::host_names("Weis-Mac-Mini.local", {"192.168.50.241", "8.8.8.8", "bogus", "192.168.50.241", "fd7a:115c:a1e0:0:0:0:0:1", "2001:db8::5"});
  EXPECT_EQ(names.dns, (std::vector<std::string> {"weis-mac-mini.local", "localhost"}));
  EXPECT_EQ(names.ips, (std::vector<std::string> {"127.0.0.1", "::1", "192.168.50.241", "fd7a:115c:a1e0::1"}));
}

TEST(WebUICertTest, HostNamesWithoutALabelStillCoverLocalhost) {
  const auto names = webui_cert::host_names("\xE2\x80\x99", {});
  EXPECT_EQ(names.dns, (std::vector<std::string> {"localhost"}));
  EXPECT_EQ(names.ips, (std::vector<std::string> {"127.0.0.1", "::1"}));
}

TEST(WebUICertTest, AuthorityIsANameConstrainedCa) {
  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  const auto cert = crypto::x509(authority.x509);
  ASSERT_TRUE(cert);
  ASSERT_TRUE(crypto::pkey(authority.pkey));

  EXPECT_EQ(X509_check_ca(cert.get()), 1);
  EXPECT_NE(X509_get_ext_by_NID(cert.get(), NID_name_constraints, -1), -1);

  char common_name[128] {};
  X509_NAME_get_text_by_NID(X509_get_subject_name(cert.get()), NID_commonName, common_name, sizeof(common_name));
  EXPECT_STREQ(common_name, "Sunshine Local CA (weis-mac-mini)");
}

TEST(WebUICertTest, IssuedCertificateIsTrustedThroughTheAuthority) {
  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  const auto names = mac_mini_names();
  const auto issued = webui_cert::issue(authority, names, now);
  ASSERT_TRUE(issued.has_value());

  const auto cert = crypto::x509(issued->x509);
  const auto authority_cert = crypto::x509(authority.x509);
  ASSERT_TRUE(cert);
  EXPECT_EQ(verify(cert, authority_cert), X509_V_OK);
  EXPECT_EQ(webui_cert::certificate_names(cert), names);
  EXPECT_EQ(X509_check_ca(cert.get()), 0);

  // Clients reject server certificates valid for more than 398 days.
  int days = 0;
  int seconds = 0;
  ASSERT_EQ(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(cert.get()), X509_get0_notAfter(cert.get())), 1);
  EXPECT_LE(days, 398);

  const auto key = crypto::pkey(issued->pkey);
  ASSERT_TRUE(key);
  EXPECT_EQ(X509_check_private_key(cert.get(), key.get()), 1);
}

TEST(WebUICertTest, AuthorityCannotVouchForOtherSites) {
  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  const auto authority_cert = crypto::x509(authority.x509);

  const auto public_name = webui_cert::issue(authority, {{"example.com"}, {}}, now);
  ASSERT_TRUE(public_name.has_value());
  EXPECT_EQ(verify(crypto::x509(public_name->x509), authority_cert), X509_V_ERR_PERMITTED_VIOLATION);

  const auto public_address = webui_cert::issue(authority, {{"weis-mac-mini.local"}, {"8.8.8.8"}}, now);
  ASSERT_TRUE(public_address.has_value());
  EXPECT_EQ(verify(crypto::x509(public_address->x509), authority_cert), X509_V_ERR_PERMITTED_VIOLATION);
}

TEST(WebUICertTest, IssueFailsWithoutAUsableAuthorityOrName) {
  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  EXPECT_FALSE(webui_cert::issue({}, mac_mini_names(), now).has_value());
  EXPECT_FALSE(webui_cert::issue({authority.x509, ""}, mac_mini_names(), now).has_value());
  EXPECT_FALSE(webui_cert::issue(authority, {{}, {"192.168.50.241"}}, now).has_value());
}

TEST(WebUICertTest, CertificateNamesOfMissingOrPlainCertificatesAreEmpty) {
  EXPECT_EQ(webui_cert::certificate_names(nullptr), webui_cert::names_t {});

  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  EXPECT_EQ(webui_cert::certificate_names(crypto::x509(authority.x509)), webui_cert::names_t {});
}

TEST(WebUICertTest, ReissueOnlyWhenSomethingChanged) {
  const auto authority = webui_cert::make_authority("Weis-Mac-Mini", now);
  const auto authority_cert = crypto::x509(authority.x509);
  const auto names = mac_mini_names();
  const auto cert = crypto::x509(webui_cert::issue(authority, names, now)->x509);

  EXPECT_FALSE(webui_cert::needs_reissue(cert, authority_cert, names, now));
  EXPECT_TRUE(webui_cert::needs_reissue(nullptr, authority_cert, names, now));
  EXPECT_TRUE(webui_cert::needs_reissue(cert, nullptr, names, now));
  EXPECT_TRUE(webui_cert::needs_reissue(cert, authority_cert, webui_cert::host_names("Weis-Mac-Mini", {"192.168.50.7"}), now));
  EXPECT_TRUE(webui_cert::needs_reissue(cert, authority_cert, names, now + std::chrono::hours {24 * 380}));

  const auto other_authority = crypto::x509(webui_cert::make_authority("Weis-Mac-Mini", now).x509);
  EXPECT_TRUE(webui_cert::needs_reissue(cert, other_authority, names, now));
}

TEST(WebUICertTest, LocalAddressesAreValidAddresses) {
  for (const auto &address : webui_cert::local_addresses()) {
    boost::system::error_code ec;
    boost::asio::ip::make_address(address, ec);
    EXPECT_FALSE(ec) << address;
  }
}

TEST_F(WebUICertFilesTest, EnsureCreatesTheAuthorityAndCertificateChain) {
  const auto files = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(files.has_value());

  const auto authority_pem = read_text(files->authority);
  const auto chain = read_text(files->cert);
  ASSERT_FALSE(authority_pem.empty());
  EXPECT_TRUE(chain.ends_with(authority_pem));
  EXPECT_TRUE(std::filesystem::exists(dir / "webui_ca_key.pem"));

  const auto cert = crypto::x509(chain);
  EXPECT_EQ(verify(cert, crypto::x509(authority_pem)), X509_V_OK);
  EXPECT_EQ(webui_cert::certificate_names(cert), mac_mini_names());
  EXPECT_EQ(X509_check_private_key(cert.get(), crypto::pkey(read_text(files->pkey)).get()), 1);
}

TEST_F(WebUICertFilesTest, EnsureKeepsFilesThatAreStillValid) {
  const auto first = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(first.has_value());
  const auto authority_pem = read_text(first->authority);
  const auto chain = read_text(first->cert);

  const auto second = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(read_text(second->authority), authority_pem);
  EXPECT_EQ(read_text(second->cert), chain);
}

TEST_F(WebUICertFilesTest, EnsureReissuesWhenTheAddressChangesButKeepsTheAuthority) {
  const auto first = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(first.has_value());
  const auto authority_pem = read_text(first->authority);
  const auto chain = read_text(first->cert);

  const auto second = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.7"}, now);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(read_text(second->authority), authority_pem);
  EXPECT_NE(read_text(second->cert), chain);
  EXPECT_EQ(webui_cert::certificate_names(crypto::x509(read_text(second->cert))), webui_cert::host_names("Weis-Mac-Mini", {"192.168.50.7"}));
}

TEST_F(WebUICertFilesTest, EnsureReissuesWhenTheKeyNoLongerMatches) {
  const auto first = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(first.has_value());
  const auto chain = read_text(first->cert);
  std::filesystem::remove(first->pkey);

  const auto second = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(second.has_value());
  const auto cert = crypto::x509(read_text(second->cert));
  EXPECT_NE(read_text(second->cert), chain);
  EXPECT_EQ(X509_check_private_key(cert.get(), crypto::pkey(read_text(second->pkey)).get()), 1);
}

TEST_F(WebUICertFilesTest, EnsureReplacesADamagedAuthority) {
  const auto first = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(first.has_value());
  const auto authority_pem = read_text(first->authority);
  {
    std::ofstream out {dir / "webui_ca_key.pem", std::ios::binary | std::ios::trunc};
    out << "damaged";
  }

  const auto second = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(second.has_value());
  const auto new_authority = read_text(second->authority);
  EXPECT_NE(new_authority, authority_pem);
  EXPECT_EQ(verify(crypto::x509(read_text(second->cert)), crypto::x509(new_authority)), X509_V_OK);
}

TEST_F(WebUICertFilesTest, EnsureReplacesAnAuthorityNearItsEnd) {
  const auto first = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now);
  ASSERT_TRUE(first.has_value());
  const auto authority_pem = read_text(first->authority);

  const auto later = now + std::chrono::hours {24 * 365 * 10};
  const auto second = webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, later);
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(read_text(second->authority), authority_pem);
}

TEST_F(WebUICertFilesTest, EnsureFailsWhenTheDirectoryIsNotWritable) {
  // A regular file where the directory should be makes every write fail.
  std::ofstream {dir} << "not a directory";

  EXPECT_FALSE(webui_cert::ensure(dir, "Weis-Mac-Mini", {"192.168.50.241"}, now).has_value());
}
