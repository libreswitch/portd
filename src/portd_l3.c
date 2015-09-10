/*
 * Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 *
 * File: portd_l3.c
 */

#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_addr.h>
#include <unistd.h>
#include <assert.h>

#include "hash.h"
#include "portd.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(portd_l3);

extern unsigned int idl_seqno;
extern struct ovsdb_idl *idl;
extern struct ovsdb_idl_txn *txn;
extern bool commit_txn;
extern struct hmap all_vrfs;

int nl_ip_sock;
static int portd_get_prefix(int family, char *ip_address, void *prefix,
                            unsigned char *prefixlen);

/*********** Begin Connected routes handling **************/

static void
apply_mask_ipv6 (struct prefix_ipv6 *p)
{
    u_char *pnt;
    int index;
    int offset;
    static u_char maskbit[] = {0x00, 0x80, 0xc0, 0xe0, 0xf0,
                                0xf8, 0xfc, 0xfe, 0xff};

    index = p->prefixlen / 8;

    if (index < 16) {
        pnt = (u_char *) &p->prefix;
        offset = p->prefixlen % 8;

        pnt[index] &= maskbit[offset];
        index++;

        while (index < 16) {
            pnt[index++] = 0;
        }
    }
}

/* Convert masklen into IP address's netmask (network byte order). */
static void
masklen2ip (const int masklen, struct in_addr *netmask)
{
    assert (masklen >= 0 && masklen <= PORTD_IPV4_MAX_LEN);

    /* left shift is only defined for less than the size of the type.
     * we unconditionally use long long in case the target platform
     * has defined behaviour for << 32 (or has a 64-bit left shift) */
    if (sizeof(unsigned long long) > 4) {
        netmask->s_addr = htonl(0xffffffffULL << (32 - masklen));
    } else {
        netmask->s_addr = htonl(masklen ? 0xffffffffU << (32 - masklen) : 0);
    }
}

/* Apply mask to IPv4 prefix (network byte order). */
static void
apply_mask_ipv4 (struct prefix_ipv4 *p)
{
    struct in_addr mask;
    masklen2ip(p->prefixlen, &mask);
    p->prefix.s_addr &= mask.s_addr;
}

/*
 * Add a directly connected route to the DB. The NH is the port which
 * will be the egress port for the subnet
 */
static int
portd_add_connected_route (struct ovsrec_port *ovs_port, bool is_v4)
{
    const struct ovsrec_route *row = NULL;
    const struct ovsrec_vrf *row_vrf = NULL;
    struct ovsrec_nexthop *row_nh = NULL;

    const bool selected = true;
    char prefix_str[256];
    int64_t distance = CONNECTED_ROUTE_DISTANCE;
    int retval;

    /*
     * HALON_TODO: For now we support only 1 VRF in the system.
     * When we have support for multiple VRF, then fetch the
     * correct VRF for the port
     */
    row_vrf = ovsrec_vrf_first(idl);
    if(!row_vrf) {
        VLOG_ERR("No vrf information yet.");
        return -1;
    }
    /*
     * Populate the route row
     */
    row = ovsrec_route_insert(txn);
    ovsrec_route_set_vrf(row, row_vrf);
    if (is_v4) {
        struct prefix_ipv4 v4_prefix;
        char buf[INET_ADDRSTRLEN];

        ovsrec_route_set_address_family(row,
                        OVSREC_ROUTE_ADDRESS_FAMILY_IPV4);
        /*
         * Conversion to prefix format is a 3 step process:
         * - Convert the IP address string to prefix format.
         * - Apply the mask i.e. A.B.C.D/24 to A.B.C.0/24
         * - Convert it back to string to write to DB
         */
        retval = portd_get_prefix(AF_INET, ovs_port->ip4_address,
                                  &v4_prefix.prefix, &v4_prefix.prefixlen);
        if (retval) {
            VLOG_ERR("Error converting DB string to prefix: %s",
                     ovs_port->ip4_address);
            return retval;
        }
        apply_mask_ipv4(&v4_prefix);

        inet_ntop (AF_INET, &(v4_prefix.prefix), buf, INET_ADDRSTRLEN);
        snprintf (prefix_str, INET_PREFIX_SIZE,
                  "%s/%d", buf, v4_prefix.prefixlen);

        ovsrec_route_set_prefix(row, (const char *)prefix_str);
    } else {
        struct prefix_ipv6 v6_prefix;
        char buf[INET6_ADDRSTRLEN];

        ovsrec_route_set_address_family(row,
                        OVSREC_ROUTE_ADDRESS_FAMILY_IPV6);
        /*
         * Conversion to prefix format is a 3 step process:
         * - Convert the IP address string to prefix format.
         * - Apply the mask i.e. A.B.C.D/24 to A.B.C.0/24
         * - Convert it back to string to write to DB
         */
        retval = portd_get_prefix(AF_INET6, ovs_port->ip6_address,
                                  &v6_prefix.prefix, &v6_prefix.prefixlen);
        if (retval) {
            VLOG_ERR("Error converting DB string to prefix: %s",
                     ovs_port->ip6_address);
            return retval;
        }
        apply_mask_ipv6(&v6_prefix);

        inet_ntop (AF_INET6, &(v6_prefix.prefix), buf, INET6_ADDRSTRLEN);
        snprintf (prefix_str, INET6_PREFIX_SIZE,
                  "%s/%d", buf, v6_prefix.prefixlen);

        ovsrec_route_set_prefix(row, (const char *)prefix_str);
    }
    ovsrec_route_set_sub_address_family(row,
                        OVSREC_ROUTE_SUB_ADDRESS_FAMILY_UNICAST);
    ovsrec_route_set_from(row, OVSREC_ROUTE_FROM_CONNECTED);
    /*
     * Connected routes have a distance of 0
     */
    ovsrec_route_set_distance(row, &distance, 1);
    /*
     * Set the selected bit to true for the route entry
     */
    ovsrec_route_set_selected(row, &selected, 1);

    /*
     * Populate the Nexthop row
     */
    row_nh = ovsrec_nexthop_insert(txn);

    ovsrec_nexthop_set_ports(row_nh, &ovs_port, row_nh->n_ports + 1);

    /*
     * Update the route entry with the new nexthop
     */
    ovsrec_route_set_nexthops(row, &row_nh, row->n_nexthops + 1);

    commit_txn = true;

    return 0;
}

