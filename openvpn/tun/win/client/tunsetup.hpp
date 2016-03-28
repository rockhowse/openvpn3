//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2015 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// Client tun setup for Windows

#ifndef OPENVPN_TUN_WIN_CLIENT_TUNSETUP_H
#define OPENVPN_TUN_WIN_CLIENT_TUNSETUP_H

#include <string>
#include <sstream>
#include <ostream>
#include <utility>

#include <openvpn/common/exception.hpp>
#include <openvpn/common/rc.hpp>
#include <openvpn/common/string.hpp>
#include <openvpn/common/size.hpp>
#include <openvpn/common/arraysize.hpp>
#include <openvpn/error/excode.hpp>
#include <openvpn/win/scoped_handle.hpp>
#include <openvpn/win/cmd.hpp>
#include <openvpn/tun/win/tunutil.hpp>
#include <openvpn/tun/win/client/setupbase.hpp>

#if _WIN32_WINNT >= 0x0600 // Vista+
#include <openvpn/tun/win/nrpt.hpp>
#include <openvpn/tun/win/wfp.hpp>
#endif

#include <versionhelpers.h>

namespace openvpn {
  namespace TunWin {
    class Setup : public SetupBase
    {
    public:
      typedef RCPtr<Setup> Ptr;

      virtual HANDLE establish(const TunBuilderCapture& pull,
			       const std::wstring& openvpn_app_path,
			       Stop* stop,
			       std::ostream& os) override // defined by SetupBase
      {
	// close out old remove cmds, if they exist
	destroy(os);

	// enumerate available TAP adapters
	Util::TapNameGuidPairList guids;
	os << "TAP ADAPTERS:" << std::endl << guids.to_string() << std::endl;

	// open TAP device handle
	std::string path_opened;
	Util::TapNameGuidPair tap;
	Win::ScopedHANDLE th(Util::tap_open(guids, path_opened, tap));
	const std::string msg = "Open TAP device \"" + tap.name + "\" PATH=\"" + path_opened + '\"';

	if (!th.defined())
	  {
	    os << msg << " FAILED" << std::endl;
	    throw ErrorCode(Error::TUN_IFACE_CREATE, true, "cannot acquire TAP handle");
	  }

	os << msg << " SUCCEEDED" << std::endl;
	Util::TAPDriverVersion version(th());
	os << version.to_string() << std::endl;

	// create ActionLists for setting up and removing adapter properties
	ActionList::Ptr add_cmds(new ActionList());
	remove_cmds.reset(new ActionList());

	// populate add/remove lists with actions
	adapter_config(th(), openvpn_app_path, tap, pull, *add_cmds, *remove_cmds, os);

	// execute the add actions
	add_cmds->execute(os);

	// now that the add actions have succeeded,
	// enable the remove actions
	remove_cmds->enable_destroy(true);

	return th.release();
      }

      virtual void destroy(std::ostream& os) override // defined by DestructorBase
      {
	if (remove_cmds)
	  {
	    remove_cmds->destroy(os);
	    remove_cmds.reset();
	  }
      }

      virtual ~Setup()
      {
	std::ostringstream os;
	destroy(os);
      }

