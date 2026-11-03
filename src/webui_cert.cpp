/**
 * @file src/webui_cert.cpp
 * @brief Definitions for the web UI's browser-trusted TLS certificate.
 */
// this include
#include "webui_cert.h"

// standard includes
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

// lib includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

// local includes
#include "logging.h"

using namespace std::literals;

namespace webui_cert {
  namespace {
    namespace fs = std::filesystem;

    using general_names_t = util::safe_ptr<GENERAL_NAMES, &GENERAL_NAMES_free>;
    using x509_extension_t = util::safe_ptr<X509_EXTENSION, &X509_EXTENSION_free>;
    using x509_name_t = util::safe_ptr<X509_NAME, &X509_NAME_free>;

    constexpr std::chrono::hours authority_lifetime {24 * 365 * 10};
    // Browsers cap publicly trusted certificates at 398 days, and some apply the cap to every certificate.
    constexpr std::chrono::hours certificate_lifetime {24 * 397};
    constexpr std::chrono::hours renew_before {24 * 30};
    // Tolerate a client clock that is slightly behind.
    constexpr std::chrono::hours backdate {1};

    // Must cover exactly what is_private_address() accepts and the DNS names host_names() produces.
    constexpr auto name_constraints =
      "critical,"
      "permitted;DNS:local,"
      "permitted;DNS:localhost,"
      "permitted;IP:127.0.0.0/255.0.0.0,"
      "permitted;IP:10.0.0.0/255.0.0.0,"
      "permitted;IP:172.16.0.0/255.240.0.0,"
      "permitted;IP:192.168.0.0/255.255.0.0,"
      "permitted;IP:100.64.0.0/255.192.0.0,"
      "permitted;IP:::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff,"
      "permitted;IP:fc00::/fe00::";

    std::string read_text(const fs::path &path) {
      std::ifstream in {path, std::ios::binary};
      return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
    }

    bool write_text(const fs::path &path, std::string_view contents, bool secret) {
      {
        std::ofstream out {path, std::ios::binary | std::ios::trunc};
        if (!out.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
          BOOST_LOG(error) << "Couldn't write "sv << path.string();
          return false;
        }
      }

      if (secret) {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
      }
      return true;
    }

    bool set_validity(X509 *cert, std::chrono::system_clock::time_point now, std::chrono::hours lifetime) {
      return ASN1_TIME_set(X509_getm_notBefore(cert), std::chrono::system_clock::to_time_t(now - backdate)) &&
             ASN1_TIME_set(X509_getm_notAfter(cert), std::chrono::system_clock::to_time_t(now + lifetime));
    }

    bool expires_before(const X509 *cert, std::chrono::system_clock::time_point when) {
      auto limit = std::chrono::system_clock::to_time_t(when);
      return X509_cmp_time(X509_get0_notAfter(cert), &limit) <= 0;
    }