static bool
is_route_matched (const struct ovsrec_route *row_route, char *prefix_str,
                  char *port_name)
{
    if (!strcmp(row_route->prefix, prefix_str) &&
        !strcmp(row_route->from, OVSREC_ROUTE_FROM_CONNECTED) &&
        (row_route->sub_address_family == NULL ||
         !strcmp(row_route->sub_address_family,
                OVSREC_ROUTE_SUB_ADDRESS_FAMILY_UNICAST)) &&
        !strcmp(row_route->nexthops[0]->ports[0]->name, port_name)) {
        return true;
    }
    return false;
}

/*
 * Delete a directly connected route to the DB. The NH is the port which
 * will be the egress port for the subnet
 */
static int
portd_del_connected_route (char *address, char *port_name, bool is_v4)
{
    int retval;
    char prefix_str[256];
    const struct ovsrec_route *row_route = NULL;

    /*
     * Get the ip address from the port and convert it to the
     * prefix format
     */
    if (is_v4) {
        struct prefix_ipv4 v4_prefix;
        char buf[INET_ADDRSTRLEN];
        /*
         * Conversion to prefix format is a 3 step process:
         * - Convert the IP address string to prefix format.
         * - Apply the mask i.e. A.B.C.D/24 to A.B.C.0/24
         * - Convert it back to string
         */
        retval = portd_get_prefix(AF_INET, address, &v4_prefix.prefix,
                                  &v4_prefix.prefixlen);
        if (retval) {
            VLOG_ERR("Error converting DB string to prefix: %s", address);
            return retval;
        }
        apply_mask_ipv4(&v4_prefix);

        inet_ntop (AF_INET, &(v4_prefix.prefix), buf, INET_ADDRSTRLEN);
        snprintf (prefix_str, INET_PREFIX_SIZE,
                  "%s/%d", buf, v4_prefix.prefixlen);

        OVSREC_ROUTE_FOR_EACH(row_route, idl) {
            if (row_route->address_family != NULL) {
                if (strcmp(row_route->address_family, "ipv4")) {
                    continue;
                }
            }
            if (is_route_matched(row_route, prefix_str, port_name)) {
                /*
                 * Found the row. Delete the route and its nexthop
                 */
                ovsrec_nexthop_delete(row_route->nexthops[0]);
                ovsrec_route_delete(row_route);
                commit_txn = true;
                return 0;
            }
        }
    } else {
        struct prefix_ipv6 v6_prefix;
        char buf[INET6_ADDRSTRLEN];
        /*
         * Conversion to prefix format is a 3 step process:
         * - Convert the IP address string to prefix format.
         * - Apply the mask i.e. A.B.C.D/24 to A.B.C.0/24
         * - Convert it back to string
         */
        retval = portd_get_prefix(AF_INET6, address, &v6_prefix.prefix,
                                  &v6_prefix.prefixlen);
        if (retval) {
            VLOG_ERR("Error converting DB string to prefix: %s", address);
            return retval;
        }
        apply_mask_ipv6(&v6_prefix);

        inet_ntop (AF_INET6, &(v6_prefix.prefix), buf, INET6_ADDRSTRLEN);
        snprintf (prefix_str, INET6_PREFIX_SIZE,
                  "%s/%d", buf, v6_prefix.prefixlen);

        OVSREC_ROUTE_FOR_EACH(row_route, idl) {
            if (row_route->address_family == NULL ||
                strcmp(row_route->address_family, "ipv6")) {
                /*
                 * Skip NULL and non ipv6 address families
                 */
                continue;
            }
            if (is_route_matched(row_route, prefix_str, port_name)) {
                /*
                 * Found the row. Delete the route and its nexthop
                 */
                ovsrec_nexthop_delete(row_route->nexthops[0]);
                ovsrec_route_delete(row_route);
                commit_txn = true;
                return 0;
            }
        }
    }
    /*
     * We should have found an entry and returned before we hit the end
     */
    VLOG_ERR("Connected route not found for port %s",port_name);
    return -1;
}

/*********** End Connected routes handling **************/

static
int portd_netlink_socket_open(void)
{
    struct sockaddr_nl s_addr;

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

    if (sock < 0) {
        return sock;
    }

    memset((void *) &s_addr, 0, sizeof(s_addr));
    s_addr.nl_family = AF_NETLINK;
    s_addr.nl_pid = getpid();
    s_addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (bind(sock, (struct sockaddr *) &s_addr, sizeof(s_addr)) < 0) {
        return -1;
    }

    return sock;
}

static
void portd_netlink_socket_close(int socket)
{
    close(socket);
}

void
portd_exit_ipcfg(void)
{
    portd_netlink_socket_close(nl_ip_sock);
}

void
portd_init_ipcfg(void)
{
    nl_ip_sock = portd_netlink_socket_open();
}

