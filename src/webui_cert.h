/**
 * @file src/webui_cert.h
 * @brief Declarations for the web UI's browser-trusted TLS certificate.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "crypto.h"

/**
 * @brief TLS certificate for the web UI, issued by a per-host certificate authority that clients can trust.
 *
 * The authority is name-constrained to `.local` names, `localhost` and private addresses, so a client
 * that trusts it cannot be shown a certificate for any other site. Moonlight pairing keeps using its own
 * certificate, so trusting the web UI never requires paired clients to pair again.
 */
namespace webui_cert {
  /**
   * @brief Names a web UI certificate covers.
   */
  struct names_t {
    std::vector<std::string> dns;  ///< DNS names in lower case.
    std::vector<std::string> ips;  ///< IP addresses in canonical text form.

    /**
     * @brief Compare two name sets, including order.
     *
     * @param other Name set to compare with.
     * @return True when both sets hold the same names in the same order.
     */
    bool operator==(const names_t &other) const = default;
  };

  /**
   * @brief Files the web server and clients use.
   */
  struct files_t {
    std::filesystem::path cert;  ///< Web UI certificate followed by the authority certificate.
    std::filesystem::path pkey;  ///< Web UI private key.
    std::filesystem::path authority;  ///< Authority certificate for clients to trust.
  };

  /**
   * @brief Report whether an address may appear in the web UI certificate.
   *
   * @param address IPv4 or IPv6 address in text form.
   * @return True for loopback, RFC 1918, carrier-grade NAT (used by Tailscale) and unique local IPv6 addresses.
   */
  bool is_private_address(std::string_view address);

  /**
   * @brief Derive the host label used for `<label>.local`.
   *
   * @param host_name Host name as reported by the platform, possibly with a domain or spaces.
   * @return Lower-case label with characters outside `[a-z0-9-]` removed, or empty if nothing is left.
   */
  std::string host_label(std::string_view host_name);

  /**
   * @brief Build the names the web UI certificate should cover.
   *
   * @param host_name Host name as reported by the platform.
   * @param addresses Local addresses of this host; addresses that are not private are left out.
   * @return `<label>.local` and `localhost`, plus the loopback and private addresses.
   */
  names_t host_names(std::string_view host_name, const std::vector<std::string> &addresses);

  /**
   * @brief Create the certificate authority for this host.
   *
   * @param host_name Host name as reported by the platform, used in the authority's name.
   * @param now Current time.
   * @return PEM certificate and private key of a name-constrained authority valid for ten years,
   *         or empty strings if it could not be created.
   */
  crypto::creds_t make_authority(std::string_view host_name, std::chrono::system_clock::time_point now);

  /**
   * @brief Issue a web UI certificate.
   *
   * @param authority PEM certificate and private key of the issuing authority.
   * @param names Names to cover.
   * @param now Current time.
   * @return PEM certificate and private key, or empty if the authority cannot be loaded or signing fails.
   */
  std::optional<crypto::creds_t> issue(const crypto::creds_t &authority, const names_t &names, std::chrono::system_clock::time_point now);

  /**
   * @brief Read the names a certificate covers.
   *
   * @param cert Certificate to read.
   * @return DNS names and IP addresses from the subject alternative name extension.
   */
  names_t certificate_names(const crypto::x509_t &cert);

  /**
   * @brief Decide whether a web UI certificate has to be issued again.
   *
   * @param cert Current web UI certificate, or null if there is none.
   * @param authority Current authority certificate.
   * @param names Names the certificate should cover.
   * @param now Current time.
   * @return True when the certificate is missing, was signed by another authority, covers other names,
   *         or expires within 30 days.
   */
  bool needs_reissue(const crypto::x509_t &cert, const crypto::x509_t &authority, const names_t &names, std::chrono::system_clock::time_point now);

  /**
   * @brief Find this host's primary local addresses.
   *
   * @return The source addresses the host would use for IPv4 and IPv6 traffic, when it has a route.
   */
  std::vector<std::string> local_addresses();

  /**
   * @brief Load or create the authority and web UI certificate.
   *
   * The authority is kept across runs so clients only have to trust it once. The web UI certificate is
   * issued again whenever the host's names or addresses change, or when it is about to expire.
   *
   * @param dir Directory that holds the files.
   * @param host_name Host name as reported by the platform.
   * @param addresses Local addresses of this host.
   * @param now Current time.
   * @return Paths of the files, or empty if they could not be created.
   */
  std::optional<files_t> ensure(const std::filesystem::path &dir, std::string_view host_name, const std::vector<std::string> &addresses, std::chrono::system_clock::time_point now);
}  // namespace webui_cert
