/**
 * @file src/gabs/network.h
 * @brief Gabs Remote: network adapters of the PC, used for Wake-on-LAN and the presence page.
 *
 * Requests that reach the host through Tailscale arrive on a virtual adapter without a MAC address.
 * The app still needs the MAC of the physical card to wake the PC, so the host falls back to the
 * physical adapter that is up and has an IPv4 gateway.
 */
#pragma once

// lib includes
#include <boost/asio/ip/address.hpp>

// standard includes
#include <string>
#include <string_view>
#include <vector>

namespace gabs::network {

  /**
   * @brief One network adapter of the PC.
   */
  struct adapter_t {
    std::string mac;  ///< "aa:bb:cc:dd:ee:ff" (lower case), empty when the adapter has none.
    std::vector<std::string> addresses;  ///< Unicast addresses, normalized strings.
    bool physical;  ///< Ethernet or Wi-Fi card.
    bool up;  ///< Operational status is up.
    bool has_ipv4_gateway;  ///< At least one IPv4 default gateway.
  };

  /**
   * @brief One unicast address of the PC.
   */
  struct ip_address_t {
    std::string address;
    bool deprecated;  ///< IPv6 address past its preferred lifetime.
  };

  /**
   * @brief MAC address to report for Wake-on-LAN.
   * @param local_address Address the request arrived on.
   * @param adapters Adapters of the PC.
   * @return The MAC of the adapter that has `local_address` (if it has one), else the MAC of the first
   *         physical adapter that is up and has an IPv4 gateway, else "00:00:00:00:00:00".
   */
  std::string wol_mac_for(std::string_view local_address, const std::vector<adapter_t> &adapters);

  /**
   * @brief Address to report as `LocalIP` in /serverinfo.
   *
   * Over Tailscale the request arrives on 100.64.0.0/10; the app would store that as the PC's home
   * address and lose the real one, so the host reports the IPv4 of the physical card instead
   * (127.0.0.1, which the app ignores, if there is none). Other addresses are returned unchanged.
   */
  std::string lan_ipv4_for(std::string_view local_address, const std::vector<adapter_t> &adapters);

  /**
   * @brief `lan_ipv4_for(local_address, enumerate_adapters())`.
   */
  std::string lan_ipv4_address(std::string_view local_address);

  /**
   * @brief Adapters of this PC (platform specific).
   */
  std::vector<adapter_t> enumerate_adapters();

  /**
   * @brief Unicast addresses of this PC (platform specific).
   */
  std::vector<ip_address_t> enumerate_ip_addresses();

  /**
   * @brief `wol_mac_for(local_address, enumerate_adapters())`, logging a warning only the first time
   *        each address needs the fallback.
   */
  std::string wol_mac_address(std::string_view local_address);

  /**
   * @brief Whether an outgoing UDP packet should carry this address as its source (IP_PKTINFO).
   *
   * The video and audio threads start sending before the control stream connects and sets the
   * session's local address. Until then the address is unspecified, and Windows rejects a packet
   * info with 0.0.0.0 (WSASendMsg error 10022). Those packets go out with the source the OS picks.
   */
  bool use_as_source(const boost::asio::ip::address &address);

  /**
   * @brief Control buffer to hand to WSASendMsg: null when no control message was written.
   *
   * A non-null buffer with length 0 makes WSASendMsg fail with WSAEFAULT (10014).
   */
  char *control_buffer(char *buffer, unsigned long used);

}  // namespace gabs::network
