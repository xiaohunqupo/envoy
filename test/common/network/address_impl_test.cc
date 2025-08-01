#include <iostream>
#include <memory>
#include <string>

#include "envoy/common/exception.h"
#include "envoy/common/platform.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/common/fmt.h"
#include "source/common/common/utility.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/listen_socket_impl.h"
#include "source/common/network/utility.h"

#include "test/mocks/api/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/threadsafe_singleton_injector.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Network {
namespace Address {
namespace {

Ipv6Instance v4MappedV6Instance(const std::string& address) {
  return Ipv6Instance(address, /*port=*/0, /*sock_interface=*/nullptr, /*v6only=*/false);
}

bool addressesEqual(const InstanceConstSharedPtr& a, const Instance& b) {
  if (a == nullptr || a->type() != Type::Ip || b.type() != Type::Ip) {
    return false;
  } else {
    return a->ip()->addressAsString() == b.ip()->addressAsString();
  }
}

void testSocketBindAndConnect(Network::Address::IpVersion ip_version, bool v6only) {
  auto addr_port = Network::Utility::parseInternetAddressAndPortNoThrow(
      fmt::format("{}:0", Network::Test::getAnyAddressUrlString(ip_version)), v6only);
  ASSERT_NE(addr_port, nullptr);

  if (addr_port->ip()->port() == 0) {
    addr_port = Network::Test::findOrCheckFreePort(addr_port, Socket::Type::Stream);
  }
  ASSERT_NE(addr_port, nullptr);
  ASSERT_NE(addr_port->ip(), nullptr);

  // Create a socket on which we'll listen for connections from clients.
  SocketImpl sock(Socket::Type::Stream, addr_port, nullptr, {});
  EXPECT_TRUE(sock.ioHandle().isOpen()) << addr_port->asString();

  // Check that IPv6 sockets accept IPv6 connections only.
  if (addr_port->ip()->version() == IpVersion::v6) {
    int socket_v6only = 0;
    socklen_t size_int = sizeof(socket_v6only);
    ASSERT_GE(
        sock.getSocketOption(IPPROTO_IPV6, IPV6_V6ONLY, &socket_v6only, &size_int).return_value_,
        0);
    EXPECT_EQ(v6only, socket_v6only != 0);
  }

  // Bind the socket to the desired address and port.
  const Api::SysCallIntResult result = sock.bind(addr_port);
  ASSERT_EQ(result.return_value_, 0)
      << addr_port->asString() << "\nerror: " << errorDetails(result.errno_)
      << "\nerrno: " << result.errno_;

  // Do a bare listen syscall. Not bothering to accept connections as that would
  // require another thread.
  ASSERT_EQ(sock.listen(128).return_value_, 0);

  auto client_connect = [](Address::InstanceConstSharedPtr addr_port) {
    // Create a client socket and connect to the server.
    SocketImpl client_sock(Socket::Type::Stream, addr_port, nullptr, {});

    EXPECT_TRUE(client_sock.ioHandle().isOpen()) << addr_port->asString();

    // Instance::socket creates a non-blocking socket, which that extends all the way to the
    // operation of ::connect(), so connect returns with errno==EWOULDBLOCK before the tcp
    // handshake can complete. For testing convenience, re-enable blocking on the socket
    // so that connect will wait for the handshake to complete.
    ASSERT_EQ(client_sock.setBlockingForTest(true).return_value_, 0);

    // Connect to the server.
    const Api::SysCallIntResult result = client_sock.connect(addr_port);
    ASSERT_EQ(result.return_value_, 0)
        << addr_port->asString() << "\nerror: " << errorDetails(result.errno_)
        << "\nerrno: " << result.errno_;
  };

  auto client_addr_port = Network::Utility::parseInternetAddressAndPortNoThrow(
      fmt::format("{}:{}", Network::Test::getLoopbackAddressUrlString(ip_version),
                  addr_port->ip()->port()),
      v6only);
  ASSERT_NE(client_addr_port, nullptr);
  client_connect(client_addr_port);

  if (!v6only) {
    ASSERT_EQ(IpVersion::v6, addr_port->ip()->version());
    auto v4_addr_port = Network::Utility::parseInternetAddressNoThrow(
        Network::Test::getLoopbackAddressUrlString(Network::Address::IpVersion::v4),
        addr_port->ip()->port(), true);
    ASSERT_NE(v4_addr_port, nullptr);
    client_connect(v4_addr_port);
  }
}
} // namespace

class AddressImplSocketTest : public testing::TestWithParam<IpVersion> {};
INSTANTIATE_TEST_SUITE_P(IpVersions, AddressImplSocketTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(AddressImplSocketTest, SocketBindAndConnect) {
  // Test listening on and connecting to an unused port with an IP loopback address.
  testSocketBindAndConnect(GetParam(), true);
}

TEST(Ipv4CompatAddressImplSocktTest, SocketBindAndConnect) {
  if (TestEnvironment::shouldRunTestForIpVersion(Network::Address::IpVersion::v6)) {
    testSocketBindAndConnect(Network::Address::IpVersion::v6, false);
  }
}

TEST(Ipv4InstanceTest, SockaddrToString) {
  // Test addresses from various RFC 5735 reserved ranges
  static const char* addresses[] = {"0.0.0.0",        "0.0.0.255",       "0.0.255.255",
                                    "0.255.255.255",  "192.0.2.0",       "198.151.100.1",
                                    "198.151.100.10", "198.151.100.100", "10.0.0.1",
                                    "10.0.20.1",      "10.3.201.1",      "255.255.255.255"};

  for (const auto address : addresses) {
    sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    EXPECT_EQ(1, inet_pton(AF_INET, address, &addr4.sin_addr));
    addr4.sin_port = 0;
    EXPECT_STREQ(address, Ipv4Instance::sockaddrToString(addr4).c_str());
  }
}

TEST(Ipv4InstanceTest, SocketAddress) {
  sockaddr_in addr4;
  memset(&addr4, 0, sizeof(addr4));
  addr4.sin_family = AF_INET;
  EXPECT_EQ(1, inet_pton(AF_INET, "1.2.3.4", &addr4.sin_addr));
  addr4.sin_port = htons(6502);

  Ipv4Instance address(&addr4);
  EXPECT_EQ("1.2.3.4:6502", address.asString());
  EXPECT_EQ("1.2.3.4:6502", address.asStringView());
  EXPECT_EQ("1.2.3.4:6502", address.logicalName());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("1.2.3.4", address.ip()->addressAsString());
  EXPECT_EQ(6502U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("1.2.3.4"), address));
  EXPECT_EQ(nullptr, address.ip()->ipv6());
  EXPECT_TRUE(address.ip()->isUnicastAddress());
  EXPECT_EQ(nullptr, address.pipe());
  EXPECT_EQ(nullptr, address.envoyInternalAddress());
}

TEST(Ipv4InstanceTest, AddressOnly) {
  Ipv4Instance address("3.4.5.6");
  EXPECT_EQ("3.4.5.6:0", address.asString());
  EXPECT_EQ("3.4.5.6:0", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("3.4.5.6", address.ip()->addressAsString());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("3.4.5.6"), address));
  EXPECT_TRUE(address.ip()->isUnicastAddress());
}