/* write to /proc entries to enable/disable Linux ip forwarding(routing) */
void
portd_config_iprouting(int enable)
{
    int fd = -1, nbytes = 0;
    char buf[16];
    const char *ipv4_path = "/proc/sys/net/ipv4/ip_forward";
    const char *ipv6_path = "/proc/sys/net/ipv6/conf/all/forwarding";

    nbytes = sprintf(buf, "%d", enable);

    if ((fd = open(ipv4_path, O_WRONLY)) == -1) {
        VLOG_ERR("Unable to open %s (%s)", ipv4_path, strerror(errno));
        return;
    }
    if (write(fd, buf, nbytes) == -1) {
        VLOG_ERR("Unable to write to %s (%s)", ipv4_path, strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    VLOG_DBG("%s ipv4 forwarding", (enable == 1 ? "Enabled" : "Disabled"));

    if ((fd = open(ipv6_path, O_WRONLY)) == -1) {
        VLOG_ERR("Unable to open %s (%s)", ipv6_path, strerror(errno));
        return;
    }
    if (write(fd, buf, nbytes) == -1) {
        VLOG_ERR("Unable to write to %s (%s)", ipv6_path, strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    VLOG_DBG("%s ipv6 forwarding", (enable == 1 ? "Enabled" : "Disabled"));
}

/* return ipv4/ipv6 prefix and prefix length */
static int
portd_get_prefix(int family, char *ip_address, void *prefix,
                 unsigned char *prefixlen)
{
    char *p;
    char *ip_address_copy;
    int maxlen = (family == AF_INET) ? PORTD_IPV4_MAX_LEN :
                                       PORTD_IPV6_MAX_LEN;
    *prefixlen = maxlen;

    /*
     * Make a copy of the IP/IPv6 address.
     */
    ip_address_copy = xstrdup(ip_address);

    /*
     * Extract the mask length of the address.
     */
    if ((p = strchr(ip_address_copy, '/'))) {
        *p++ = '\0';
        *prefixlen = atoi(p);
    }

    /*
     * If the extracted mask length is greater
     * than 'maxlen', then free the memory in
     * 'ip_address_copy' and return -1.
     */
    if (*prefixlen > maxlen) {
        VLOG_DBG("Bad prefixlen %d > %d", *prefixlen, maxlen);
        free(ip_address_copy);
        return -1;
    }

    /*
     * If the extraction of the prefix fails, then
     * free the memory in 'ip_address_copy' and return -1.
     */
    if (inet_pton(family, ip_address_copy, prefix) == 0) {
        VLOG_DBG("%d inet_pton failed with %s", family, strerror(errno));
        free(ip_address_copy);
        return -1;
    }

    /*
     * In case of successful extraction,
     * free the memory in 'ip_address_copy'
     * and return 0.
     */
    free(ip_address_copy);
    return 0;
}

/* HALON_TODO - ipv6 secondary address also shows up as primary in 'ip -6 addr show' - fix */

/* Set IP address on Linux interface using netlink sockets */
static void
portd_set_ipaddr(int cmd, const char *port_name, char *ip_address,
                 int family, bool secondary)
{
    int buflen;
    struct rtattr *rta;
    int bytelen;
    struct {
        struct nlmsghdr n;
        struct ifaddrmsg ifa;
        char buf[128];
    } req;
    struct in_addr ipv4;
    struct in6_addr ipv6;
    unsigned char prefixlen, *ipaddr = NULL;

    memset (&req, 0, sizeof(req));

    bytelen = (family == AF_INET ? 4 : 16);

    req.n.nlmsg_len = NLMSG_LENGTH (sizeof (struct ifaddrmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_type = cmd;

    req.ifa.ifa_family = family;
    req.ifa.ifa_index = if_nametoindex(port_name);
    if (req.ifa.ifa_index == 0) {
        VLOG_ERR("Unable to get ifindex for port '%s'", port_name);
        return;
    }
    if (family == AF_INET) {
        if (portd_get_prefix(AF_INET, ip_address, &ipv4, &prefixlen) == -1) {
            VLOG_ERR("Unable to get prefix info for '%s'", ip_address);
            return;
        }
        ipaddr = (unsigned char *)&ipv4;
    } else if (family == AF_INET6) {
        if (portd_get_prefix(AF_INET6, ip_address, &ipv6, &prefixlen) == -1) {
            VLOG_ERR("Unable to get prefix info for '%s'", ip_address);
            return;
        }
        ipaddr = (unsigned char *)&ipv6;
    }
    req.ifa.ifa_prefixlen = prefixlen;

    if (secondary) {
        req.ifa.ifa_flags |=  IFA_F_SECONDARY;
    }

    buflen = RTA_LENGTH(bytelen);
    if (NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(buflen) > sizeof(req)) {
        VLOG_ERR("Message length (%d) exceeded max (%d)",
                NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(buflen), (int)sizeof(req));
        return;
    }

    rta = NLMSG_TAIL(&req.n);
    rta->rta_type = IFA_LOCAL;
    rta->rta_len = buflen;
    memcpy(RTA_DATA(rta), ipaddr, bytelen);
    req.n.nlmsg_len = NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(buflen);

    if (send(nl_ip_sock, &req, req.n.nlmsg_len, 0) == -1) {
        VLOG_ERR("Netlink failed to set IP address for '%s'", ip_address);
        return;
    }

    VLOG_DBG("Netlink %s IP addr '%s' and mask length = %u (%s) for port '%s'",
             (cmd == RTM_NEWADDR) ? "added" : "deleted",
             ip_address, prefixlen, secondary ? "secondary":"primary",
             port_name);
}

static struct net_address *
portd_ip6_addr_find(struct port *cfg, const char *address)
{
    struct net_address *addr;

    HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                             &cfg->secondary_ip6addr) {
        if (!strcmp(addr->address, address)) {
            return addr;
        }
    }

    return NULL;
}

static struct net_address *
portd_ip4_addr_find(struct port *cfg, const char *address)
{
    struct net_address *addr;

    HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                             &cfg->secondary_ip4addr) {
        if (!strcmp(addr->address, address)) {
            return addr;
        }
    }

    return NULL;
}

/*
 * This function is used to check if the given IP address is present
 * in the list of IP addresses for a specific interface in the kernel.
 */
static bool
portd_find_ip_addr_kernel(struct kernel_port *port,
                          const char *address, bool ipv6)
{
    struct net_address *addr;

    if (ipv6) {
        HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                                     &port->ip6addr) {
            if (!strcmp(addr->address, address)) {
                return true;
            }
        }
    } else {
        HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                                     &port->ip4addr) {
            if (!strcmp(addr->address, address)) {
                return true;
            }
        }
    }

    return false;
}

/*
 * This function is used to check if the given IP address is present
 * in the list of IP addresses for a specific interface in the DB.
 */
static bool
portd_find_ip_addr_db(struct port *port, const char *address, bool ipv6)
{
    struct net_address *addr;

    if (ipv6) {
        if (port->ip6_address && !strcmp(port->ip6_address, address)) {
            return true;
        }
        HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                                     &port->secondary_ip6addr) {
            if (!strcmp(addr->address, address)) {
                return true;
            }
        }
    } else {
        if (port->ip4_address && !strcmp(port->ip4_address, address)) {
            return true;
        }
        HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                                     &port->secondary_ip4addr) {
            if (!strcmp(addr->address, address)) {
                return true;
            }
        }
    }

    return false;
}

/* Add secondary v6 address in Linux that got added.
 * Delete secondary v6 addresses from Linux that got deleted.
 */