    bool set_serial(X509 *cert) {
      crypto::bignum_t serial {BN_new()};
      // 159 bits fit the 20-byte limit for positive serial numbers.
      return serial && BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) &&
             BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert));
    }

    crypto::pkey_t make_key() {
      crypto::pkey_ctx_t ctx {EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr)};
      crypto::pkey_t key;
      if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0 || EVP_PKEY_keygen(ctx.get(), &key) <= 0) {
        return {};
      }
      return key;
    }

    x509_name_t make_name(std::string_view common_name) {
      x509_name_t name {X509_NAME_new()};
      constexpr std::string_view organization {"Sunshine"};
      if (!name || !X509_NAME_add_entry_by_txt(name.get(), "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(organization.data()), static_cast<int>(organization.size()), -1, 0) || !X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(common_name.data()), static_cast<int>(common_name.size()), -1, 0)) {
        return {};
      }
      return name;
    }

    bool add_extension(X509 *cert, X509V3_CTX &ctx, int nid, const std::string &value) {
      x509_extension_t extension {X509V3_EXT_nconf_nid(nullptr, &ctx, nid, value.c_str())};
      return extension && X509_add_ext(cert, extension.get(), -1);
    }

    std::string alt_names(const names_t &names) {
      std::string result;
      const auto append = [&result](std::string_view kind, const std::string &name) {
        if (!result.empty()) {
          result += ',';
        }
        result.append(kind).append(name);
      };
      for (const auto &name : names.dns) {
        append("DNS:"sv, name);
      }
      for (const auto &address : names.ips) {
        append("IP:"sv, address);
      }
      return result;
    }

    std::string describe(const names_t &names) {
      std::string result;
      for (const auto &name : names.dns) {
        result += result.empty() ? name : ", " + name;
      }
      for (const auto &address : names.ips) {
        result += result.empty() ? address : ", " + address;
      }
      return result;
    }

    bool authority_usable(const crypto::x509_t &cert, const crypto::pkey_t &key, std::chrono::system_clock::time_point now) {
      // A certificate must never outlive its authority.
      return cert && key && X509_check_private_key(cert.get(), key.get()) == 1 && !expires_before(cert.get(), now + certificate_lifetime);
    }
  }  // namespace

  bool is_private_address(std::string_view address) {
    boost::system::error_code ec;
    const auto parsed = boost::asio::ip::make_address(std::string {address}, ec);
    if (ec) {
      return false;
    }

    if (parsed.is_v4()) {
      const auto value = parsed.to_v4().to_uint();
      const auto within = [value](std::uint32_t network, int prefix) {
        return (value >> (32 - prefix)) == (network >> (32 - prefix));
      };
      return within(0x7F000000, 8) || within(0x0A000000, 8) || within(0xAC100000, 12) ||
             within(0xC0A80000, 16) || within(0x64400000, 10);
    }

    const auto v6 = parsed.to_v6();
    return v6.is_loopback() || (v6.to_bytes()[0] & 0xFE) == 0xFC;
  }

  std::string host_label(std::string_view host_name) {
    std::string label;
    for (const auto c : host_name.substr(0, host_name.find('.'))) {
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        label += c;
      } else if (c >= 'A' && c <= 'Z') {
        label += static_cast<char>(c - 'A' + 'a');
      } else if ((c == '-' || c == ' ' || c == '_') && !label.empty() && label.back() != '-') {
        label += '-';
      }
    }

    // DNS labels are limited to 63 characters and can't end with a hyphen.
    label.resize(std::min<std::size_t>(label.size(), 63));
    while (!label.empty() && label.back() == '-') {
      label.pop_back();
    }
    return label;
  }

  names_t host_names(std::string_view host_name, const std::vector<std::string> &addresses) {
    names_t names;
    if (const auto label = host_label(host_name); !label.empty()) {
      names.dns.push_back(label + ".local");
    }
    names.dns.emplace_back("localhost");

    names.ips = {"127.0.0.1", "::1"};
    for (const auto &address : addresses) {
      boost::system::error_code ec;
      const auto parsed = boost::asio::ip::make_address(address, ec);
      if (ec) {
        continue;
      }

      const auto text = parsed.to_string();
      if (is_private_address(text) && std::ranges::find(names.ips, text) == names.ips.end()) {
        names.ips.push_back(text);
      }
    }
    return names;
  }

  crypto::creds_t make_authority(std::string_view host_name, std::chrono::system_clock::time_point now) {
    const auto label = host_label(host_name);
    auto key = make_key();
    crypto::x509_t cert {X509_new()};
    const auto name = make_name(label.empty() ? "Sunshine Local CA"s : "Sunshine Local CA (" + label + ')');
    if (!key || !cert || !name || !X509_set_version(cert.get(), 2) || !set_serial(cert.get()) || !set_validity(cert.get(), now, authority_lifetime) || !X509_set_subject_name(cert.get(), name.get()) || !X509_set_issuer_name(cert.get(), name.get()) || !X509_set_pubkey(cert.get(), key.get())) {
      return {};
    }

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
    if (!add_extension(cert.get(), ctx, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") || !add_extension(cert.get(), ctx, NID_key_usage, "critical,keyCertSign,cRLSign") || !add_extension(cert.get(), ctx, NID_subject_key_identifier, "hash") || !add_extension(cert.get(), ctx, NID_name_constraints, name_constraints) || !X509_sign(cert.get(), key.get(), EVP_sha256())) {
      return {};
    }

    return {crypto::pem(cert), crypto::pem(key)};
  }

  std::optional<crypto::creds_t> issue(const crypto::creds_t &authority, const names_t &names, std::chrono::system_clock::time_point now) {
    auto authority_cert = crypto::x509(authority.x509);
    const auto authority_key = crypto::pkey(authority.pkey);
    if (!authority_cert || !authority_key || names.dns.empty()) {
      return std::nullopt;
    }

    auto key = make_key();
    crypto::x509_t cert {X509_new()};
    const auto name = make_name(names.dns.front());
    if (!key || !cert || !name || !X509_set_version(cert.get(), 2) || !set_serial(cert.get()) || !set_validity(cert.get(), now, certificate_lifetime) || !X509_set_subject_name(cert.get(), name.get()) || !X509_set_issuer_name(cert.get(), X509_get_subject_name(authority_cert.get())) || !X509_set_pubkey(cert.get(), key.get())) {
      return std::nullopt;
    }

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, authority_cert.get(), cert.get(), nullptr, nullptr, 0);
    if (!add_extension(cert.get(), ctx, NID_basic_constraints, "critical,CA:FALSE") || !add_extension(cert.get(), ctx, NID_key_usage, "critical,digitalSignature") || !add_extension(cert.get(), ctx, NID_ext_key_usage, "serverAuth") || !add_extension(cert.get(), ctx, NID_subject_key_identifier, "hash") || !add_extension(cert.get(), ctx, NID_authority_key_identifier, "keyid:always") || !add_extension(cert.get(), ctx, NID_subject_alt_name, alt_names(names)) || !X509_sign(cert.get(), authority_key.get(), EVP_sha256())) {
      return std::nullopt;
    }

    return crypto::creds_t {crypto::pem(cert), crypto::pem(key)};
  }

  names_t certificate_names(const crypto::x509_t &cert) {
    names_t names;
    if (!cert) {
      return names;
    }

    const general_names_t alt {static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(cert.get(), NID_subject_alt_name, nullptr, nullptr))};
    if (!alt) {
      return names;
    }

    for (int i = 0; i < sk_GENERAL_NAME_num(alt.get()); ++i) {
      const auto *entry = sk_GENERAL_NAME_value(alt.get(), i);
      if (entry->type == GEN_DNS) {
        const auto *value = entry->d.dNSName;
        names.dns.emplace_back(reinterpret_cast<const char *>(ASN1_STRING_get0_data(value)), ASN1_STRING_length(value));
      } else if (entry->type == GEN_IPADD) {
        const auto *value = entry->d.iPAddress;
        const auto *data = ASN1_STRING_get0_data(value);
        if (ASN1_STRING_length(value) == 4) {
          boost::asio::ip::address_v4::bytes_type bytes;
          std::copy_n(data, bytes.size(), bytes.begin());
          names.ips.push_back(boost::asio::ip::address_v4 {bytes}.to_string());
        } else if (ASN1_STRING_length(value) == 16) {
          boost::asio::ip::address_v6::bytes_type bytes;
          std::copy_n(data, bytes.size(), bytes.begin());
          names.ips.push_back(boost::asio::ip::address_v6 {bytes}.to_string());
        }
      }
    }
    return names;
  }

  bool needs_reissue(const crypto::x509_t &cert, const crypto::x509_t &authority, const names_t &names, std::chrono::system_clock::time_point now) {
    if (!cert || !authority) {
      return true;
    }

    const crypto::pkey_t authority_key {X509_get_pubkey(authority.get())};
    if (!authority_key || X509_verify(cert.get(), authority_key.get()) != 1) {
      return true;
    }

    return expires_before(cert.get(), now + renew_before) || certificate_names(cert) != names;
  }

  std::vector<std::string> local_addresses() {
    std::vector<std::string> addresses;
    boost::asio::io_context io;

    // Connecting a UDP socket only picks the route and source address; nothing is sent. The targets are
    // documentation addresses, so they follow the default route without naming a real host.
    for (const auto *probe : {"192.0.2.1", "2001:db8::1"}) {
      boost::system::error_code ec;
      const auto target = boost::asio::ip::make_address(probe, ec);
      boost::asio::ip::udp::socket socket {io};
      socket.open(target.is_v4() ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6(), ec);
      if (!ec) {
        socket.connect({target, 9}, ec);
      }
      if (ec) {
        continue;
      }

      const auto local = socket.local_endpoint(ec);
      if (!ec) {
        addresses.push_back(local.address().to_string());
      }
    }
    return addresses;
  }

  std::optional<files_t> ensure(const std::filesystem::path &dir, std::string_view host_name, const std::vector<std::string> &addresses, std::chrono::system_clock::time_point now) {
    const files_t files {dir / "webui_cert.pem", dir / "webui_key.pem", dir / "webui_ca.pem"};
    const auto authority_key_file = dir / "webui_ca_key.pem";

    std::error_code ec;
    fs::create_directories(dir, ec);

    crypto::creds_t authority {read_text(files.authority), read_text(authority_key_file)};
    auto authority_cert = crypto::x509(authority.x509);
    bool new_authority = false;
    if (!authority_usable(authority_cert, crypto::pkey(authority.pkey), now)) {
      authority = make_authority(host_name, now);
      if (authority.x509.empty()) {
        BOOST_LOG(error) << "Couldn't create the web UI certificate authority"sv;
        return std::nullopt;
      }
      if (!write_text(authority_key_file, authority.pkey, true) || !write_text(files.authority, authority.x509, false)) {
        return std::nullopt;
      }

      authority_cert = crypto::x509(authority.x509);
      new_authority = true;
      BOOST_LOG(info) << "Created a web UI certificate authority; trust "sv << files.authority.string()
                      << " on clients to open the web UI without a certificate warning"sv;
    }

    const auto names = host_names(host_name, addresses);
    const auto current = crypto::x509(read_text(files.cert));
    const auto current_key = crypto::pkey(read_text(files.pkey));
    const bool key_matches = current && current_key && X509_check_private_key(current.get(), current_key.get()) == 1;
    if (new_authority || !key_matches || needs_reissue(current, authority_cert, names, now)) {
      const auto issued = issue(authority, names, now);
      if (!issued) {
        BOOST_LOG(error) << "Couldn't issue the web UI certificate"sv;
        return std::nullopt;
      }

      // The authority follows the certificate so clients receive the whole chain.
      if (!write_text(files.pkey, issued->pkey, true) || !write_text(files.cert, issued->x509 + authority.x509, false)) {
        return std::nullopt;
      }
      BOOST_LOG(info) << "Issued a web UI certificate for "sv << describe(names);
    }

    return files;
  }
}  // namespace webui_cert