TEST(Ipv4InstanceTest, AddressAndPort) {
  Ipv4Instance address("127.0.0.1", 80);
  EXPECT_EQ("127.0.0.1:80", address.asString());
  EXPECT_EQ("127.0.0.1:80", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("127.0.0.1", address.ip()->addressAsString());
  EXPECT_FALSE(address.ip()->isAnyAddress());
  EXPECT_EQ(80U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("127.0.0.1"), address));
  EXPECT_TRUE(address.ip()->isUnicastAddress());
}

TEST(Ipv4InstanceTest, PortOnly) {
  Ipv4Instance address(443);
  EXPECT_EQ("0.0.0.0:443", address.asString());
  EXPECT_EQ("0.0.0.0:443", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("0.0.0.0", address.ip()->addressAsString());
  EXPECT_TRUE(address.ip()->isAnyAddress());
  EXPECT_EQ(443U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("0.0.0.0"), address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv4InstanceTest, Multicast) {
  Ipv4Instance address("230.0.0.1");
  EXPECT_EQ("230.0.0.1:0", address.asString());
  EXPECT_EQ("230.0.0.1:0", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("230.0.0.1", address.ip()->addressAsString());
  EXPECT_FALSE(address.ip()->isAnyAddress());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("230.0.0.1"), address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv4InstanceTest, Broadcast) {
  Ipv4Instance address("255.255.255.255");
  EXPECT_EQ("255.255.255.255:0", address.asString());
  EXPECT_EQ("255.255.255.255:0", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("255.255.255.255", address.ip()->addressAsString());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v4, address.ip()->version());
  EXPECT_TRUE(
      addressesEqual(Network::Utility::parseInternetAddressNoThrow("255.255.255.255"), address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv4InstanceTest, LinkLocal) {
  // Link-local addresses.
  EXPECT_TRUE(Ipv4Instance("169.254.0.0").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv4Instance("169.254.42.43").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv4Instance("169.254.255.255").ip()->isLinkLocalAddress());

  // Not link-local addresses.
  EXPECT_FALSE(Ipv4Instance("169.255.0.0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv4Instance("169.255.255.255").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv4Instance("170.254.0.0").ip()->isLinkLocalAddress());
}

TEST(Ipv4InstanceTest, Teredo) {
  // Teredo addresses are not applicable to IPv4.
  EXPECT_FALSE(Ipv4Instance("20.1.1.1").ip()->isTeredoAddress());
  EXPECT_FALSE(Ipv4Instance("200.1.1.1").ip()->isTeredoAddress());
}

TEST(Ipv4InstanceTest, SiteLocal) {
  // Site-local addresses are not applicable to IPv4.
  EXPECT_FALSE(Ipv4Instance("1.2.3.4").ip()->isSiteLocalAddress());
  EXPECT_FALSE(Ipv4Instance("200.1.1.1").ip()->isSiteLocalAddress());
}

TEST(Ipv4InstanceTest, UniqueLocal) {
  // Unique Local Addresses (ULA) are not applicable to IPv4.
  EXPECT_FALSE(Ipv4Instance("1.2.3.4").ip()->isUniqueLocalAddress());
  EXPECT_FALSE(Ipv4Instance("200.1.1.1").ip()->isUniqueLocalAddress());
}

TEST(Ipv4InstanceTest, BadAddress) {
  EXPECT_THROW(Ipv4Instance("foo"), EnvoyException);
  EXPECT_THROW(Ipv4Instance("bar", 1), EnvoyException);
}

TEST(Ipv6InstanceTest, SocketAddress) {
  sockaddr_in6 addr6;
  memset(&addr6, 0, sizeof(addr6));
  addr6.sin6_family = AF_INET6;
  EXPECT_EQ(1, inet_pton(AF_INET6, "01:023::00Ef", &addr6.sin6_addr));
  addr6.sin6_port = htons(32000);

  Ipv6Instance address(addr6);
  EXPECT_EQ("[1:23::ef]:32000", address.asString());
  EXPECT_EQ("[1:23::ef]:32000", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("1:23::ef", address.ip()->addressAsString());
  EXPECT_FALSE(address.ip()->isAnyAddress());
  EXPECT_EQ(32000U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(
      addressesEqual(Network::Utility::parseInternetAddressNoThrow("1:0023::0Ef"), address));
  EXPECT_EQ(nullptr, address.ip()->ipv4());
  EXPECT_TRUE(address.ip()->isUnicastAddress());
  EXPECT_EQ(nullptr, address.pipe());
  EXPECT_EQ(nullptr, address.envoyInternalAddress());
}

TEST(Ipv6InstanceTest, AddressOnly) {
  Ipv6Instance address("2001:0db8:85a3:0000:0000:8a2e:0370:7334");
  EXPECT_EQ("[2001:db8:85a3::8a2e:370:7334]:0", address.asString());
  EXPECT_EQ("[2001:db8:85a3::8a2e:370:7334]:0", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("2001:db8:85a3::8a2e:370:7334", address.ip()->addressAsString());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(addressesEqual(
      Network::Utility::parseInternetAddressNoThrow("2001:db8:85a3::8a2e:0370:7334"), address));
  EXPECT_TRUE(address.ip()->isUnicastAddress());
}

TEST(Ipv6InstanceTest, AddressAndPort) {
  Ipv6Instance address("::0001", 80);
  EXPECT_EQ("[::1]:80", address.asString());
  EXPECT_EQ("[::1]:80", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("::1", address.ip()->addressAsString());
  EXPECT_EQ(80U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(
      addressesEqual(Network::Utility::parseInternetAddressNoThrow("0:0:0:0:0:0:0:1"), address));
  EXPECT_TRUE(address.ip()->isUnicastAddress());
}

TEST(Ipv6InstanceTest, ScopeIdStripping) {
  sockaddr_in6 addr6;
  memset(&addr6, 0, sizeof(addr6));
  addr6.sin6_family = AF_INET6;
  EXPECT_EQ(1, inet_pton(AF_INET6, "fe80::f8f3:11ff:fef4:25a8", &addr6.sin6_addr));
  addr6.sin6_port = htons(80);
  addr6.sin6_scope_id = 20u;

  Ipv6Instance address(addr6);
  EXPECT_EQ("[fe80::f8f3:11ff:fef4:25a8%20]:80", address.asString());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_EQ(20U, address.ip()->ipv6()->scopeId());
  auto no_scope_address = address.ip()->ipv6()->addressWithoutScopeId();
  EXPECT_EQ("[fe80::f8f3:11ff:fef4:25a8]:80", no_scope_address->asString());
  EXPECT_EQ(IpVersion::v6, no_scope_address->ip()->version());
  EXPECT_EQ(0U, no_scope_address->ip()->ipv6()->scopeId());
}

TEST(Ipv6InstanceTest, PortOnly) {
  Ipv6Instance address(443);
  EXPECT_EQ("[::]:443", address.asString());
  EXPECT_EQ("[::]:443", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("::", address.ip()->addressAsString());
  EXPECT_TRUE(address.ip()->isAnyAddress());
  EXPECT_EQ(443U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(addressesEqual(Network::Utility::parseInternetAddressNoThrow("::0000"), address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv6InstanceTest, Multicast) {
  Ipv6Instance address("FF00::");
  EXPECT_EQ("[ff00::]:0", address.asString());
  EXPECT_EQ("[ff00::]:0", address.asStringView());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("ff00::", address.ip()->addressAsString());
  EXPECT_FALSE(address.ip()->isAnyAddress());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(addressesEqual(
      Network::Utility::parseInternetAddressNoThrow("FF00:0000:0000:0000:0000:0000:0000:0000"),
      address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv6InstanceTest, Broadcast) {
  Ipv6Instance address("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF");
  EXPECT_EQ("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:0", address.asString());
  EXPECT_EQ(Type::Ip, address.type());
  EXPECT_EQ("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", address.ip()->addressAsString());
  EXPECT_EQ(0U, address.ip()->port());
  EXPECT_EQ(IpVersion::v6, address.ip()->version());
  EXPECT_TRUE(addressesEqual(
      Network::Utility::parseInternetAddressNoThrow("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF"),
      address));
  EXPECT_FALSE(address.ip()->isUnicastAddress());
}

TEST(Ipv6InstanceTest, LinkLocal) {
  // Link-local addresses are in the range "fe80::0" to "febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff".
  EXPECT_TRUE(Ipv6Instance("fe80:0:0:0:0:0:0:0").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe80::0").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe80::1").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe80::42:43").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe80::ffff:ffff:ffff:ffff").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe81::1").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fe90::1").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("febf::0").ip()->isLinkLocalAddress());
  EXPECT_TRUE(Ipv6Instance("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff").ip()->isLinkLocalAddress());

  // Not link-local addresses.
  EXPECT_FALSE(Ipv6Instance("::fe80").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv6Instance("fec0::0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv6Instance("ff00::0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv6Instance("ab80::0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv6Instance("abcd::0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(Ipv6Instance("::ffff").ip()->isLinkLocalAddress());
}

TEST(Ipv6InstanceTest, V4MappedLinkLocal) {
  // Link-local addresses in the range ::ffff:169.254.0.0/16.
  EXPECT_TRUE(v4MappedV6Instance("::ffff:169.254.0.0").ip()->isLinkLocalAddress());
  EXPECT_TRUE(v4MappedV6Instance("::ffff:169.254.42.42").ip()->isLinkLocalAddress());
  EXPECT_TRUE(v4MappedV6Instance("::ffff:169.254.255.255").ip()->isLinkLocalAddress());

  // Not link-local addresses.
  EXPECT_FALSE(v4MappedV6Instance("::ffff:169.255.0.0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(v4MappedV6Instance("::ffff:170.254.0.0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(v4MappedV6Instance("::ffff:0.0.0.0").ip()->isLinkLocalAddress());
  EXPECT_FALSE(v4MappedV6Instance("::ffff:192.168.1.1").ip()->isLinkLocalAddress());
  EXPECT_FALSE(v4MappedV6Instance("::ffff:10.54.1.1").ip()->isLinkLocalAddress());
}

TEST(Ipv6InstanceTest, Teredo) {
  // Teredo addresses are in the range 2001::/32.
  EXPECT_TRUE(Ipv6Instance("2001:0:0:0:0:0:0:0").ip()->isTeredoAddress());
  EXPECT_TRUE(Ipv6Instance("2001::1").ip()->isTeredoAddress());
  EXPECT_TRUE(Ipv6Instance("2001::42:43").ip()->isTeredoAddress());
  EXPECT_TRUE(Ipv6Instance("2001::ffff:ffff:ffff:ffff").ip()->isTeredoAddress());

  // Not Teredo addresses.
  EXPECT_FALSE(Ipv6Instance("2002::0").ip()->isTeredoAddress());
  EXPECT_FALSE(Ipv6Instance("2002::1").ip()->isTeredoAddress());
  EXPECT_FALSE(Ipv6Instance("3001::1").ip()->isTeredoAddress());
}

TEST(Ipv6InstanceTest, UniqueLocal) {
  // Unique Local Addresses (ULA) are in the range fc00::/7.
  EXPECT_TRUE(Ipv6Instance("fc00:0:0:0:0:0:0:0").ip()->isUniqueLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fc00::1").ip()->isUniqueLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fc00::42:43").ip()->isUniqueLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fdff::ffff:ffff:ffff:ffff:ffff:ffff").ip()->isUniqueLocalAddress());

  // Not ULA addresses.
  EXPECT_FALSE(Ipv6Instance("fec0:0:0:0:0:0:0:0").ip()->isUniqueLocalAddress());
  EXPECT_FALSE(
      Ipv6Instance("feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff").ip()->isUniqueLocalAddress());
  EXPECT_FALSE(Ipv6Instance("fe00::0").ip()->isUniqueLocalAddress());
  EXPECT_FALSE(Ipv6Instance("fe80::0").ip()->isUniqueLocalAddress());
  EXPECT_FALSE(Ipv6Instance("ff00::0").ip()->isUniqueLocalAddress());
}

TEST(Ipv6InstanceTest, SiteLocal) {
  // Site-local addresses are in the range fec0::/10.
  EXPECT_TRUE(Ipv6Instance("fec0:0:0:0:0:0:0:0").ip()->isSiteLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fec0::1").ip()->isSiteLocalAddress());
  EXPECT_TRUE(Ipv6Instance("fec0::42:43").ip()->isSiteLocalAddress());
  EXPECT_TRUE(Ipv6Instance("feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff").ip()->isSiteLocalAddress());

  // Not site-local addresses.
  EXPECT_FALSE(Ipv6Instance("fc00:0:0:0:0:0:0:0").ip()->isSiteLocalAddress());
  EXPECT_FALSE(Ipv6Instance("fdff::ffff:ffff:ffff:ffff:ffff:ffff").ip()->isSiteLocalAddress());
  EXPECT_FALSE(Ipv6Instance("ff00::0").ip()->isSiteLocalAddress());
  EXPECT_FALSE(Ipv6Instance("2002::1").ip()->isSiteLocalAddress());
  EXPECT_FALSE(Ipv6Instance("3001::1").ip()->isSiteLocalAddress());
}

TEST(Ipv6InstanceTest, BadAddress) {
  EXPECT_THROW(Ipv6Instance("foo"), EnvoyException);
  EXPECT_THROW(Ipv6Instance("bar", 1), EnvoyException);
}

TEST(PipeInstanceTest, Basic) {
  auto address = THROW_OR_RETURN_VALUE(PipeInstance::create("/foo"), std::unique_ptr<PipeInstance>);
  EXPECT_EQ("/foo", address->asString());
  EXPECT_EQ(Type::Pipe, address->type());
  EXPECT_EQ(nullptr, address->ip());
  EXPECT_EQ(nullptr, address->envoyInternalAddress());
}

TEST(InternalInstanceTest, Basic) {
  EnvoyInternalInstance address("listener_foo");
  EXPECT_EQ("envoy://listener_foo/", address.asString());
  EXPECT_EQ(Type::EnvoyInternal, address.type());
  EXPECT_EQ(nullptr, address.ip());
  EXPECT_EQ(nullptr, address.pipe());
  EXPECT_NE(nullptr, address.envoyInternalAddress());
  EXPECT_EQ(nullptr, address.sockAddr());
  EXPECT_EQ(static_cast<decltype(address.sockAddrLen())>(0), address.sockAddrLen());
}

TEST(InternalInstanceTest, BasicWithId) {
  EnvoyInternalInstance address("listener_foo", "endpoint_bar");
  EXPECT_EQ("envoy://listener_foo/endpoint_bar", address.asString());
}

// Excluding Windows; chmod(2) against Windows AF_UNIX socket files succeeds,
// but stat(2) against those returns ENOENT.
#ifndef WIN32
TEST(PipeInstanceTest, BasicPermission) {
  std::string path = TestEnvironment::unixDomainSocketPath("foo.sock");

  const mode_t mode = 0777;
  std::shared_ptr<PipeInstance> address =
      THROW_OR_RETURN_VALUE(PipeInstance::create(path, mode), std::unique_ptr<PipeInstance>);
  SocketImpl sock(Socket::Type::Stream, address, nullptr, {});

  EXPECT_TRUE(sock.ioHandle().isOpen()) << address->asString();

  Api::SysCallIntResult result = sock.bind(address);
  ASSERT_EQ(result.return_value_, 0)
      << address->asString() << "\nerror: " << errorDetails(result.errno_)
      << "\terrno: " << result.errno_;

  Api::OsSysCalls& os_sys_calls = Api::OsSysCallsSingleton::get();
  struct stat stat_buf;
  result = os_sys_calls.stat(path.c_str(), &stat_buf);
  EXPECT_EQ(result.return_value_, 0);
  // Get file permissions bits
  ASSERT_EQ(stat_buf.st_mode & 07777, mode)
      << path << std::oct << "\t" << (stat_buf.st_mode & 07777) << std::dec << "\t"
      << (stat_buf.st_mode) << errorDetails(result.errno_);
}
#endif

TEST(PipeInstanceTest, PermissionFail) {
  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);
  std::string path = TestEnvironment::unixDomainSocketPath("foo.sock");

  const mode_t mode = 0777;
  InstanceConstSharedPtr address =
      THROW_OR_RETURN_VALUE(PipeInstance::create(path, mode), std::unique_ptr<PipeInstance>);
  SocketImpl sock(Socket::Type::Stream, address, nullptr, {});

  EXPECT_TRUE(sock.ioHandle().isOpen()) << address->asString();

  EXPECT_CALL(os_sys_calls, bind(_, _, _)).WillOnce(Return(Api::SysCallIntResult{0, 0}));
  EXPECT_CALL(os_sys_calls, chmod(_, _)).WillOnce(Return(Api::SysCallIntResult{-1, 0}));
  EXPECT_NE(sock.bind(address).return_value_, 0);
}

TEST(PipeInstanceTest, AbstractNamespacePermission) {
#if defined(__linux__)
  std::string path = "@/foo";
  const mode_t mode = 0777;
  EXPECT_THAT(PipeInstance::create(path, mode).status().message(),
              testing::ContainsRegex("Cannot set mode for Abstract AF_UNIX sockets"));

  sockaddr_un sun;
  sun.sun_family = AF_UNIX;
  StringUtil::strlcpy(&sun.sun_path[1], path.data(), path.size());
  sun.sun_path[0] = '\0';
  socklen_t ss_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sun.sun_path);

  EXPECT_THAT(PipeInstance::create(&sun, ss_len, mode).status().message(),
              testing::ContainsRegex("Cannot set mode for Abstract AF_UNIX sockets"));
#endif
}

TEST(PipeInstanceTest, AbstractNamespace) {
#if defined(__linux__)
  auto address =
      THROW_OR_RETURN_VALUE(PipeInstance::create("@/foo"), std::unique_ptr<PipeInstance>);
  EXPECT_EQ("@/foo", address->asString());
  EXPECT_EQ("@/foo", address->asStringView());
  EXPECT_EQ(Type::Pipe, address->type());
  EXPECT_EQ(nullptr, address->ip());
#else
  EXPECT_FALSE(PipeInstance::create("@/foo").status().ok());
#endif
}

TEST(PipeInstanceTest, BadAddress) {
  std::string long_address(1000, 'X');
  EXPECT_THAT(PipeInstance::create(long_address).status().message(),
              testing::ContainsRegex("exceeds maximum UNIX domain socket path size"));
}

// Validate that embedded nulls in abstract socket addresses are included and represented with '@'.
TEST(PipeInstanceTest, EmbeddedNullAbstractNamespace) {
  std::string embedded_null("@/foo/bar");
  embedded_null[5] = '\0'; // Set embedded null.
#if defined(__linux__)
  auto address =
      THROW_OR_RETURN_VALUE(PipeInstance::create(embedded_null), std::unique_ptr<PipeInstance>);
  EXPECT_EQ("@/foo@bar", address->asString());
  EXPECT_EQ("@/foo@bar", address->asStringView());
  EXPECT_EQ(Type::Pipe, address->type());
  EXPECT_EQ(nullptr, address->ip());
#else
  EXPECT_FALSE(PipeInstance::create(embedded_null).status().ok());
#endif
}

// Reject embedded nulls in filesystem pathname addresses.
TEST(PipeInstanceTest, EmbeddedNullPathError) {
  std::string embedded_null("/foo/bar");
  embedded_null[4] = '\0'; // Set embedded null.
  EXPECT_THAT(PipeInstance::create(embedded_null).status().message(),
              testing::ContainsRegex("contains embedded null characters"));
}

TEST(PipeInstanceTest, UnlinksExistingFile) {
  const auto bind_uds_socket = [](const std::string& path) {
    std::shared_ptr<PipeInstance> address =
        THROW_OR_RETURN_VALUE(PipeInstance::create(path), std::unique_ptr<PipeInstance>);
    SocketImpl sock(Socket::Type::Stream, address, nullptr, {});

    EXPECT_TRUE(sock.ioHandle().isOpen()) << address->asString();

    const Api::SysCallIntResult result = sock.bind(address);

    ASSERT_EQ(result.return_value_, 0)
        << address->asString() << "\nerror: " << errorDetails(result.errno_)
        << "\nerrno: " << result.errno_;
  };

  const std::string path = TestEnvironment::unixDomainSocketPath("UnlinksExistingFile.sock");
  bind_uds_socket(path);
  bind_uds_socket(path); // after closing, second bind to the same path should succeed.
}

TEST(AddressFromSockAddrDeathTest, IPv4) {
  sockaddr_storage ss;
  memset(&ss, 0, sizeof(ss));
  auto& sin = reinterpret_cast<sockaddr_in&>(ss);

  sin.sin_family = AF_INET;
  EXPECT_EQ(1, inet_pton(AF_INET, "1.2.3.4", &sin.sin_addr));
  sin.sin_port = htons(6502);

  EXPECT_DEATH(*addressFromSockAddr(ss, 1), "ss_len");
  EXPECT_DEATH(*addressFromSockAddr(ss, sizeof(sockaddr_in) - 1), "ss_len");
  EXPECT_DEATH(*addressFromSockAddr(ss, sizeof(sockaddr_in) + 1), "ss_len");

  EXPECT_EQ("1.2.3.4:6502", (*addressFromSockAddr(ss, sizeof(sockaddr_in)))->asString());

  // Invalid family.
  sin.sin_family = AF_UNSPEC;
  EXPECT_FALSE(addressFromSockAddr(ss, sizeof(sockaddr_in)).ok());
}

TEST(AddressFromSockAddrDeathTest, IPv6) {
  sockaddr_storage ss;
  memset(&ss, 0, sizeof(ss));
  auto& sin6 = reinterpret_cast<sockaddr_in6&>(ss);

  sin6.sin6_family = AF_INET6;
  EXPECT_EQ(1, inet_pton(AF_INET6, "01:023::00Ef", &sin6.sin6_addr));
  sin6.sin6_port = htons(32000);

  EXPECT_DEATH(*addressFromSockAddr(ss, 1), "ss_len");
  EXPECT_DEATH(*addressFromSockAddr(ss, sizeof(sockaddr_in6) - 1), "ss_len");
  EXPECT_DEATH(*addressFromSockAddr(ss, sizeof(sockaddr_in6) + 1), "ss_len");

  EXPECT_EQ("[1:23::ef]:32000", (*addressFromSockAddr(ss, sizeof(sockaddr_in6)))->asString());

  // Test that IPv4-mapped IPv6 address is returned as an Ipv4Instance when 'v6only' parameter is
  // 'false', but not otherwise.
  EXPECT_EQ(1, inet_pton(AF_INET6, "::ffff:192.0.2.128", &sin6.sin6_addr));
  EXPECT_EQ(IpVersion::v4,
            (*addressFromSockAddr(ss, sizeof(sockaddr_in6), false))->ip()->version());
  EXPECT_EQ("192.0.2.128:32000",
            (*addressFromSockAddr(ss, sizeof(sockaddr_in6), false))->asString());
  EXPECT_EQ(IpVersion::v6, (*addressFromSockAddr(ss, sizeof(sockaddr_in6), true))->ip()->version());
  EXPECT_EQ("[::ffff:192.0.2.128]:32000",
            (*addressFromSockAddr(ss, sizeof(sockaddr_in6), true))->asString());

// Verify that when forceV6() is true, IPv4-mapped IPv6 address will be converted even when |v6only|
// is true
#if defined(__APPLE__) || defined(__ANDROID_API__)
  Runtime::maybeSetRuntimeGuard("envoy.reloadable_features.always_use_v6", true);
  EXPECT_EQ(IpVersion::v4, (*addressFromSockAddr(ss, sizeof(sockaddr_in6), true))->ip()->version());
  EXPECT_EQ("192.0.2.128:32000",
            (*addressFromSockAddr(ss, sizeof(sockaddr_in6), true))->asString());
#endif
}

TEST(AddressFromSockAddrDeathTest, Pipe) {
  sockaddr_storage ss;
  memset(&ss, 0, sizeof(ss));
  auto& sun = reinterpret_cast<sockaddr_un&>(ss);
  sun.sun_family = AF_UNIX;

  StringUtil::strlcpy(sun.sun_path, "/some/path", sizeof sun.sun_path);

  EXPECT_DEATH(*addressFromSockAddr(ss, 1), "ss_len");
  EXPECT_DEATH(*addressFromSockAddr(ss, offsetof(struct sockaddr_un, sun_path)), "ss_len");

  socklen_t ss_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sun.sun_path);
  EXPECT_EQ("/some/path", (*addressFromSockAddr(ss, ss_len))->asString());

  // Abstract socket namespace.
  StringUtil::strlcpy(&sun.sun_path[1], "/some/abstract/path", sizeof sun.sun_path);
  sun.sun_path[0] = '\0';
  ss_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen("/some/abstract/path");
#if defined(__linux__)
  EXPECT_EQ("@/some/abstract/path", (*addressFromSockAddr(ss, ss_len))->asString());
#else
  EXPECT_FALSE(addressFromSockAddr(ss, ss_len).ok());
#endif
}

// Test comparisons between all the different (known) test classes.
struct TestCase {
  enum InstanceType { Ipv4, Ipv6, Pipe, Internal };

  TestCase() = default;
  TestCase(enum InstanceType type, const std::string& address, uint32_t port)
      : address_(address), type_(type), port_(port) {}
  TestCase(const TestCase& rhs) = default;

  bool operator==(const TestCase& rhs) {
    return (type_ == rhs.type_ && address_ == rhs.address_ && port_ == rhs.port_);
  }

  std::string address_;
  enum InstanceType type_ { Ipv4 };
  uint32_t port_ = 0; // Ignored for Pipe
};

class MixedAddressTest : public testing::TestWithParam<::testing::tuple<TestCase, TestCase>> {
public:
protected:
  InstanceConstSharedPtr testCaseToInstance(const struct TestCase& test_case) {
    // Catch default construction.
    if (test_case.address_.empty()) {
      return nullptr;
    }
    switch (test_case.type_) {
    case TestCase::Ipv4:
      return std::make_shared<Ipv4Instance>(test_case.address_, test_case.port_);
      break;
    case TestCase::Ipv6:
      return std::make_shared<Ipv6Instance>(test_case.address_, test_case.port_);
      break;
    case TestCase::Pipe:
      return THROW_OR_RETURN_VALUE(PipeInstance::create(test_case.address_),
                                   std::unique_ptr<PipeInstance>);
      break;
    case TestCase::Internal:
      return std::make_shared<EnvoyInternalInstance>(test_case.address_);
      break;
    }
    return nullptr;
  }
};

TEST_P(MixedAddressTest, Equality) {
  TestCase lhs_case = ::testing::get<0>(GetParam());
  const TestCase& rhs_case = ::testing::get<1>(GetParam());
  InstanceConstSharedPtr lhs = testCaseToInstance(lhs_case);
  InstanceConstSharedPtr rhs = testCaseToInstance(rhs_case);
  if (lhs_case == rhs_case) {
    EXPECT_EQ(*lhs, *rhs) << lhs->asString() << " != " << rhs->asString();
  } else {
    EXPECT_NE(*lhs, *rhs) << lhs->asString() << " == " << rhs->asString();
  }
}

struct TestCase test_cases[] = {
    {TestCase::Ipv4, "1.2.3.4", 1},          {TestCase::Ipv4, "1.2.3.4", 2},
    {TestCase::Ipv4, "1.2.3.5", 1},          {TestCase::Ipv6, "01:023::00ef", 1},
    {TestCase::Ipv6, "01:023::00ef", 2},     {TestCase::Ipv6, "01:023::00ed", 1},
    {TestCase::Pipe, "/path/to/pipe/1", 0},  {TestCase::Pipe, "/path/to/pipe/2", 0},
    {TestCase::Internal, "listener_foo", 0}, {TestCase::Internal, "listener_bar", 0}};

INSTANTIATE_TEST_SUITE_P(AddressCrossProduct, MixedAddressTest,
                         ::testing::Combine(::testing::ValuesIn(test_cases),
                                            ::testing::ValuesIn(test_cases)));
} // namespace Address
} // namespace Network
} // namespace Envoy