static void
portd_config_secondary_ipv6_addr(struct port *port,
                                 struct ovsrec_port *port_row)
{
    struct shash new_ip6_list;
    struct net_address *addr, *next;
    struct shash_node *addr_node;
    int i;

    shash_init(&new_ip6_list);

    /*
     * Collect the interested network addresses
     */
    for (i = 0; i < port_row->n_ip6_address_secondary; i++) {
        if(!shash_add_once(&new_ip6_list, port_row->ip6_address_secondary[i],
                           port_row->ip6_address_secondary[i])) {
            VLOG_WARN("Duplicate address in secondary list %s\n",
                      port_row->ip6_address_secondary[i]);
        }
    }

    /*
     * Parse the existing list of addresses and remove obsolete ones
     */
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &port->secondary_ip6addr) {
        if (!shash_find_data(&new_ip6_list, addr->address)) {
            hmap_remove(&port->secondary_ip6addr, &addr->addr_node);
            portd_set_ipaddr(RTM_DELADDR, port->name, addr->address, AF_INET6, true);
            free(addr->address);
            free(addr);
        }
    }

    /*
     * Add the newly added addresses to the list
     */
    SHASH_FOR_EACH (addr_node, &new_ip6_list) {
        struct net_address *addr;
        const char *address = addr_node->data;
        if (!portd_ip6_addr_find(port, address)) {
            /*
             * Add the new address to the list
             */
            addr = xzalloc(sizeof *addr);
            addr->address = xstrdup(address);
            hmap_insert(&port->secondary_ip6addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            portd_set_ipaddr(RTM_NEWADDR, port->name, addr->address, AF_INET6, true);
        }
    }
}


/* Add secondary v4 address in Linux that got added in db.
 * Delete secondary v4 addresses from Linux that got deleted from db.
 */
static void
portd_config_secondary_ipv4_addr(struct port *port,
                                 struct ovsrec_port *port_row)
{
    struct shash new_ip_list;
    struct net_address *addr, *next;
    struct shash_node *addr_node;
    int i;

    shash_init(&new_ip_list);

    /*
     * Collect the interested network addresses
     */
    for (i = 0; i < port_row->n_ip4_address_secondary; i++) {
        if(!shash_add_once(&new_ip_list, port_row->ip4_address_secondary[i],
                           port_row->ip4_address_secondary[i])) {
            VLOG_WARN("Duplicate address in secondary list %s\n",
                      port_row->ip4_address_secondary[i]);
        }
    }

    /*
     * Parse the existing list of addresses and remove obsolete ones
     */
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &port->secondary_ip4addr) {
        if (!shash_find_data(&new_ip_list, addr->address)) {
            hmap_remove(&port->secondary_ip4addr, &addr->addr_node);
            portd_set_ipaddr(RTM_DELADDR, port->name, addr->address, AF_INET, true);
            free(addr->address);
            free(addr);
        }
    }

    /*
     * Add the newly added addresses to the list
     */
    SHASH_FOR_EACH (addr_node, &new_ip_list) {
        struct net_address *addr;
        const char *address = addr_node->data;
        if (!portd_ip4_addr_find(port, address)) {
            /*
             * Add the new address to the list
             */
            addr = xzalloc(sizeof *addr);
            addr->address = xstrdup(address);
            hmap_insert(&port->secondary_ip4addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            portd_set_ipaddr(RTM_NEWADDR, port->name, addr->address, AF_INET, true);
        }
    }
}

/**
 * This function adds ipv4 address on a given port to kernel.
 */
void
portd_add_ipv4_addr(struct port *port)
{
    struct net_address *addr, *next_addr;

    if (!port) {
        VLOG_DBG("The port on which the addresses need to be added into "
                 "kernel is null\n");
        return;
    }

    if (port->ip4_address) {
        portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip4_address, AF_INET, false);
    }

    HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node, &port->secondary_ip4addr) {
        portd_set_ipaddr(RTM_NEWADDR, port->name, addr->address, AF_INET, true);
    }
}

/**
 * This function adds ipv6 address on a given port to kernel.
 */
void
portd_add_ipv6_addr(struct port *port)
{
    struct net_address *addr, *next_addr;

    if (!port) {
        VLOG_DBG("The port on which the addresses need to be added into "
                 "kernel is null\n");
        return;
    }

    if (port->ip6_address) {
        portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip6_address, AF_INET6, false);
    }

    HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node, &port->secondary_ip6addr) {
        portd_set_ipaddr(RTM_NEWADDR, port->name, addr->address, AF_INET6, true);
    }
}

/**
 * This functionn adds both ipv4 and ipv6 addresses on a given port to kernel
 */
void
portd_add_ipaddr(struct port *port)
{
    portd_add_ipv4_addr(port);
    portd_add_ipv6_addr(port);
}

/**
 * This function deletes ipv4 address on a given port from kernel
 */
void
portd_del_ipv4_addr(struct port *port)
{
    struct net_address *addr, *next_addr;

    if (!port) {
        VLOG_DBG("The port on which the addresses need to be deleted into "
                 "kernel is null\n");
        return;
    }

    if (port->ip4_address) {
        portd_set_ipaddr(RTM_DELADDR, port->name, port->ip4_address, AF_INET, false);
    }

    HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node, &port->secondary_ip4addr) {
        portd_set_ipaddr(RTM_DELADDR, port->name, addr->address, AF_INET, true);
    }
}

/**
 * This function deletes ipv6 address on a given port from kernel
 */
void
portd_del_ipv6_addr(struct port *port)
{
    struct net_address *addr, *next_addr;

    if (!port) {
        VLOG_DBG("The port on which the addresses need to be deleted into "
                 "kernel is null\n");
        return;
    }

    if (port->ip6_address) {
        portd_set_ipaddr(RTM_DELADDR, port->name, port->ip6_address, AF_INET6, false);
    }

    HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node, &port->secondary_ip6addr) {
        portd_set_ipaddr(RTM_DELADDR, port->name, addr->address, AF_INET6, true);
    }
}

/**
 * This function deletes both ipv4 and ipv6 address on a given port
 * from kernel
 */
void
portd_del_ipaddr(struct port *port)
{
    portd_del_ipv4_addr(port);
    portd_del_ipv6_addr(port);
}