    private:
#if _WIN32_WINNT >= 0x0600
      // Configure TAP adapter on Vista and higher
      void adapter_config(HANDLE th,
			  const std::wstring& openvpn_app_path,
			  const Util::TapNameGuidPair& tap,
			  const TunBuilderCapture& pull,
			  ActionList& create,
			  ActionList& destroy,
			  std::ostream& os)
      {
	// Windows interface index
	const std::string tap_index_name = tap.index_or_name();

	// special IPv6 next-hop recognized by TAP driver (magic)
	const std::string ipv6_next_hop = "fe80::8";

	// get default gateway
	const Util::DefaultGateway gw;

	// set local4 and local6 to point to IPv4/6 route configurations
	const TunBuilderCapture::RouteAddress* local4 = pull.vpn_ipv4();
	const TunBuilderCapture::RouteAddress* local6 = pull.vpn_ipv6();

	// set TAP media status to CONNECTED
	Util::tap_set_media_status(th, true);

	// try to delete any stale routes on interface left over from previous session
	create.add(new Util::ActionDeleteAllRoutesOnInterface(tap.index));

	// Set IPv4 Interface
	//
	// Usage: set address [name=]<string>
	//  [[source=]dhcp|static]
	//  [[address=]<IPv4 address>[/<integer>] [[mask=]<IPv4 mask>]
	//  [[gateway=]<IPv4 address>|none [gwmetric=]<integer>]
	//  [[type=]unicast|anycast]
	//  [[subinterface=]<string>]
	//  [[store=]active|persistent]
	// Usage: delete address [name=]<string> [[address=]<IPv4 address>]
	//  [[gateway=]<IPv4 address>|all]
	//  [[store=]active|persistent]
	if (local4)
	  {
	    // Process ifconfig and topology
	    const std::string netmask = IPv4::Addr::netmask_from_prefix_len(local4->prefix_length).to_string();
	    const IP::Addr localaddr = IP::Addr::from_string(local4->address);
	    if (local4->net30)
	      Util::tap_configure_topology_net30(th, localaddr, local4->prefix_length);
	    else
	      Util::tap_configure_topology_subnet(th, localaddr, local4->prefix_length);
	    create.add(new WinCmd("netsh interface ip set address " + tap_index_name + " static " + local4->address + ' ' + netmask + " gateway=" + local4->gateway + " store=active"));
	    destroy.add(new WinCmd("netsh interface ip delete address " + tap_index_name + ' ' + local4->address + " gateway=all store=active"));
	  }

	// Should we block IPv6?
	if (pull.block_ipv6)
	  {
	    static const char *const block_ipv6_net[] = {
	      "2000::/4",
	      "3000::/4",
	      "fc00::/7",
	    };
	    for (size_t i = 0; i < array_size(block_ipv6_net); ++i)
	      {
		create.add(new WinCmd("netsh interface ipv6 add route " + std::string(block_ipv6_net[i]) + " interface=1 store=active"));
		destroy.add(new WinCmd("netsh interface ipv6 delete route " + std::string(block_ipv6_net[i]) + " interface=1 store=active"));
	      }
	  }

	// Set IPv6 Interface
	//
	// Usage: set address [interface=]<string> [address=]<IPv6 address>
	//  [[type=]unicast|anycast]
	//  [[validlifetime=]<integer>|infinite]
	//  [[preferredlifetime=]<integer>|infinite]
	//  [[store=]active|persistent]
	//Usage: delete address [interface=]<string> [address=]<IPv6 address>
	//  [[store=]active|persistent]
	if (local6 && !pull.block_ipv6)
	  {
	    create.add(new WinCmd("netsh interface ipv6 set address " + tap_index_name + ' ' + local6->address + " store=active"));
	    destroy.add(new WinCmd("netsh interface ipv6 delete address " + tap_index_name + ' ' + local6->address + " store=active"));

	    create.add(new WinCmd("netsh interface ipv6 add route " + local6->gateway + '/' + to_string(local6->prefix_length) + ' ' + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	    destroy.add(new WinCmd("netsh interface ipv6 delete route " + local6->gateway + '/' + to_string(local6->prefix_length) + ' ' + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	  }

	// Process Routes
	//
	// Usage: add route [prefix=]<IPv4 address>/<integer> [interface=]<string>
	//  [[nexthop=]<IPv4 address>] [[siteprefixlength=]<integer>]
	//  [[metric=]<integer>] [[publish=]no|age|yes]
	//  [[validlifetime=]<integer>|infinite]
	//  [[preferredlifetime=]<integer>|infinite]
	//  [[store=]active|persistent]
	// Usage: delete route [prefix=]<IPv4 address>/<integer> [interface=]<string>
	//  [[nexthop=]<IPv4 address>]
	//  [[store=]active|persistent]
	//
	// Usage: add route [prefix=]<IPv6 address>/<integer> [interface=]<string>
	//  [[nexthop=]<IPv6 address>] [[siteprefixlength=]<integer>]
	//  [[metric=]<integer>] [[publish=]no|age|yes]
	//  [[validlifetime=]<integer>|infinite]
	//  [[preferredlifetime=]<integer>|infinite]
	//  [[store=]active|persistent]
	// Usage: delete route [prefix=]<IPv6 address>/<integer> [interface=]<string>
	//  [[nexthop=]<IPv6 address>]
	//  [[store=]active|persistent]
	{
	  for (auto &route : pull.add_routes)
	    {
	      if (route.ipv6)
		{
		  if (!pull.block_ipv6)
		    {
		      create.add(new WinCmd("netsh interface ipv6 add route " + route.address + '/' + to_string(route.prefix_length) + ' ' + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
		      destroy.add(new WinCmd("netsh interface ipv6 delete route " + route.address + '/' + to_string(route.prefix_length) + ' ' + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
		    }
		}
	      else
		{
		  if (local4)
		    {
		      create.add(new WinCmd("netsh interface ip add route " + route.address + '/' + to_string(route.prefix_length) + ' ' + tap_index_name + ' ' + local4->gateway + " store=active"));
		      destroy.add(new WinCmd("netsh interface ip delete route " + route.address + '/' + to_string(route.prefix_length) + ' ' + tap_index_name + ' ' + local4->gateway + " store=active"));
		    }
		  else
		    throw tun_win_setup("IPv4 routes pushed without IPv4 ifconfig");
		}
	    }
	}

	// Process exclude routes
	if (!pull.exclude_routes.empty())
	  {
	    if (gw.defined())
	      {
		bool ipv6_error = false;
		for (std::vector<TunBuilderCapture::Route>::const_iterator i = pull.exclude_routes.begin(); i != pull.exclude_routes.end(); ++i)
		  {
		    const TunBuilderCapture::Route& route = *i;
		    if (route.ipv6)
		      {
			ipv6_error = true;
		      }
		    else
		      {
			create.add(new WinCmd("netsh interface ip add route " + route.address + '/' + to_string(route.prefix_length) + ' ' + to_string(gw.interface_index()) + ' ' + gw.gateway_address() + " store=active"));
			destroy.add(new WinCmd("netsh interface ip delete route " + route.address + '/' + to_string(route.prefix_length) + ' ' + to_string(gw.interface_index()) + ' ' + gw.gateway_address() + " store=active"));
		      }
		  }
		if (ipv6_error)
		  os << "NOTE: exclude IPv6 routes not currently supported" << std::endl;
	      }
	    else
	      os << "NOTE: exclude routes error: cannot detect default gateway" << std::endl;
	  }

	// Process IPv4 redirect-gateway
	if (pull.reroute_gw.ipv4)
	  {
	    // add server bypass route
	    if (gw.defined())
	      {
		if (!pull.remote_address.ipv6)
		  {
		    create.add(new WinCmd("netsh interface ip add route " + pull.remote_address.address + "/32 " + to_string(gw.interface_index()) + ' ' + gw.gateway_address() + " store=active"));
		    destroy.add(new WinCmd("netsh interface ip delete route " + pull.remote_address.address + "/32 " + to_string(gw.interface_index()) + ' ' + gw.gateway_address() + " store=active"));
		  }
	      }
	    else
	      throw tun_win_setup("redirect-gateway error: cannot detect default gateway");

	    create.add(new WinCmd("netsh interface ip add route 0.0.0.0/1 " + tap_index_name + ' ' + local4->gateway + " store=active"));
	    create.add(new WinCmd("netsh interface ip add route 128.0.0.0/1 " + tap_index_name + ' ' + local4->gateway + " store=active"));
	    destroy.add(new WinCmd("netsh interface ip delete route 0.0.0.0/1 " + tap_index_name + ' ' + local4->gateway + " store=active"));
	    destroy.add(new WinCmd("netsh interface ip delete route 128.0.0.0/1 " + tap_index_name + ' ' + local4->gateway + " store=active"));
	  }

	// Process IPv6 redirect-gateway
	if (pull.reroute_gw.ipv6 && !pull.block_ipv6)
	  {
	    create.add(new WinCmd("netsh interface ipv6 add route 0::/1 " + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	    create.add(new WinCmd("netsh interface ipv6 add route 8000::/1 " + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	    destroy.add(new WinCmd("netsh interface ipv6 delete route 0::/1 " + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	    destroy.add(new WinCmd("netsh interface ipv6 delete route 8000::/1 " + tap_index_name + ' ' + ipv6_next_hop + " store=active"));
	  }

	// Process DNS Servers
	//
	// Usage: set dnsservers [name=]<string> [source=]dhcp|static
	//  [[address=]<IP address>|none]
	//  [[register=]none|primary|both]
	//  [[validate=]yes|no]
	// Usage: add dnsservers [name=]<string> [address=]<IPv4 address>
	//  [[index=]<integer>] [[validate=]yes|no]
	// Usage: delete dnsservers [name=]<string> [[address=]<IP address>|all] [[validate=]yes|no]
	//
	// Usage: set dnsservers [name=]<string> [source=]dhcp|static
	//  [[address=]<IPv6 address>|none]
	//  [[register=]none|primary|both]
	//  [[validate=]yes|no]
	// Usage: add dnsservers [name=]<string> [address=]<IPv6 address>
	//  [[index=]<integer>] [[validate=]yes|no]
	// Usage: delete dnsservers [name=]<string> [[address=]<IPv6 address>|all] [[validate=]yes|no]
	{
	  // fix for vista and dnsserver vs win7+ dnsservers
	  std::string dns_servers_cmd = "dnsservers";
	  std::string validate_cmd = " validate=no";
	  if (IsWindowsVistaOrGreater() && !IsWindows7OrGreater()) {
	    dns_servers_cmd = "dnsserver";
	    validate_cmd = "";
	  }

#if 1
	  // normal production setting
	  const bool use_nrpt = IsWindows8OrGreater();
	  const bool add_netsh_rules = true;
#else
	  // test NRPT registry settings on pre-Win8
	  const bool use_nrpt = true;
	  const bool add_netsh_rules = true;
#endif
	  // per-protocol indices
	  constexpr size_t IPv4 = 0;
	  constexpr size_t IPv6 = 1;
	  int indices[2] = {0, 0}; // DNS server counters for IPv4/IPv6

	  // iterate over pushed DNS server list
	  for (size_t i = 0; i < pull.dns_servers.size(); ++i)
	    {
	      const TunBuilderCapture::DNSServer& ds = pull.dns_servers[i];
	      if (ds.ipv6 && pull.block_ipv6)
		continue;
	      const std::string proto = ds.ipv6 ? "ipv6" : "ip";
	      const int idx = indices[bool(ds.ipv6)]++;
	      if (add_netsh_rules)
		{
		  if (idx)
		    create.add(new WinCmd("netsh interface " + proto + " add " + dns_servers_cmd + " " + tap_index_name + ' ' + ds.address + " " + to_string(idx+1) + validate_cmd));
		  else
		    {
		      create.add(new WinCmd("netsh interface " + proto + " set " + dns_servers_cmd + " " + tap_index_name + " static " + ds.address + " register=primary" + validate_cmd));
		      destroy.add(new WinCmd("netsh interface " + proto + " delete " + dns_servers_cmd + " " + tap_index_name + " all" + validate_cmd));
		    }
		}
	    }

	  // If NRPT enabled and at least one IPv4 or IPv6 DNS
	  // server was added, add NRPT registry entries to
	  // route DNS through the tunnel.
	  // Also consider selective DNS routing using domain
	  // suffix list from pull.search_domains as set by
	  // "dhcp-option DOMAIN ..." directives.
	  if (use_nrpt && (indices[IPv4] || indices[IPv6]))
	    {
	      // domain suffix list
	      std::vector<std::string> dsfx;

	      // Only add DNS routing suffixes if not rerouting gateway.
	      // Otherwise, route all DNS requests with wildcard (".").
	      const bool redir_dns4 = pull.reroute_gw.ipv4 && indices[IPv4];
	      const bool redir_dns6 = pull.reroute_gw.ipv6 && indices[IPv6];
	      if (!redir_dns4 && !redir_dns6)
		{
		  for (const auto &sd : pull.search_domains)
		    {
		      std::string dom = sd.domain;
		      if (!dom.empty())
			{
			  // each DNS suffix must begin with '.'
			  if (dom[0] != '.')
			    dom = "." + dom;
			  dsfx.push_back(std::move(dom));
			}
		    }
		}
	      if (dsfx.empty())
		dsfx.emplace_back(".");

	      // DNS server list
	      std::vector<std::string> dserv;
	      for (const auto &ds : pull.dns_servers)
		dserv.push_back(ds.address);

	      create.add(new NRPT::ActionCreate(dsfx, dserv));
	      destroy.add(new NRPT::ActionDelete);
	    }

#if 1
	  // Use WFP for DNS leak protection.

	  // If we added DNS servers, block DNS on all interfaces except
	  // the TAP adapter.
	  if (IsWindows8OrGreater() && !openvpn_app_path.empty() && (indices[0] || indices[1]))
	    {
	      create.add(new ActionWFP(openvpn_app_path, tap.index, true, wfp));
	      destroy.add(new ActionWFP(openvpn_app_path, tap.index, false, wfp));
	    }
#endif
	}

#if 0
	// Set a default TAP-adapter domain suffix using the first
	// domain suffix from from pull.search_domains as set by
	// "dhcp-option DOMAIN ..." directives.
	if (!pull.search_domains.empty())
	  {
	    // Only the first search domain is used
	    create.add(new Util::ActionSetSearchDomain(pull.search_domains[0].domain, tap.guid));
	    destroy.add(new Util::ActionSetSearchDomain("", tap.guid));
	  }
#endif

	// Process WINS Servers
	//
	// Usage: set winsservers [name=]<string> [source=]dhcp|static
	//  [[address=]<IP address>|none]
	// Usage: add winsservers [name=]<string> [address=]<IP address> [[index=]<integer>]
	// Usage: delete winsservers [name=]<string> [[address=]<IP address>|all]
	{
	  for (size_t i = 0; i < pull.wins_servers.size(); ++i)
	    {
	      const TunBuilderCapture::WINSServer& ws = pull.wins_servers[i];
	      if (i)
		create.add(new WinCmd("netsh interface ip add winsservers " + tap_index_name + ' ' + ws.address + ' ' + to_string(i+1)));
	      else
		{
		  create.add(new WinCmd("netsh interface ip set winsservers " + tap_index_name + " static " + ws.address));
		  destroy.add(new WinCmd("netsh interface ip delete winsservers " + tap_index_name + " all"));
		}
	    }
	}

	// flush DNS cache
	create.add(new WinCmd("ipconfig /flushdns"));
	destroy.add(new WinCmd("ipconfig /flushdns"));
      }
#else
      // Configure TAP adapter for pre-Vista
      // Currently we don't support IPv6 on pre-Vista
      void adapter_config(HANDLE th,
			  const std::wstring& openvpn_app_path,
			  const Util::TapNameGuidPair& tap,
			  const TunBuilderCapture& pull,
			  ActionList& create,
			  ActionList& destroy,
			  std::ostream& os)
      {
	// Windows interface index
	const std::string tap_index_name = tap.index_or_name();

	// get default gateway
	const Util::DefaultGateway gw;

	// set local4 to point to IPv4 route configurations
	const TunBuilderCapture::RouteAddress* local4 = pull.vpn_ipv4();

	// Make sure the TAP adapter is set for DHCP
	{
	  const Util::IPAdaptersInfo ai;
	  if (!ai.is_dhcp_enabled(tap.index))
	    {
	      os << "TAP: DHCP is disabled, attempting to enable" << std::endl;
	      ActionList::Ptr cmds(new ActionList());
	      cmds->add(new Util::ActionEnableDHCP(tap));
	      cmds->execute(os);
	    }
	}

	// Set IPv4 Interface
	if (local4)
	  {
	    // Process ifconfig and topology
	    const std::string netmask = IPv4::Addr::netmask_from_prefix_len(local4->prefix_length).to_string();
	    const IP::Addr localaddr = IP::Addr::from_string(local4->address);
	    if (local4->net30)
	      Util::tap_configure_topology_net30(th, localaddr, local4->prefix_length);
	    else
	      Util::tap_configure_topology_subnet(th, localaddr, local4->prefix_length);
	  }

	// On pre-Vista, set up TAP adapter DHCP masquerade for
	// configuring adapter properties.
	{
	  os << "TAP: configure DHCP masquerade" << std::endl;
	  Util::TAPDHCPMasquerade dhmasq;
	  dhmasq.init_from_capture(pull);
	  dhmasq.ioctl(th);
	}

	// set TAP media status to CONNECTED
	Util::tap_set_media_status(th, true);

	// ARP
	Util::flush_arp(tap.index, os);

	// DHCP release/renew
	{
	  const Util::InterfaceInfoList ii;
	  Util::dhcp_release(ii, tap.index, os);
	  Util::dhcp_renew(ii, tap.index, os);
	}

	// Wait for TAP adapter to come up
	{
	  bool succeed = false;
	  const Util::IPNetmask4 vpn_addr(pull, "VPN IP");
	  for (int i = 1; i <= 30; ++i)
	    {
	      os << '[' << i << "] waiting for TAP adapter to receive DHCP settings..." << std::endl;
	      const Util::IPAdaptersInfo ai;
	      if (ai.is_up(tap.index, vpn_addr))
		{
		  succeed = true;
		  break;
		}
	      ::Sleep(1000);
	    }
	  if (!succeed)
	    throw tun_win_setup("TAP adapter DHCP handshake failed");
	}

	// Process routes
	os << "Sleeping 5 seconds prior to adding routes..." << std::endl;
	::Sleep(5000);
	for (auto &route : pull.add_routes)
	  {
	    if (!route.ipv6)
	      {
		if (local4)
		  {
		    const std::string netmask = IPv4::Addr::netmask_from_prefix_len(route.prefix_length).to_string();
		    create.add(new WinCmd("route ADD " + route.address + " MASK " + netmask + ' ' + local4->gateway));
		    destroy.add(new WinCmd("route DELETE " + route.address + " MASK " + netmask + ' ' + local4->gateway));
		  }
		else
		  throw tun_win_setup("IPv4 routes pushed without IPv4 ifconfig");
	      }
	  }

	// Process exclude routes
	if (!pull.exclude_routes.empty())
	  {
	    if (gw.defined())
	      {
		for (auto &route : pull.exclude_routes)
		  {
		    if (!route.ipv6)
		      {
			const std::string netmask = IPv4::Addr::netmask_from_prefix_len(route.prefix_length).to_string();
			create.add(new WinCmd("route ADD " + route.address + " MASK " + netmask + ' ' + gw.gateway_address()));
			destroy.add(new WinCmd("route DELETE " + route.address + " MASK " + netmask + ' ' + gw.gateway_address()));
		      }
		  }
	      }
	    else
	      os << "NOTE: exclude routes error: cannot detect default gateway" << std::endl;
	  }

	// Process IPv4 redirect-gateway
	if (pull.reroute_gw.ipv4)
	  {
	    // add server bypass route
	    if (gw.defined())
	      {
		if (!pull.remote_address.ipv6)
		  {
		    create.add(new WinCmd("route ADD " + pull.remote_address.address + " MASK 255.255.255.255 " + gw.gateway_address()));
		    destroy.add(new WinCmd("route DELETE " + pull.remote_address.address + " MASK 255.255.255.255 " + gw.gateway_address()));
		  }
	      }
	    else
	      throw tun_win_setup("redirect-gateway error: cannot detect default gateway");

	    create.add(new WinCmd("route ADD 0.0.0.0 MASK 128.0.0.0 " + local4->gateway));
	    create.add(new WinCmd("route ADD 128.0.0.0 MASK 128.0.0.0 " + local4->gateway));
	    destroy.add(new WinCmd("route DELETE 0.0.0.0 MASK 128.0.0.0 " + local4->gateway));
	    destroy.add(new WinCmd("route DELETE 128.0.0.0 MASK 128.0.0.0 " + local4->gateway));
	  }

	// flush DNS cache
	//create.add(new WinCmd("net stop dnscache"));
	//create.add(new WinCmd("net start dnscache"));
	create.add(new WinCmd("ipconfig /flushdns"));
	//create.add(new WinCmd("ipconfig /registerdns"));
	destroy.add(new WinCmd("ipconfig /flushdns"));
      }
#endif

#if _WIN32_WINNT >= 0x0600 // Vista+
      TunWin::WFPContext::Ptr wfp{new TunWin::WFPContext};
#endif

      ActionList::Ptr remove_cmds;
    };
  }
}

#endif