/* Take care of add/delete/modify of v4/v6 address from db */
void
portd_reconfig_ipaddr(struct port *port, struct ovsrec_port *port_row)
{
    /*
     * Configure primary network addresses
     */
    if (port_row->ip4_address) {
        if (port->ip4_address) {
            if (strcmp(port->ip4_address, port_row->ip4_address) != 0) {
                portd_set_ipaddr(RTM_DELADDR, port->name, port->ip4_address,
                                 AF_INET, false);
                /*
                 * Delete the old route
                 */
                portd_del_connected_route(port->ip4_address, port->name, true);
                free(port->ip4_address);

                port->ip4_address = xstrdup(port_row->ip4_address);
                portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip4_address,
                                 AF_INET, false);
                /*
                 * Add the new route
                 */
                portd_add_connected_route(port_row, true);
            }
        } else {
            port->ip4_address = xstrdup(port_row->ip4_address);
            portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip4_address,
                             AF_INET, false);
            /*
             * Add a new route
             */
            portd_add_connected_route(port_row, true);
        }
    } else {
        if (port->ip4_address != NULL) {
            portd_set_ipaddr(RTM_DELADDR, port->name, port->ip4_address,
                             AF_INET, false);
            /*
             * Delete the route
             */
            portd_del_connected_route(port->ip4_address, port->name, true);
            free(port->ip4_address);
            port->ip4_address = NULL;
        }
    }

    if (port_row->ip6_address) {
        if (port->ip6_address) {
            if (strcmp(port->ip6_address, port_row->ip6_address) !=0) {
                portd_set_ipaddr(RTM_DELADDR, port->name, port->ip6_address,
                                 AF_INET6, false);
                /*
                 * Delete the old route
                 */
                portd_del_connected_route(port->ip6_address, port->name, false);
                free(port->ip6_address);

                port->ip6_address = xstrdup(port_row->ip6_address);
                portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip6_address,
                                 AF_INET6, false);
                /*
                 * Add the new route
                 */
                portd_add_connected_route(port_row, false);
            }
        } else {
            port->ip6_address = xstrdup(port_row->ip6_address);
            portd_set_ipaddr(RTM_NEWADDR, port->name, port->ip6_address,
                             AF_INET6, false);
            /*
             * Add the new route
             */
            portd_add_connected_route(port_row, false);
        }
    } else {
        if (port->ip6_address != NULL) {
            portd_set_ipaddr(RTM_DELADDR, port->name, port->ip6_address,
                             AF_INET6, false);
            /*
             * Delete the route
             */
            portd_del_connected_route(port->ip6_address, port->name, false);
            free(port->ip6_address);
            port->ip6_address = NULL;
        }
    }

    /*
     * Configure secondary network addresses
     */
    if (OVSREC_IDL_IS_COLUMN_MODIFIED(ovsrec_port_col_ip4_address_secondary,
                                      idl_seqno) ) {
        VLOG_DBG("ip4_address_secondary modified");
        portd_config_secondary_ipv4_addr(port, port_row);
    }

    if (OVSREC_IDL_IS_COLUMN_MODIFIED(ovsrec_port_col_ip6_address_secondary,
                                      idl_seqno) ) {
        VLOG_DBG("ip6_address_secondary modified");
        portd_config_secondary_ipv6_addr(port, port_row);
    }
}


/**
 * Function: add_link_attr
 * Params:
 *      n: pointer to start of netlink message. Mainly used to adjust message length.
 *      nlmsg_maxlen: max allowed message length (header+payload).
 *      attr_type: attribute type (typically, IFLA_XXX).
 *      payload: data appended to nlmsg packet.
 *      payload_len: length of data being appended.
 * Return:
 *      0: success
 *     -1: buffer is full, can't add anymore payload.
 */
static int
add_link_attr(struct nlmsghdr *n, int nlmsg_maxlen,
              int attr_type, const void *payload, int payload_len)
{
    int len = RTA_LENGTH(payload_len);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > nlmsg_maxlen) {
        VLOG_ERR("message exceeded bound of %d. Failed to add attribute: %d",
                 nlmsg_maxlen, attr_type);
        return -1;
    }

    rta = NLMSG_TAIL(n);
    rta->rta_type = attr_type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), payload, payload_len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
    return 0;
}

/**
 * Function: portd_add_vlan_interface
 * Param:
 *      interface_name: "Parent" interface on which vlan interface is created.
 *      vlan_interface_name: Name of VLAN interface to be created.
 *      vlan_tag: VLAN id.
 * Return:
 * Desc:
 *      Insert VLAN interface <vlan_interface_name> on top of <interface_name>
 *      with VLAN tag <vlan_tag>
 */
void
portd_add_vlan_interface(const char *interface_name,
                           const char *vlan_interface_name,
                           const unsigned short vlan_tag)
{
    int     ifindex;

    struct {
        struct nlmsghdr  n;
        struct ifinfomsg i;
        char             buf[128];  /* must fit interface name length (IFNAMSIZ)
                                       and attribute hdrs. */
    } req;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_SPACE(sizeof(struct ifinfomsg));
    req.n.nlmsg_pid     = getpid();
    req.n.nlmsg_type    = RTM_NEWLINK;
    req.n.nlmsg_flags   = NLM_F_REQUEST | NLM_F_CREATE;

    req.i.ifi_family    = AF_UNSPEC;
    ifindex             = if_nametoindex(interface_name);

    if (ifindex == 0) {
        VLOG_ERR("Unable to get ifindex for interface: %s", interface_name);
        return;
    }

    struct rtattr *linkinfo = NLMSG_TAIL(&req.n);
    add_link_attr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);
    add_link_attr(&req.n, sizeof(req), IFLA_INFO_KIND, "vlan", 4);

    struct rtattr * data = NLMSG_TAIL(&req.n);
    add_link_attr(&req.n, sizeof(req), IFLA_INFO_DATA, NULL, 0);
    add_link_attr(&req.n, sizeof(req), IFLA_VLAN_ID, &vlan_tag, 2);

    /* Adjust rta_len for attributes */
    data->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)data;
    linkinfo->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)linkinfo;

    add_link_attr(&req.n, sizeof(req), IFLA_LINK, &ifindex, 4);
    add_link_attr(&req.n, sizeof(req), IFLA_IFNAME, vlan_interface_name,
                  strlen(vlan_interface_name)+1);

    if (send(nl_ip_sock, &req, req.n.nlmsg_len, 0) == -1) {
        VLOG_ERR("Netlink failed to create vlan interface: %s",
                 vlan_interface_name);
        return;
    }
}

/**
 * Function: portd_del_vlan_interface
 * Param:
 *      interface_name: "Parent" interface on which vlan interface is created.
 *      vlan_interface_name: Name of VLAN interface to be created.
 *      vlan_tag: VLAN id.
 * Return:
 * Desc:
 *      Delete VLAN interface <vlan_interface_name>
 * OPENSWITCH_TODO:
 *      Code in this function can delete non-vlan interfaces as well. Generalize
 *      the name.
 */
void
portd_del_vlan_interface(const char *vlan_interface_name)
{
    struct {
        struct nlmsghdr  n;
        struct ifinfomsg i;
        char             buf[128];  /* must fit interface name length (IFNAMSIZ)*/
    } req;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_SPACE(sizeof(struct ifinfomsg));
    req.n.nlmsg_pid     = getpid();
    req.n.nlmsg_type    = RTM_DELLINK;
    req.n.nlmsg_flags   = NLM_F_REQUEST;

    req.i.ifi_family    = AF_UNSPEC;
    req.i.ifi_index     = if_nametoindex(vlan_interface_name);

    if (req.i.ifi_index == 0) {
        VLOG_ERR("Unable to get ifindex for interface: %s", vlan_interface_name);
        return;
    }

    if (send(nl_ip_sock, &req, req.n.nlmsg_len, 0) == -1) {
        VLOG_ERR("Netlink failed to delete vlan interface: %s",
                 vlan_interface_name);
        return;
    }
}

/**
 * Function: portd_interface_up_down
 * Param:
 *      vlan_interface_name: Name of the interface to be set to "up" or "down".
 *      status: "up" or "down".
 * Return:
 * Desc:
 *      Set an interface state to "up" or "down".
 * OPENSWITCH_TODO:
 *      Plugin to "shutdown"/"no shutdown" commands under an interface.
 */
void
portd_interface_up_down(const char *interface_name, const char *status)
{
    if (status == NULL || strcmp(status, PORTD_EMPTY_STRING)==0) {
        VLOG_ERR("Invalid status argument");
        return;
    }

    if (interface_name == NULL ||
            strcmp(interface_name, PORTD_EMPTY_STRING)==0) {
        VLOG_ERR("Invalid interface-name as argument");
        return;
    }

    struct {
        struct nlmsghdr  n;
        struct ifinfomsg i;
        char             buf[128];  /* must fit interface name length (IFNAMSIZ)*/
    } req;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_SPACE(sizeof(struct ifinfomsg));
    req.n.nlmsg_pid     = getpid();
    req.n.nlmsg_type    = RTM_NEWLINK;
    req.n.nlmsg_flags   = NLM_F_REQUEST;

    req.i.ifi_family    = AF_UNSPEC;
    req.i.ifi_index     = if_nametoindex(interface_name);

    if (req.i.ifi_index==0) {
        VLOG_ERR("Unable to get ifindex for interface: %s", interface_name);
        return;
    }

    /* OPENSWITCH_TODO: _May_ have to convert this to "no shutdown"/"shutdown" */
    if (strcmp(status, "up")==0) {
        req.i.ifi_change |= IFF_UP;
        req.i.ifi_flags  |= IFF_UP;
    } else if (strcmp(status, "down")==0) {
        req.i.ifi_change |= IFF_UP;
        req.i.ifi_flags  &= ~IFF_UP;
    }

    if (send(nl_ip_sock, &req, req.n.nlmsg_len, 0) == -1) {
        VLOG_ERR("Netlink failed to bring %s the interface %s", status,
                 interface_name);
        return;
    }
}

/*
 * This function is used to check and add a kernel interface to the
 * kernel port list which will be used later to compare with DB list.
 */
static struct kernel_port *
find_or_create_kernel_port(struct shash *kernel_port_list, const char *ifname)
{
    struct kernel_port *port;
    port = shash_find_data(kernel_port_list, ifname);
    /* For every new interface, add to kernel port list */
    if (!port) {
        port = xzalloc(sizeof *port);
        port->name = xstrdup(ifname);
        hmap_init(&port->ip4addr);
        hmap_init(&port->ip6addr);
    }
    return port;
}

/*
 * This function is used to parse the Netlink response
 * for the dump request.
 */
static void
parse_nl_msg(struct nlmsghdr *nlh, int msglen,
             struct shash *kernel_port_list)
{
    struct rtattr *rta;
    struct ifaddrmsg *ifa;
    int rtalen;
    char ifname[IF_NAMESIZE];
    char recvip[INET6_ADDRSTRLEN];
    char ip_address[INET6_PREFIX_SIZE];
    struct kernel_port *port;
    struct net_address *addr;
    bool add_to_list = false;

    while (NLMSG_OK(nlh, msglen)) {
        port = NULL;
        ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
        rta = (struct rtattr *)IFA_RTA(ifa);
        rtalen = IFA_PAYLOAD(nlh);

        memset(ifname, 0, sizeof(ifname));
        if_indextoname(ifa->ifa_index, ifname);
        VLOG_DBG("Interface = %s\n",ifname);

        if(!strcmp(ifname, LOOPBACK_INTERFACE_NAME)) {
            nlh = NLMSG_NEXT(nlh, msglen);
            continue;
        }

        for (; RTA_OK(rta, rtalen); rta = RTA_NEXT(rta, rtalen)) {
            memset(ip_address, 0, sizeof(ip_address));
            if(rta->rta_type == IFA_ADDRESS){
                memset(recvip, 0, sizeof(recvip));
                if (ifa->ifa_family == AF_INET) {
                    inet_ntop(AF_INET, RTA_DATA(rta), recvip,
                            INET_ADDRSTRLEN);
                    snprintf(ip_address, INET6_PREFIX_SIZE,
                            "%s/%d",recvip,ifa->ifa_prefixlen);
                    VLOG_DBG("Netlink message has IPv4 addr : %s",ip_address);
                    port = find_or_create_kernel_port(kernel_port_list, ifname);
                    addr = xzalloc(sizeof *addr);
                    addr->address = xstrdup(ip_address);
                    hmap_insert(&port->ip4addr, &addr->addr_node,
                            hash_string(addr->address, 0));
                    add_to_list = true;
                } else if (ifa->ifa_family == AF_INET6) {
                    inet_ntop(AF_INET6, RTA_DATA(rta), recvip,
                            INET6_ADDRSTRLEN);
                    snprintf(ip_address, INET6_PREFIX_SIZE,
                            "%s/%d",recvip,ifa->ifa_prefixlen);
                    if (ifa->ifa_scope == IPV6_ADDR_SCOPE_LINK) {
                        VLOG_DBG("Link Local IPv6 address. Do nothing!");
                        break;
                    }
                    VLOG_DBG("Netlink message has IPv6 addr : %s",ip_address);
                    port = find_or_create_kernel_port(kernel_port_list, ifname);
                    addr = xzalloc(sizeof *addr);
                    addr->address = xstrdup(ip_address);
                    hmap_insert(&port->ip6addr, &addr->addr_node,
                            hash_string(addr->address, 0));
                    add_to_list = true;
                }
            }
        }
        if (add_to_list) {
            shash_add_once(kernel_port_list, ifname, port);
            add_to_list = false;
        }
        nlh = NLMSG_NEXT(nlh, msglen);
    }
}

/*
 * This function is used to parse the dump command response.
 * It handles multi part message and calls the parse_nl_msg
 * function to act on Netlink messages.
 */
static void
parse_ip_addr_dump(struct shash *kernel_port_list)
{
    bool multipart_msg_end = false;
    while (!multipart_msg_end) {
        struct sockaddr_nl nladdr;
        struct msghdr msg;
        struct iovec iov;
        struct nlmsghdr *nlh;
        char buffer[RECV_BUFFER_SIZE];
        int ret;

        iov.iov_base = (void *)buffer;
        iov.iov_len = sizeof(buffer);
        msg.msg_name = (void *)&(nladdr);
        msg.msg_namelen = sizeof(nladdr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        ret = recvmsg(nl_ip_sock, &msg, 0);
        if (ret < 0) {
            return;
        }
        nlh = (struct nlmsghdr*) buffer;

        switch(nlh->nlmsg_type) {

            case RTM_NEWADDR:
                parse_nl_msg(nlh, ret, kernel_port_list);
                break;

            case NLMSG_DONE:
                VLOG_DBG("End of multi part message");
                multipart_msg_end = true;
                break;

            default:
                break;
        }
        if (!(nlh->nlmsg_flags & NLM_F_MULTI)) {
            VLOG_DBG("End of message. Not a multipart message");
            break;
        }
    }
    return;
}

/*
 * This function is used to make netlink calls to dump the IP addresses
 * from the kernel. It is called twice (IPv4 & IPv6).
 */
static void
portd_populate_kernel_ip_addr(int family, struct shash *kernel_port_list)
{
    struct rtattr *rta;
    int bytelen;
    struct {
        struct nlmsghdr n;
        struct ifaddrmsg ifa;
    } req;

    memset (&req, 0, sizeof(req));

    bytelen = (family == AF_INET ? 4 : 16);

    req.n.nlmsg_len = NLMSG_LENGTH (sizeof (struct ifaddrmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

    req.n.nlmsg_type = RTM_GETADDR;

    req.ifa.ifa_family = family;
    rta = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len));
    rta->rta_len = RTA_LENGTH(bytelen);

    if (send(nl_ip_sock, &req, req.n.nlmsg_len, 0) == -1) {
        VLOG_ERR("Netlink failed to send message for IP addr dump");
        return;
    }
    VLOG_DBG("Netlink %s addr dump command sent",
            family == AF_INET? "IPv4" : "IPv6");
    /* Parse the dump command response */
    parse_ip_addr_dump(kernel_port_list);
}

/*
 * This function loops over the L3 interfaces which are attached to
 * VRFs and makes a DB list of IP addresses for each interface.
 */
static void
portd_populate_db_ip_addr(struct shash *db_port_list)
{
    struct port *db_port;
    struct vrf *vrf, *next_vrf;
    const struct ovsrec_port *port_row;
    struct net_address *addr;
    struct smap hw_cfg_smap;
    int vlan_id;
    size_t i, j, k;

    HMAP_FOR_EACH_SAFE (vrf, next_vrf, node, &all_vrfs) {
        for (i = 0; i < vrf->cfg->n_ports; i++) {
            port_row = vrf->cfg->ports[i];
            db_port = xzalloc(sizeof *db_port);
            db_port->vrf = vrf;
            db_port->name = xstrdup(port_row->name);
            db_port->cfg = port_row;
            smap_clone(&hw_cfg_smap, &port_row->hw_config);
            vlan_id = smap_get_int(&hw_cfg_smap,
                    PORT_HW_CONFIG_MAP_INTERNAL_VLAN_ID, 0);
            if(vlan_id != 0) {
                db_port->internal_vid = vlan_id;
            } else {
                db_port->internal_vid = -1;
            }
            smap_destroy(&hw_cfg_smap);
            hmap_init(&db_port->secondary_ip4addr);
            hmap_init(&db_port->secondary_ip6addr);
            if (port_row->ip4_address) {
                db_port->ip4_address = xstrdup(port_row->ip4_address);
            }
            if (port_row->ip6_address) {
                db_port->ip6_address = xstrdup(port_row->ip6_address);
            }
            for(j = 0 ; j < port_row->n_ip4_address_secondary ; j++) {
                addr = xzalloc(sizeof *addr);
                addr->address = xstrdup(port_row->ip4_address_secondary[j]);
                hmap_insert(&db_port->secondary_ip4addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            }
            for(k = 0 ; k < port_row->n_ip6_address_secondary ; k++) {
                addr = xzalloc(sizeof *addr);
                addr->address = xstrdup(port_row->ip6_address_secondary[k]);
                hmap_insert(&db_port->secondary_ip6addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            }
            shash_add_once(db_port_list, port_row->name, db_port);
            VLOG_DBG("L3 interface '%s' added to DB port list",
                    port_row->name);
        }
    }
}

/*
 * This function is used to add a port to the local cache
 * after processing it for IP addresses cleanup. This will ensure
 * that the interface does not get reconfigured.
 */
static void
portd_add_port_to_cache(struct port *port)
{
    struct vrf *vrf, *next_vrf;
    size_t i;
    HMAP_FOR_EACH_SAFE (vrf, next_vrf, node, &all_vrfs) {
        for (i = 0; i < vrf->cfg->n_ports; i++) {
            if (!strcmp(vrf->cfg->ports[i]->name, port->name)) {
                hmap_insert(&vrf->ports, &port->port_node,
                        hash_string(port->name, 0));
            }
        }
    }
}

/*
 * This function is used to ensure kernel and DB IP addresses
 * are in sync after a daemon restart. It does the following:
 * 1. Populate a list of kernel IPv4 & IPv6 addresses
 *    (both primary and secondary)
 * 2. Populate a list of IP addresses from the DB.
 * 3. Compare the list of kernel IP addresses with the DB IP addresses
 *    a) Delete obsolete IP addresses
 *    b) Add new IP addresses
 */
void
portd_ipaddr_config_on_init(void)
{
    struct shash kernel_port_list, db_port_list;
    struct port *db_port;
    struct kernel_port *kernel_port;
    struct shash_node *node, *next;
    struct net_address *addr, *next_addr;
    bool add_to_local_cache = false;

    /* Initialize and populate the list of kernel IP addresses */
    shash_init(&kernel_port_list);
    portd_populate_kernel_ip_addr(AF_INET, &kernel_port_list);
    portd_populate_kernel_ip_addr(AF_INET6, &kernel_port_list);
    /* Initialize and populate the list of DB IP addresses */
    shash_init(&db_port_list);
    portd_populate_db_ip_addr(&db_port_list);

    if (VLOG_IS_DBG_ENABLED()) {
        VLOG_DBG("Dump of kernel ports");
        SHASH_FOR_EACH_SAFE (node, next, &kernel_port_list) {
            kernel_port = node->data;
            VLOG_DBG("Port Name : %s", kernel_port->name);
            HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node,
                    &kernel_port->ip4addr) {
                VLOG_DBG("IPv4 addr : %s", addr->address);
            }
            HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node,
                    &kernel_port->ip6addr) {
                VLOG_DBG("IPv6 addr : %s", addr->address);
            }
        }
        VLOG_DBG("Dump of DB ports");
        SHASH_FOR_EACH_SAFE (node, next, &db_port_list) {
            db_port = node->data;
            VLOG_DBG("Port Name : %s", db_port->name);
            VLOG_DBG("IPv4 addr : %s", db_port->ip4_address);
            VLOG_DBG("IPv6 addr : %s", db_port->ip6_address);
            HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node,
                    &db_port->secondary_ip4addr) {
                VLOG_DBG("Secondary IPv4 addr : %s", addr->address);
            }
            HMAP_FOR_EACH_SAFE (addr, next_addr, addr_node,
                    &db_port->secondary_ip6addr) {
                VLOG_DBG("Secondary IPv6 addr : %s", addr->address);
            }
        }
    }

    /* We loop over the kernel port list and add/delete IP addresses
     * after matching with DB */
    SHASH_FOR_EACH_SAFE (node, next, &kernel_port_list) {
        kernel_port = node->data;
        db_port = shash_find_data(&db_port_list, kernel_port->name);
        /* If port is not found in the DB, then it was possibly an L3 port
         * which became L2 when the daemon crashed. Remove all IP addresses
         * configured on that interface from the kernel */
        if (!db_port) {
            VLOG_DBG("Port %s is no longer L3. Deleting IP addresses"
                     " from kernel", kernel_port->name);
            HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                               &kernel_port->ip4addr) {
                portd_set_ipaddr(RTM_DELADDR, kernel_port->name,
                        addr->address, AF_INET, false);
            }

            HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                               &kernel_port->ip6addr) {
                portd_set_ipaddr(RTM_DELADDR, kernel_port->name,
                        addr->address, AF_INET6, false);
            }
            continue;
        }
        /* If port exists in the DB, then we will add to local cache.
         * This is needed so that portd does not reconfigure the
         * interfaces again. */
        add_to_local_cache = true;

        /* Remove all the IP addresses from the kernel that are not
         * present in the DB */
        HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                           &kernel_port->ip4addr) {
            if (!portd_find_ip_addr_db(db_port,
                    addr->address, false)) {
                portd_set_ipaddr(RTM_DELADDR, db_port->name,
                        addr->address, AF_INET, false);
            }
        }

        HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                           &kernel_port->ip6addr) {
            if (!portd_find_ip_addr_db(db_port,
                    addr->address, true)) {
                portd_set_ipaddr(RTM_DELADDR, db_port->name,
                        addr->address, AF_INET6, false);
            }
        }

        /* Check for IP addresses which are not present and add them */
        if (db_port->ip4_address) {
            if (!portd_find_ip_addr_kernel(kernel_port,
                    db_port->ip4_address, false)) {
                portd_set_ipaddr(RTM_NEWADDR, db_port->name,
                        db_port->ip4_address, AF_INET, false);
            }
        }
        if (db_port->ip6_address) {
            if (!portd_find_ip_addr_kernel(kernel_port,
                    db_port->ip6_address, true)) {
                portd_set_ipaddr(RTM_NEWADDR, db_port->name,
                        db_port->ip6_address, AF_INET6, false);
            }
        }
        HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                           &db_port->secondary_ip4addr) {
            if (!portd_find_ip_addr_kernel(kernel_port,
                    addr->address, false)) {
                portd_set_ipaddr(RTM_NEWADDR, db_port->name,
                        addr->address, AF_INET, true);
            }
        }
        HMAP_FOR_EACH_SAFE(addr, next_addr, addr_node,
                           &db_port->secondary_ip6addr) {
            if (!portd_find_ip_addr_kernel(kernel_port,
                    addr->address, true)) {
                portd_set_ipaddr(RTM_NEWADDR, db_port->name,
                        addr->address, AF_INET6, true);
            }
        }

         if (add_to_local_cache) {
            add_to_local_cache = false;
            portd_add_port_to_cache(db_port);
        }
        /* Free kernel port */
        hmap_destroy(&kernel_port->ip4addr);
        hmap_destroy(&kernel_port->ip6addr);
        free(kernel_port->name);
        free(kernel_port);
        kernel_port = NULL;
    }
    shash_destroy(&kernel_port_list);
    shash_destroy(&db_port_list);
}