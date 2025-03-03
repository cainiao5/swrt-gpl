/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <bcmnvram.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/if_ether.h>	//have in front of <linux/mii.h> to avoid redefinition of 'struct ethhdr'
#include <linux/mii.h>
#include <dirent.h>

#include <shutils.h>
#include <shared.h>
#include <utils.h>
#include <qca.h>

#define NR_WANLAN_PORT	6
#define DBGOUT		NULL			/* "/dev/console" */


static const char *upstream_iptv_ifaces[16] = {
#if defined(RAX120)
	[WANS_DUALWAN_IF_WAN] = "eth4",
	[WANS_DUALWAN_IF_WAN2] = "eth5",
#else
#error Define WAN interfaces that can be used as upstream port of IPTV.
#endif
};

#if defined(RAX120)
/* RAX120 virtual port mapping
 * Assume LAN port closed to 1G WAN port is LAN1.
 */
enum {
	LAN1_PORT=0,
	LAN2_PORT,
	LAN3_PORT,
	LAN4_PORT,
	WAN_PORT=4,
	WAN5GR_PORT,

	MAX_WANLAN_PORT
};
#else
#error Define WAN/LAN ports!
#endif

/* array index:		virtual port mapping enumeration.
 * 			e.g. LAN1_PORT, LAN2_PORT, etc.
 * array element:	PHY address, negative value means absent PHY.
 * 			0x10? means Aquantia AQR11,
 * 			      check it with is_aqr_phy_exist before using it on RAX120.
 * 			0x20? means ethX, ethtool ioctl
 */
#if defined(RAX120)
/* HwId: A */
static const int vport_to_phy_addr_hwid_a[MAX_WANLAN_PORT] = {
	3, 2, 1, 0, 4, 0x107
};
static const int *vport_to_phy_addr = vport_to_phy_addr_hwid_a;

/**
 * The vport_to_iface array is used to get interface name of each virtual
 * port.  If bled need to know TX/RX statistics of LAN1~2, WAN1, WAN2 (AQR107),
 * and 10G SFP+, bled has to find this information from netdev.  So, define
 * this array and implement vport_to_iface_name() function which is used by
 * bled in update_swports_bled().
 *
 * array index:		virtual port mapping enumeration.
 * 			e.g. LAN1_PORT, LAN2_PORT, etc.
 * array element:	Interface name of specific virtual port.
 */
static const char *vport_to_iface[MAX_WANLAN_PORT] = {
	"eth3", "eth2", "eth1", "eth0", "eth4", "eth5"
};
#else
#error FIXME
#endif

/**
 * Convert (v)port to interface name.
 * @vport:	(virtual) port number
 * @return:
 * 	NULL:	@vport doesn't map to specific interface name.
 *  otherwise:	@vport do map to a specific interface name.
 */
const char *vport_to_iface_name(unsigned int vport)
{
	if (vport >= ARRAY_SIZE(vport_to_iface)) {
		dbg("%s: don't know vport %d\n", __func__, vport);
		return NULL;
	}

	return vport_to_iface[vport];
}

/**
 * Convert interface name to (v)port.
 * @iface:	interface name
 * @return:	(virtual) port number
 *  >=0:	(virtual) port number
 *  < 0:	can't find (virtual) port number for @iface.
 */
int iface_name_to_vport(const char *iface)
{
	int ret = -2, i;

	if (!iface)
		return -1;

	for (i = 0; ret < 0 && i < ARRAY_SIZE(vport_to_iface); ++i) {
		if (!vport_to_iface[i] || strcmp(vport_to_iface[i], iface))
			continue;
		ret = i;
	}

	return ret;
}

/* 0:WAN, 1:LAN, 2:WAN2(5G RJ-45), 3: SFP+, first index is switch_stb_x nvram variable.
 * lan_wan_partition[switch_stb_x][0] is virtual port0,
 * lan_wan_partition[switch_stb_x][1] is virtual port1, etc.
 * If it's 2, check it with is_aqr_phy_exist() before using it on RAX120.
 */
static const int lan_wan_partition[8][MAX_WANLAN_PORT] = {
	/* L1, L2, L3, L4, W1G, W5GR */
	{1,1,1,1,0,2}, // Normal
	{0,1,1,1,0,2}, // IPTV STB port = LAN1
	{1,0,1,1,0,2}, // IPTV STB port = LAN2
	{1,1,0,1,0,2}, // IPTV STB port = LAN3
	{1,1,1,0,0,2}, // IPTV STB port = LAN4
	{0,0,1,1,0,2}, // IPTV STB port = LAN1 & LAN2
	{1,1,0,0,0,2}, // IPTV STB port = LAN3 & LAN4
	{1,1,1,1,1,2}  // ALL
};

/* ALL WAN/LAN virtual port bit-mask */
static unsigned int wanlanports_mask = ((1U << WAN_PORT) | (1U << WAN5GR_PORT) | \
	(1U << LAN1_PORT) | (1U << LAN2_PORT) | (1U << LAN3_PORT) | (1U << LAN4_PORT));

/* IPTV virtual port bitmask */
static const unsigned int stb_to_mask[8] = { 
					0,
	(1U << LAN1_PORT),
	(1U << LAN2_PORT),
	(1U << LAN3_PORT),
	(1U << LAN4_PORT),
	(1U << LAN1_PORT) | (1U << LAN2_PORT),
	(1U << LAN3_PORT) | (1U << LAN4_PORT),
	(1U << LAN1_PORT) | (1U << LAN2_PORT) | (1U << LAN3_PORT) | (1U << LAN4_PORT)
};

/* Model-specific LANx ==> Model-specific virtual PortX.
 * array index:	Model-specific LANx (started from 0).
 * array value:	Model-specific virtual port number.
 */
const int lan_id_to_vport[NR_WANLAN_PORT] = {
	LAN1_PORT,
	LAN2_PORT,
	LAN3_PORT,
	LAN4_PORT,
	WAN_PORT,
	WAN5GR_PORT
};

/* Model-specific LANx (started from 0) ==> Model-specific virtual PortX */
static inline int lan_id_to_vport_nr(int id)
{
	//printf("===============id %d is %d\n", id, lan_id_to_vport[id]);
	return lan_id_to_vport[id];
}

/* PHY address => switch port */
static inline int phy_addr_to_sw_port(int phy)
{
	return phy + 1;
}

#if defined(RTCONFIG_BONDING_WAN) || defined(RTCONFIG_LACP)
/* array index:		port number used in wanports_bond, enum bs_port_id.
 * 			0: WAN, 1~4: LAN1~4, 30: 5G base-T (RJ-45), 31: 10G SFP+
 * array element:	virtual port
 * 			e.g. LAN1_PORT ~ LAN4_PORT, WAN_PORT, etc.
 */
static const int bsport_to_vport[32] = {
	WAN_PORT, LAN1_PORT, LAN2_PORT, LAN3_PORT, LAN4_PORT,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	WAN5GR_PORT, -1
};
#endif

void reset_qca_switch(void);

#define	CPU_PORT_WAN_MASK	(1U << WAN_PORT)
#define CPU_PORT_LAN_MASK	(0xF)

/* Final model-specific LAN/WAN/WANS_LAN partition definitions.
 * Because LAN/WAN ports of RAX120 are distributed on several switches and phys.
 * These ports bitmask below are virtual port bitmask for a pseudo switch covers
 * QCA8075, AQR111, and SFP+.
 * bit0: VP0, bit1: VP1, bit2: VP2, bit3: VP3, bit4: VP4, bit5: VP5
 */
static unsigned int lan_mask = 0;	/* LAN only. Exclude WAN, WANS_LAN, and generic IPTV port. */
static unsigned int wan_mask = 0;	/* wan_type = WANS_DUALWAN_IF_WAN. Include generic IPTV port. */
static unsigned int wan2_mask = 0;	/* wan_type = WANS_DUALWAN_IF_WAN2. Include generic IPTV port. */
static unsigned int sfpp_mask = 0;	/* wan_type = WANS_DUALWAN_IF_SFPP. Include generic IPTV port. */
static unsigned int wans_lan_mask = 0;	/* wan_type = WANS_DUALWAN_IF_LAN. */

/* RT-N56U's P0, P1, P2, P3, P4 = LAN4, LAN3, LAN2, LAN1, WAN
 * ==> Model-specific virtual port number.
 * array inex:	RT-N56U's port number.
 * array value:	Model-specific virtual port number
 */
static int n56u_to_model_port_mapping[] = {
	LAN1_PORT,	//0000 0000 0001 LAN1
	LAN2_PORT,	//0000 0000 0010 LAN2
	LAN3_PORT,	//0000 0000 0100 LAN3
	LAN4_PORT,	//0000 0000 1000 LAN4
	WAN_PORT,	//0000 0001 0000 WAN
};

#define RTN56U_WAN_GMAC	(1U << 9)

int esw_stb;

/**
 * Get WAN port mask
 * @wan_unit:	wan_unit, if negative, select WANS_DUALWAN_IF_WAN
 * @return:	port bitmask
 */
static unsigned int get_wan_port_mask(int wan_unit)
{
	int sw_mode = sw_mode();
	char nv[] = "wanXXXports_maskXXXXXX";

	if (sw_mode == SW_MODE_AP || sw_mode == SW_MODE_REPEATER)
#if defined(RTCONFIG_AMAS)
		if (!nvram_match("re_mode", "1"))
#endif	/* RTCONFIG_AMAS */
		return 0;

	if (wan_unit <= 0 || wan_unit >= WAN_UNIT_MAX)
		strlcpy(nv, "wanports_mask", sizeof(nv));
	else
		snprintf(nv, sizeof(nv), "wan%dports_mask", wan_unit);
	return nvram_get_int(nv);
}

/**
 * Get LAN port mask
 * @return:	port bitmask
 */
static unsigned int get_lan_port_mask(void)
{
	int sw_mode = sw_mode();
	unsigned int m = nvram_get_int("lanports_mask");

	if (sw_mode == SW_MODE_AP || __mediabridge_mode(sw_mode))
		m = wanlanports_mask;

	return m;
}

#define IPQ807X_MDIO_SYS_DIR	"/sys/devices/platform/soc/90000.mdio/mdio_bus/90000.mdio/"
#define AQR_PHY_SYS_DIR		IPQ807X_MDIO_SYS_DIR "90000.mdio:07"
int is_aqr_phy_exist(void)
{
	static int aqr_phy_absent = -1;

	if (aqr_phy_absent >= 0)
		return !aqr_phy_absent;

	if (!d_exists(IPQ807X_MDIO_SYS_DIR)) {
		dbg("%s hasn't been created, assume AQR PHY exist!\n", IPQ807X_MDIO_SYS_DIR);
		return 1;
	}
	if (d_exists(AQR_PHY_SYS_DIR))
		aqr_phy_absent = 0;
	else
		aqr_phy_absent = 1;

	return !aqr_phy_absent;
}

/* Return PHY address of AQR111 based on HwId
 * @return:
 * 	-1: error
 *  0 ~ 31: PHY address
 */
int aqr_phy_addr(void)
{
#if defined(RAX120)
        return 7;
#else
#warning FIXME
#endif
}

/**
 * Convert vportmask to real portmask (QCA8337 only).
 * In most platform, vportmask = rportmask.
 * @vportmask:	virtual port mask
 * @return:	real portmask (QCA8337 only)
 */
unsigned int vportmask_to_rportmask(unsigned int vportmask)
{
	return vportmask;
}

enum {
	VLAN_TYPE_STB_VOIP = 0,
	VLAN_TYPE_WAN,
	VLAN_TYPE_WAN_NO_VLAN,	/* 桥接 WAN/STB for 中华电信. */

	VLAN_TYPE_MAX
};
/**
 * Set a VLAN
 * @vtype:	VLAN type
 * 	0:	VLAN for STB/VoIP
 * 	1:	VLAN for WAN
 * @upstream_if:upstream interface name
 * @vid:	VLAN ID, 0 ~ 4095.
 * @prio:	VLAN Priority
 * @mbr:	VLAN members
 * @untag:	VLAN members that need untag
 *
 * @return
 * 	0:	success
 *     -1:	invalid parameter
 */
int qca8075_aqr111_vlan_set(int vtype, char *upstream_if, int vid, int prio, unsigned int mbr, unsigned int untag)
{
	unsigned int upstream_mask = 0;
	int upstream_vport, wan_vlan_br = 0, wan_br = 0;
	char wvlan_if[IFNAMSIZ], vid_str[6], prio_str[4], brv_if[IFNAMSIZ];
	char *set_upst_viface_egress_map[] = { "vconfig", "set_egress_map", wvlan_if, "0", prio_str, NULL };

	dbg("%s: vtype %d upstream_if %s vid %d prio %d mbr 0x%x untag 0x%x\n",
		__func__, vtype, upstream_if, vid, prio, mbr, untag);
	if (!upstream_if || vtype < 0 || vtype >= VLAN_TYPE_MAX) {
		dbg("%s: invalid parameter\n", __func__);
		return -1;
	}

	if (bond_wan_enabled() && !strcmp(upstream_if, "bond1")) {
		uint32_t wmask = nums_str_to_u32_mask(nvram_safe_get("wanports_bond"));
		int b;
		const char *p;

		while ((b = ffs(wmask)) > 0) {
			b--;
			if ((p = bs_port_id_to_iface(b)) != NULL) {
				upstream_vport = iface_name_to_vport(p);
				if (upstream_vport < 0 || upstream_vport >= MAX_WANLAN_PORT) {
					dbg("%s: Can't find vport for upstream iface [%s] of bond1\n", __func__, upstream_if);
					return -1;
				}
				upstream_mask |= 1U << upstream_vport;
			}
			wmask &= ~(1U << b);
		}
	} else {
		upstream_vport = iface_name_to_vport(upstream_if);
		if (upstream_vport < 0 || upstream_vport >= MAX_WANLAN_PORT) {
			dbg("%s: Can't find vport for upstream iface [%s]\n", __func__, upstream_if);
			return -1;
		}
		upstream_mask = 1U << upstream_vport;
	}

	if (vtype == VLAN_TYPE_WAN_NO_VLAN) {
		wan_br = 1;
		vtype = VLAN_TYPE_WAN;
	}
	if (vtype == VLAN_TYPE_WAN && (mbr & ~(1U << WAN_PORT)) != 0)
		wan_vlan_br = 1;

	/* Replace WAN port as selected upstream port. */
	if (mbr & (1U << WAN_PORT)) {
		mbr &= ~(1U << WAN_PORT);
		mbr |= upstream_mask;
	}
	if (untag & (1U << WAN_PORT)) {
		untag &= ~(1U << WAN_PORT);
		untag |= upstream_mask;
	}

	snprintf(vid_str, sizeof(vid_str), "%d", vid);
	snprintf(prio_str, sizeof(prio_str), "%d", prio);
	snprintf(brv_if, sizeof(brv_if), "brv%d", vid);

	if ((vtype == VLAN_TYPE_WAN && wan_vlan_br) || vtype == VLAN_TYPE_STB_VOIP) {
		/* Use bridge to connect WAN and STB/VoIP. */
		eval("brctl", "addbr", brv_if);
		eval("ifconfig", brv_if, "0.0.0.0", "up");

		set_netdev_sysfs_param(brv_if, "bridge/multicast_querier", "1");
		set_netdev_sysfs_param(brv_if, "bridge/multicast_snooping",
			nvram_match("switch_br_no_snooping", "1")? "0" : "1");
	}

	if (vtype == VLAN_TYPE_WAN) {
		if (wan_br) {
			/* In this case, no VLAN on WAN port. */
			strlcpy(wvlan_if, upstream_if, sizeof(wvlan_if));
		} else {
			/* Follow naming rule in set_basic_ifname_vars() on upstream interface. */
			snprintf(wvlan_if, sizeof(wvlan_if), "vlan%d", vid);
			_eval(set_upst_viface_egress_map, DBGOUT, 0, NULL);
			eval("ifconfig", upstream_if, "0.0.0.0", "up");
		}
		if (wan_vlan_br) {
			eval("brctl", "addif", brv_if, wvlan_if);
		}
		eval("ifconfig", wvlan_if, "0.0.0.0", "up");
	} else if (vtype == VLAN_TYPE_STB_VOIP) {
		snprintf(wvlan_if, sizeof(wvlan_if), "%s.%d", upstream_if, vid);
		_eval(set_upst_viface_egress_map, DBGOUT, 0, NULL);
		eval("brctl", "addif", brv_if, wvlan_if);
		eval("ifconfig", wvlan_if, "0.0.0.0", "up");
	}

	return 0;
}

/**
 * Get link status and/or phy speed of a traditional PHY.
 * @phy:	PHY address
 * @link:	pointer to unsigned integer.
 * @speed:	pointer to unsigned integer.
 * 		If speed != NULL,
 * 			*speed = 1 means 100Mbps
 * 			*speed = 2 means 1000Mbps
 * 			*speed = 3 means 10Gbps
 * 			*speed = 4 means 2.5Gbps
 * 			*speed = 5 means 5Gbps
 * @link_speed:	pointer to unsigned integer which is used to store normal link speed.
 * 		e.g. 100 = 100Mbps, 10000 = 10Gbps, etc.
 * @return
 * 	0:	success
 *     -1:	invalid parameter.
 *     -2:	miireg_read() MII_BMSR failed
 */
static int get_phy_info(unsigned int phy, unsigned int *link, unsigned int *speed, unsigned int *link_speed)
{
	int r = 0, l = 1, s = 0, lspd = 0;

	/* Use ssdk_sh port linkstatus/speed get command to minimize number of executed ssdk_sh
	 * and number of accessing MDIO bus.
	 */
	if (phy >= 0 && phy <= 4) {
		r = ipq8074_port_speed(phy_addr_to_sw_port(phy));
	} else if (phy < 0x20) {
		r = mdio_phy_speed(phy);
	} else if (phy >= 0x100 && phy < 0x120) {
		r = aqr_phy_speed(phy & 0x1F);
	} else {
		dbg("%s: Unknown PHY address 0x%x\n", __func__, phy);
	}

	lspd = r;
	switch (r) {
	case 5000:
		s = 5;
		break;
	case 2500:
		s = 4;
		break;
	case 10000:
		s = 3;
		break;
	case 1000:
		s = 2;
		break;
	case 100:
		s = 1;
		break;
	case 10:
		s = 0;
		break;
	default:
		l  = lspd = 0;
		break;
	}

	if (link)
		*link = l;
	if (speed)
		*speed = s;
	if (link_speed)
		*link_speed = lspd;

	return 0;
}

/**
 * Get link status and/or phy speed of a virtual port.
 * @vport:	virtual port number
 * @link:	pointer to unsigned integer.
 * 		If link != NULL,
 * 			*link = 0 means link-down
 * 			*link = 1 means link-up.
 * @speed:	pointer to unsigned integer.
 * 		If speed != NULL,
 * 			*speed = 1 means 100Mbps
 * 			*speed = 2 means 1000Mbps
 * @return:
 * 	0:	success
 *     -1:	invalid parameter
 *  otherwise:	fail
 */
static int get_qca8075_aqr111_vport_info(unsigned int vport, unsigned int *link, unsigned int *speed)
{
	int phy;

	if (vport >= MAX_WANLAN_PORT || (!link && !speed))
		return -1;

	if (link)
		*link = 0;
	if (speed)
		*speed = 0;

	if (vport == WAN5GR_PORT && !is_aqr_phy_exist())
		return 0;

	phy = *(vport_to_phy_addr + vport);
	if (phy < 0) {
		dbg("%s: can't get PHY address of vport %d\n", __func__, vport);
		return -1;
	}

	get_phy_info(phy, link, speed, NULL);

	return 0;
}

/**
 * Get linkstatus in accordance with port bit-mask.
 * @mask:	port bit-mask.
 * 		bit0 = VP0, bit1 = VP1, etc.
 * @linkStatus:	link status of all ports that is defined by mask.
 * 		If one port of mask is linked-up, linkStatus is true.
 * @return:	speed.
 * 	0:	not connected.
 *  non-zero:	linkrate.
 */
static int get_qca8075_aqr111_mask_linkStatus(unsigned int mask, unsigned int *linkStatus)
{
	int i,t,speed=0;
	unsigned int value = 0, m;

	m = mask & wanlanports_mask;
	for (i = 0; m > 0 && !value; ++i, m >>= 1) {
		if (!(m & 1))
			continue;

		get_qca8075_aqr111_vport_info(i, &value, (unsigned int*) &t);
		value &= 0x1;
	}
	*linkStatus = value;

	switch (t) {
	case 0x0:
		speed = 10;
		break;
	case 0x1:
		speed = 100;
		break;
	case 0x2:
		speed = 1000;
		break;
	case 0x3:
		speed = 10000;
		break;
	case 0x4:
		speed = 2500;
		break;
	case 0x5:
		speed = 5000;
		break;
	default:
		speed=0;
		_dprintf("%s: mask %8x t %8x invalid speed!\n", __func__, mask, t);
	}
	return speed;
}


/**
 * Set wanports_mask, wanXports_mask, and lanports_mask based on
 * @stb：
 * 	0:	Default.
 * 	1:	LAN1
 * 	2:	LAN2
 * 	3:	LAN3
 * 	4:	LAN4
 * 	5:	LAN1+LAN2
 * 	6:	LAN3+LAN4
 * @stb_bitmask parameters.
 * @stb_bitmask should be platform-specific (v)port bitmask.
 */
static void build_wan_lan_mask(int stb, int stb_bitmask)
{
	int i, unit, type, m, upstream_unit = __get_upstream_wan_unit();
	int wanscap_lan = get_wans_dualwan() & WANSCAP_LAN;
	int wans_lanport = nvram_get_int("wans_lanport");
	int sw_mode = sw_mode();
	char prefix[8], nvram_ports[20];
	unsigned int unused_wan_mask = 0, iptv_mask = 0;

	if (stb < 0 || stb >= ARRAY_SIZE(stb_to_mask)) {
		return;
	}
	if (sw_mode == SW_MODE_AP || sw_mode == SW_MODE_REPEATER)
		wanscap_lan = 0;

	if (wanscap_lan && (wans_lanport < 0 || wans_lanport > 4)) {
		_dprintf("%s: invalid wans_lanport %d!\n", __func__, wans_lanport);
		wanscap_lan = 0;
	}

	/* To compatible to original architecture, @stb and @stb_bitmask are exclusive.
	 * @stb is assumed as zero if @stb_bitmask is non-zero value.  Because
	 * "rtkswitch  8 X" specifies STB port configuration via 0 ~ 6 and
	 * "rtkswitch 38 X" specifies VoIP/STB port via RT-N56U's port bitmask.
	 */
	if (stb_bitmask != 0)
		stb = 0;
	if (sw_mode == SW_MODE_ROUTER) {
		if (stb_bitmask != 0)
			iptv_mask = stb_bitmask;
		else
			iptv_mask = stb_to_mask[stb];
	}

	lan_mask = wan_mask = wans_lan_mask = 0;
	for (i = 0; i < NR_WANLAN_PORT; ++i) {
		switch (lan_wan_partition[stb][i]) {
		case 0:
			wan_mask |= 1U << lan_id_to_vport_nr(i);
			break;
		case 1:
			lan_mask |= 1U << lan_id_to_vport_nr(i);
			break;
		case 2:
			if (is_aqr_phy_exist())
				wan2_mask |= 1U << lan_id_to_vport_nr(i);
			break;
		case 3:
			sfpp_mask |= 1U << lan_id_to_vport_nr(i);
			break;
		default:
			_dprintf("%s: Unknown LAN/WAN port definition. (stb %d i %d val %d)\n",
				__func__, stb, i, lan_wan_partition[stb][i]);
		}
	}

	/* One of LAN port is acting as WAN. */
	if (wanscap_lan) {
		wans_lan_mask = 1U << lan_id_to_vport_nr(wans_lanport);
		lan_mask &= ~wans_lan_mask;
	}

	unused_wan_mask = wan_mask | wan2_mask | sfpp_mask;
	for (unit = WAN_UNIT_FIRST; unit < WAN_UNIT_MAX; ++unit) {
		snprintf(prefix, sizeof(prefix), "%d", unit);
		snprintf(nvram_ports, sizeof(nvram_ports), "wan%sports_mask", (unit == WAN_UNIT_FIRST)?"":prefix);
		m = 0;	/* In AP/RP/MB mode, all WAN ports are bridged to LAN. */

		if (sw_mode == SW_MODE_ROUTER
#ifdef RTCONFIG_AMAS
		 ||(sw_mode == SW_MODE_AP && nvram_match("re_mode", "1"))
#endif	/* RTCONFIG_AMAS */
		 ) {
			type = get_dualwan_by_unit(unit);

			switch (type) {
			case WANS_DUALWAN_IF_WAN:
				m = wan_mask;
				unused_wan_mask &= ~wan_mask;
				break;
			case WANS_DUALWAN_IF_WAN2:
				m = wan2_mask;
				unused_wan_mask &= ~wan2_mask;
				break;
			case WANS_DUALWAN_IF_SFPP:
				m = sfpp_mask;
				unused_wan_mask &= ~sfpp_mask;
				break;
			case WANS_DUALWAN_IF_LAN:
				m = wans_lan_mask;
				break;
			default:
				nvram_unset(nvram_ports);
				break;
			}

			if (m == 0)
				continue;

			if (unit == upstream_unit)
				m |= iptv_mask;
		}
		nvram_set_int(nvram_ports, m);
	}

	/* Let all unused WAN ports become LAN ports.
	 * 1. 10G RJ-45 and 10G SFP+ can be WAN or LAN, depends on dualwan configuration.
	 * 2. 1G WAN can aggreate with LAN1/LAN2, creates 3Gbps bandwidth. (TODO)
	 */
	lan_mask = (lan_mask | unused_wan_mask) & ~iptv_mask;
	nvram_set_int("lanports_mask", lan_mask);
}

/**
 * Configure LAN/WAN partition base on generic IPTV type.
 * @type:
 * 	0:	Default.
 * 	1:	LAN1
 * 	2:	LAN2
 * 	3:	LAN3
 * 	4:	LAN4
 * 	5:	LAN1+LAN2
 * 	6:	LAN3+LAN4
 */
static void config_qca8075_aqr111_LANWANPartition(int type)
{
	build_wan_lan_mask(type, 0);
	reset_qca_switch();
	dbg("%s: LAN/WAN1/WAN2 portmask %08x/%08x/%08x Upstream %s (unit %d)\n",
		__func__, lan_mask, nvram_get_int("wanports_mask"), nvram_get_int("wan1ports_mask"),
		get_wan_base_if(), __get_upstream_wan_unit());
}

static void get_qca8075_aqr111_WAN_Speed(unsigned int *speed)
{
	int i, v = -1, t;
	unsigned int m;

	m = (get_wan_port_mask(0) | get_wan_port_mask(1)) & wanlanports_mask;
	for (i = 0; m; ++i, m >>= 1) {
		if (!(m & 1))
			continue;

		get_qca8075_aqr111_vport_info(i, NULL, (unsigned int*) &t);
		t &= 0x3;
		if (t > v)
			v = t;
	}

	switch (v) {
	case 0x0:
		*speed = 10;
		break;
	case 0x1:
		*speed = 100;
		break;
	case 0x2:
		*speed = 1000;
		break;
	case 0x3:
		*speed = 10000;
		break;
	case 0x4:
		*speed = 2500;
		break;
	case 0x5:
		*speed = 5000;
		break;
	default:
		_dprintf("%s: invalid speed! (%d)\n", __func__, v);
	}
}

/**
 * @vpmask:	Virtual port mask
 * @status:	0: power down PHY; otherwise: power up PHY
 */
static void link_down_up_qca8075_aqr111_PHY(unsigned int vpmask, int status)
{
	int vport, phy, r;
	unsigned int m;
	char iface[IFNAMSIZ];

	vpmask &= wanlanports_mask;
	for (vport = 0, m = vpmask; m; ++vport, m >>= 1) {
		if (!(m & 1))
			continue;
		if (vport >= MAX_WANLAN_PORT) {
			dbg("%s: PHY address is not defined for vport %d\n", __func__, vport);
			continue;
		}

		if (vport == WAN5GR_PORT && !is_aqr_phy_exist())
			return;
		phy = *(vport_to_phy_addr + vport);
		if (phy < 0) {
			dbg("%s: can't get PHY address of vport %d\n", __func__, vport);
			return;
		}

		if (phy < 0x20) {
			/* Legacy PHY and QCA8337 PHY */
			if ((r = read_phy_reg(phy, MII_BMCR)) < 0)
				r = 0;
			if (!status)	/* power down PHY */
				r |= BMCR_PDOWN;
			else
				r &= ~(BMCR_PDOWN);
			write_phy_reg(phy, MII_BMCR, r);
		} else if (phy >= 0x100 && phy < 0x120) {
			/* Aquantia PHY */
			if ((r = read_phy_reg(phy & 0x1F, 0x40010009)) < 0)
				r = 0;
			if (!status)	/* power down PHY */
				r |= 1;
			else
				r &= ~(1);
			write_phy_reg(phy & 0x1F, 0x40010009, r);
		} else if (phy >= 0x200 && phy < 0x210) {
			snprintf(iface, sizeof(iface), "eth%d", phy & 0xF);
			eval("ifconfig", iface, (!status)? "down" : "up");
		} else {
			dbg("%s: Unknown PHY address 0x%x\n", __func__, phy);
		}
	}
}

void reset_qca_switch(void)
{
	nvram_unset("vlan_idx");
}

static void set_Vlan_VID(int vid)
{
	char tmp[8];

	snprintf(tmp, sizeof(tmp), "%d", vid);
	nvram_set("vlan_vid", tmp);
}

static void set_Vlan_PRIO(int prio)
{
	char tmp[2];

	snprintf(tmp, sizeof(tmp), "%d", prio);
	nvram_set("vlan_prio", tmp);
}

static int convert_n56u_portmask_to_model_portmask(unsigned int orig)
{
	int i, bit, bitmask;
	bitmask = 0;
	for(i = 0; i < ARRAY_SIZE(n56u_to_model_port_mapping); i++) {
		bit = (1 << i);
		if (orig & bit)
			bitmask |= (1 << n56u_to_model_port_mapping[i]);
	}
	return bitmask;
}

/**
 * @stb_bitmask:	bitmask of STB port(s)
 * 			e.g. bit0 = P0, bit1 = P1, etc.
 */
static void initialize_Vlan(int stb_bitmask)
{
	char *p, wan_base_if[IFNAMSIZ] = "N/A";

	stb_bitmask = convert_n56u_portmask_to_model_portmask(stb_bitmask);
	build_wan_lan_mask(0, stb_bitmask);
	if ((p = get_wan_base_if()) != NULL)
		strlcpy(wan_base_if, p, sizeof(wan_base_if));

	dbg("%s: LAN/P.WAN/S.WAN portmask %08x/%08x/%08x Upstream %s (unit %d)\n",
		__func__, lan_mask, nvram_get_int("wanports_mask"), nvram_get_int("wan1ports_mask"),
		get_wan_base_if(), __get_upstream_wan_unit());
	reset_qca_switch();
}

/**
 * Create VLAN for LAN and/or WAN in accordance with bitmask parameter.
 * @bitmask:
 *  bit15~bit0:		member port bitmask.
 * 	bit0:		RT-N56U port0, LAN4
 * 	bit1:		RT-N56U port1, LAN3
 * 	bit2:		RT-N56U port2, LAN2
 * 	bit3:		RT-N56U port3, LAN1
 * 	bit4:		RT-N56U port4, WAN
 * 	bit8:		RT-N56U port8, LAN_CPU port
 * 	bit9:		RT-N56U port9, WAN_CPU port
 *  bit31~bit16:	untag port bitmask.
 * 	bit16:		RT-N56U port0, LAN4
 * 	bit17:		RT-N56U port1, LAN3
 * 	bit18:		RT-N56U port2, LAN2
 * 	bit19:		RT-N56U port3, LAN1
 * 	bit20:		RT-N56U port4, WAN
 * 	bit24:		RT-N56U port8, LAN_CPU port
 * 	bit25:		RT-N56U port9, WAN_CPU port
 * First Ralink-based model is RT-N56U.
 * Convert RT-N56U-specific bitmask to physical port of your model,
 * base on relationship between physical port and visual WAN/LAN1~4 of that model first.
 */
static void create_Vlan(int bitmask)
{
	const int vid = nvram_get_int("vlan_vid");
	const int prio = nvram_get_int("vlan_prio") & 0x7;
	const int stb_x = nvram_get_int("switch_stb_x");
	unsigned int mbr = bitmask & 0xffff;
	unsigned int untag = (bitmask >> 16) & 0xffff;
	unsigned int mbr_qca, untag_qca;
	int vtype = VLAN_TYPE_STB_VOIP;
	char upstream_if[IFNAMSIZ];

	//convert port mapping
	mbr_qca   = convert_n56u_portmask_to_model_portmask(mbr);
	untag_qca = convert_n56u_portmask_to_model_portmask(untag);
	if ((nvram_match("switch_wantag", "none") && stb_x > 0) ||
	    nvram_match("switch_wantag", "hinet")) {
		vtype = VLAN_TYPE_WAN_NO_VLAN;
	} else if (mbr & RTN56U_WAN_GMAC) {
		/* setup VLAN for WAN (WAN1 or WAN2), not VoIP/STB */
		vtype = VLAN_TYPE_WAN;
	}

	/* selecet upstream port for IPTV port. */
	strlcpy(upstream_if, get_wan_base_if(), sizeof(upstream_if));
	qca8075_aqr111_vlan_set(vtype, upstream_if, vid, prio, mbr_qca, untag_qca);
}

unsigned int
rtkswitch_Port_phyLinkRate(unsigned int port_mask)
{
	unsigned int speed = 0 ,status = 0;

	speed=get_qca8075_aqr111_mask_linkStatus(port_mask, &status);

	return speed;
}


int qca8075_aqr111_ioctl(int val, int val2)
{
	unsigned int value2 = 0;
	int i, max_wan_unit = 0;

#if defined(RTCONFIG_DUALWAN)
	max_wan_unit = 1;
#endif

	switch (val) {
	case 0:
		value2 = rtkswitch_wanPort_phyStatus(-1);
		printf("WAN link status : %u\n", value2);
		break;
	case 3:
		value2 = rtkswitch_lanPorts_phyStatus();
		printf("LAN link status : %u\n", value2);
		break;
	case 8:
		config_qca8075_aqr111_LANWANPartition(val2);
		break;
	case 13:
		get_qca8075_aqr111_WAN_Speed(&value2);
		printf("WAN speed : %u Mbps\n", value2);
		break;
	case 14: // Link up LAN ports
		link_down_up_qca8075_aqr111_PHY(get_lan_port_mask(), 1);
		break;
	case 15: // Link down LAN ports
		link_down_up_qca8075_aqr111_PHY(get_lan_port_mask(), 0);
		break;
	case 16: // Link up ALL ports
		link_down_up_qca8075_aqr111_PHY(wanlanports_mask, 1);
		break;
	case 17: // Link down ALL ports
		link_down_up_qca8075_aqr111_PHY(wanlanports_mask, 0);
		break;
	case 27:
		reset_qca_switch();
		break;
	case 36:
		set_Vlan_VID(val2);
		break;
	case 37:
		set_Vlan_PRIO(val2);
		break;
	case 38:
		initialize_Vlan(val2);
		break;
	case 39:
		create_Vlan(val2);
		break;
	case 114: // link up WAN ports
		for (i = WAN_UNIT_FIRST; i <= max_wan_unit; ++i)
			link_down_up_qca8075_aqr111_PHY(get_wan_port_mask(i), 1);
		break;
	case 115: // link down WAN ports
		for (i = WAN_UNIT_FIRST; i <= max_wan_unit; ++i)
			link_down_up_qca8075_aqr111_PHY(get_wan_port_mask(i), 0);
		break;
	case 200:	/* set LAN port number that is used as WAN port */
		/* Nothing to do, nvram_get_int("wans_lanport ") is enough. */
		break;

	/* unused ioctl command. */
	case 21:	/* reset storm control rate, only Realtek switch platform need. */
	case 22:	/* set unknown unicast storm control rate. RTK switch only. */
	case 23:	/* set unknown multicast storm control rate. RTK switch only. */
	case 24:	/* set multicast storm control rate. RTK switch only. */
	case 25:	/* set broadcast storm rate. RTK switch only. */
	case 29:	/* Set VoIP port.  Not using any more. */
	case 40:
	case 50:	/* Fix-up hwnat for WiFi interface on MTK platform. */
		break;
	default:
		printf("wrong ioctl cmd: %d\n", val);
	}

	return 0;
}

int config_rtkswitch(int argc, char *argv[])
{
	int val;
	int val2 = 0;
	char *cmd = NULL;
	char *cmd2 = NULL;

	if (argc >= 2)
		cmd = argv[1];
	else
		return -1;
	if (argc >= 3)
		cmd2 = argv[2];

	val = (int) strtol(cmd, NULL, 0);
	if (cmd2)
		val2 = (int) strtol(cmd2, NULL, 0);
	return qca8075_aqr111_ioctl(val, val2);
}

unsigned int
rtkswitch_wanPort_phyStatus(int wan_unit)
{
	unsigned int status = 0;

#if defined(RTCONFIG_BONDING_WAN)
	if (bond_wan_enabled() && sw_mode() == SW_MODE_ROUTER
	 && get_dualwan_by_unit(wan_unit) == WANS_DUALWAN_IF_WAN)
	{
		int r = ethtool_glink("bond1");
		if (r >= 0)
			status = r;
		return status;
	}
#endif

	get_qca8075_aqr111_mask_linkStatus(get_wan_port_mask(wan_unit), &status);

	return status;
}

unsigned int
rtkswitch_lanPorts_phyStatus(void)
{
	unsigned int status = 0;

	get_qca8075_aqr111_mask_linkStatus(get_lan_port_mask(), &status);

	return status;
}

unsigned int
rtkswitch_WanPort_phySpeed(void)
{
	unsigned int speed;

	get_qca8075_aqr111_WAN_Speed(&speed);

	return speed;
}

int rtkswitch_WanPort_linkUp(void)
{
	eval("rtkswitch", "114");

	return 0;
}

int rtkswitch_WanPort_linkDown(void)
{
	eval("rtkswitch", "115");

	return 0;
}

int
rtkswitch_LanPort_linkUp(void)
{
	system("rtkswitch 14");

	return 0;
}

int
rtkswitch_LanPort_linkDown(void)
{
	system("rtkswitch 15");

	return 0;
}

int
rtkswitch_AllPort_linkUp(void)
{
	system("rtkswitch 16");

	return 0;
}

int
rtkswitch_AllPort_linkDown(void)
{
	system("rtkswitch 17");

	return 0;
}

int
rtkswitch_Reset_Storm_Control(void)
{
	system("rtkswitch 21");

	return 0;
}

/**
 * @link:
 * 	0:	no-link
 * 	1:	link-up
 * @speed:
 * 	0,10:		10Mbps		==> 'M'
 * 	1,100:		100Mbps		==> 'M'
 * 	2,1000:		1000Mbps	==> 'G'
 * 	3,10000:	10Gbps		==> 'T'
 * 	4,2500:		2.5Gbps		==> 'Q'
 * 	5,5000:		5Gbps		==> 'F'
 */
static char conv_speed(unsigned int link, unsigned int speed)
{
	char ret = 'X';

	if (link != 1)
		return ret;

	if (speed == 2 || speed == 1000)
		ret = 'G';
	else if (speed == 3 || speed == 10000)
		ret = 'T';
	else if (speed == 4 || speed == 2500)
		ret = 'Q';
	else if (speed == 5 || speed == 5000)
		ret = 'F';
	else
		ret = 'M';

	return ret;
}

void ATE_port_status(void)
{
	int i;
	char buf[6 * 11], wbuf[6 * 3], lbuf[6 * 8];
	phyState pS;
	const int wan1g_sfp10g = 0;

	memset(&pS, 0, sizeof(pS));
	for (i = 0; i < NR_WANLAN_PORT; i++) {
		get_qca8075_aqr111_vport_info(lan_id_to_vport_nr(i), &pS.link[i], &pS.speed[i]);
	}

	snprintf(lbuf, sizeof(lbuf), "L1=%C;L2=%C;L3=%C;L4=%C;",
		conv_speed(pS.link[LAN1_PORT], pS.speed[LAN1_PORT]),
		conv_speed(pS.link[LAN2_PORT], pS.speed[LAN2_PORT]),
		conv_speed(pS.link[LAN3_PORT], pS.speed[LAN3_PORT]),
		conv_speed(pS.link[LAN4_PORT], pS.speed[LAN4_PORT]));
	if (wan1g_sfp10g) {
		snprintf(wbuf, sizeof(wbuf), "W0=%C;",
			conv_speed(pS.link[WAN_PORT], pS.speed[WAN_PORT]));
	} else {
		snprintf(wbuf, sizeof(wbuf), "W0=%C;W1=%C;",
			conv_speed(pS.link[WAN_PORT], pS.speed[WAN_PORT]),
			conv_speed(pS.link[WAN5GR_PORT], pS.speed[WAN5GR_PORT]));
	}

	strlcpy(buf, wbuf, sizeof(buf));
	strlcat(buf, lbuf, sizeof(buf));

	puts(buf);
}

/* Callback function which is used to fin brvX interface, X must be number.
 * @return:
 * 	0:	d->d_name is not brvX interface.
 *  non-zero:	d->d_name is brvx interface.
 */
static int brvx_filter(const struct dirent *d)
{
	const char *p;

	if (!d || strncmp(d->d_name, "brv", 3))
		return 0;

	p = d->d_name + 3;
	while (*p != '\0') {
		if (!isdigit(*p))
			return 0;
		p++;
	}

	return 1;
}

void __pre_config_switch(void)
{
	const int *paddr;
	int i, j, r1, nr_brvx, nr_brif;
	struct dirent **brvx = NULL, **brif = NULL;
	char brif_path[sizeof("/sys/class/net/X/brifXXXXX") + IFNAMSIZ];
	char *aqr_ssdk_port = "6"; /* GMAC5, AQR107 */
	char *autoneg[] = { "ssdk_sh", SWID_IPQ807X, "port", "autoNeg", "restart", aqr_ssdk_port, NULL };

	_eval(autoneg, DBGOUT, 0, NULL);

	int aqr_addr = aqr_phy_addr();

	/* Print AQR firmware version. */
	for (i = 0, paddr = vport_to_phy_addr; i < MAX_WANLAN_PORT; ++i, ++paddr) {
		int fw, build;

		/* only accept AQR phy */
		if (*paddr < 0x100 || *paddr >= 0x120)
			continue;

		fw = read_phy_reg(*paddr & 0xFF, 0x401e0020);
		build = read_phy_reg(*paddr & 0xFF, 0x401ec885);
		if (fw < 0  || build < 0) {
			dbg("Can't get AQR PHY firmware version.\n");
		} else {
			dbg("AQR PHY @ %d firmware %d.%d build %d.%d\n", *paddr & 0xFF, (fw >> 8) & 0xFF, fw & 0xFF, (build >> 4) & 0xF, build & 0xF);
		}
	}

	/* Remove all brvXXX bridge interfaces that are used to bridge WAN and STB/VoIP. */
	nr_brvx = scandir(SYS_CLASS_NET, &brvx, brvx_filter, alphasort);
	for (i = 0; i < nr_brvx; ++i) {
		snprintf(brif_path, sizeof(brif_path), "%s/%s/brif", SYS_CLASS_NET, brvx[i]->d_name);
		nr_brif = scandir(brif_path, &brif, NULL, alphasort);
		if (nr_brif <= 0) {
			free(brvx[i]);
			continue;
		}

		for (j = 0; j < nr_brif; ++j) {
			eval("brctl", "delif", brvx[i]->d_name, brif[j]->d_name);
		}
		free(brif);
		free(brvx[i]);
	}
	free(brvx);

}

void __post_config_switch(void)
{
	char *ipq807x_p1_8023az[] = { "ssdk_sh", SWID_IPQ807X, "port", "ieee8023az", "set", "1", "disable", NULL };
	/* Always turn off IEEE 802.3az support on IPQ8074 port 1 and QCA8337 port 1~5. */
	_eval(ipq807x_p1_8023az, DBGOUT, 0, NULL);
}

void __post_start_lan(void)
{
	char br_if[IFNAMSIZ];

	strlcpy(br_if, nvram_get("lan_ifname")? : nvram_default_get("lan_ifname"), sizeof(br_if));
	set_netdev_sysfs_param(br_if, "bridge/multicast_querier", "1");
	set_netdev_sysfs_param(br_if, "bridge/multicast_snooping",
		nvram_match("switch_br0_no_snooping", "1")? "0" : "1");
}

void __post_start_lan_wl(void)
{
	__post_start_lan();
}

int __sw_based_iptv(void)
{
	/* GT-AXY16000 always use software bridge to implement IPTV feature.
	 * If we support LAN3~8 as upstream port of IPTV and all VoIP/STB
	 * ports on LAN3~8, we can support IPTV by just configure QCA8337
	 * switch.  Return zero if all above conditions true.
	 */
	return 1;
}

/**
 * GT-AXY16000's WAN1~2/LAN1~2 are IPQ807x ports and LAN3~8 are QCA8337 ports.
 * If IPTV upstream port is WAN1~2 and QCA8337 port is selected as STB port,
 * e.g. all ISP IPTV profiles or pure STB port(s) on LAN3/4, or IPTV upstream
 * port is LAN5~8 (not supported yet) and IPQ807X port is selected as STB
 * port, we need to use VLAN to seperate ingress frames of STB port.  VLAN ID
 * is returned by get_sw_bridge_iptv_vid().
 * @return:
 * 	0:	WAN/IPTV ports on same switch or IPTV is not enabled.
 * 	1:	IPTV is enabled, WAN and IPTV port are different switch.
 */
int __sw_bridge_iptv_different_switches(void)
{
	int stb_x = nvram_get_int("switch_stb_x");
	const int upstream_wanif = get_dualwan_by_unit(get_upstream_wan_unit()); /* WANS_DUALWAN_IF_XXX */
	const char *no_lan_port_profiles[] = { "spark", "2degrees", "slingshot", "orcon", "voda_nz",
		"tpg", "iinet", "aapt", "intronode", "amaysim", "dodo", "iprimus", NULL
	}, **p;
	char switch_wantag[65];

	strlcpy(switch_wantag, nvram_safe_get("switch_wantag"), sizeof(switch_wantag));
	if (*switch_wantag == '\0')
		return 0;

	if (stb_x < 0 || stb_x > 6)
		stb_x = 0;

	/* If ISP IPTV profile doesn't need any LAN port, return 0. */
	for (p = &no_lan_port_profiles[0]; *p != NULL; ++p) {
		if (!strcmp(switch_wantag, *p))
			return 0;
	}

	if (upstream_wanif == WANS_DUALWAN_IF_WAN || upstream_wanif == WANS_DUALWAN_IF_WAN2) {
		if (!strcmp(switch_wantag, "none") &&
		    (stb_x == 1 || stb_x == 2 || stb_x == 5))	/* LAN1,2,1+2 */
			return 0;
		return 1;					/* LAN3,4,3+4 and all ISP IPTV profiles */
	} else if (upstream_wanif == WANS_DUALWAN_IF_LAN) {
		if (!strcmp(switch_wantag, "none") &&
		    (stb_x == 1 || stb_x == 2 || stb_x == 5))	/* LAN1,2,1+2 */
			return 1;
		return 0;					/* LAN3,4,3+4 and all ISP IPTV profiles */
	}

	dbg("%s: unknown iptv upstream wanif type [%d]\n", __func__, upstream_wanif);

	return 0;
}

/* Return wan_base_if for start_vlan() and selectable upstream port for IPTV.
 * @wan_base_if:	pointer to buffer, minimal length is IFNAMSIZ.
 * @return:		pointer to base interface name for start_vlan().
 */
char *__get_wan_base_if(char *wan_base_if)
{
	int unit, wanif_type;

	if (!wan_base_if)
		return NULL;

	/* Select upstream port of IPTV profile based on configuration at run-time. */
	*wan_base_if = '\0';
	for (unit = WAN_UNIT_FIRST; *wan_base_if == '\0' && unit < WAN_UNIT_MAX; ++unit) {
		wanif_type = get_dualwan_by_unit(unit);
		if (!upstream_iptv_ifaces[wanif_type] || *upstream_iptv_ifaces[wanif_type] == '\0')
			continue;

#if defined(RTCONFIG_BONDING_WAN)
		if (wanif_type == WANS_DUALWAN_IF_WAN && sw_mode() == SW_MODE_ROUTER && bond_wan_enabled()) {
			strlcpy(wan_base_if, "bond1", IFNAMSIZ);
		} else
#endif
			strlcpy(wan_base_if, upstream_iptv_ifaces[wanif_type], IFNAMSIZ);
	}

	return wan_base_if;
}

/* Return wan unit of upstream port for IPTV.
 * This function must same wan unit that returned by get_wan_base_if()!
 * @return:	wan unit of upstream port of IPTV.
 */
int __get_upstream_wan_unit(void)
{
	int i, wanif_type, unit = -1;

	for (i = WAN_UNIT_FIRST; unit < 0 && i < WAN_UNIT_MAX; ++i) {
		wanif_type = get_dualwan_by_unit(i);
		if (wanif_type != WANS_DUALWAN_IF_WAN &&
		    wanif_type != WANS_DUALWAN_IF_WAN2)
			continue;

		unit = i;
	}

	return unit;
}

#if defined(RTCONFIG_BONDING_WAN)
/** Helper function of get_bonding_port_status().
 * Convert bonding slave port definition that is used in wanports_bond to our virtual port definition
 * and get link status/speed of it.
 * @bs_port:	bonding slave port number, 0: WAN, 1~8: LAN1~8, 30: 10G base-T (RJ-45), 31: 10G SFP+
 * @return:
 *  <= 0:	disconnected
 *  otherwise:	link speed
 */
int __get_bonding_port_status(enum bs_port_id bs_port)
{
	int vport, phy, link = 0, speed = 0;

	if (bs_port < 0 || bs_port >= ARRAY_SIZE(bsport_to_vport))
		return 0;

	vport = bsport_to_vport[bs_port];
	if (vport == WAN5GR_PORT && !is_aqr_phy_exist())
		return 0;

	phy = vport_to_phy_addr[vport];
	if (phy < 0) {
		dbg("%s: can't get PHY address of vport %d\n", __func__, vport);
		return 0;
	}
	get_phy_info(phy, &link, NULL, &speed);

	return link? speed : 0;
}
#endif

#if defined(RTCONFIG_BONDING_WAN) || defined(RTCONFIG_LACP)
/** Convert bs_port_id to interface name.
 * @bs_port:	enum bs_port_id
 * @return:	pointer to interface name or NULL.
 *  NULL:	@bs_port doesn't have interface name or error.
 *  otherwise:	interface name.
 */
const char *bs_port_id_to_iface(enum bs_port_id bs_port)
{
	int vport;

	if (bs_port < 0 || bs_port >= ARRAY_SIZE(bsport_to_vport))
		return NULL;

	vport = bsport_to_vport[bs_port];
	if (vport == WAN5GR_PORT && !is_aqr_phy_exist())
		return NULL;

	return vport_to_iface_name(vport);
}
#endif

void set_jumbo_frame(void)
{
	unsigned int m, vport, mtu = 1500;
	const char *p;
	char ifname[IFNAMSIZ], mtu_iface[sizeof("9000XXX")], mtu_frame[sizeof("9000XX")];
	char *ifconfig_argv[] = { "ifconfig", ifname, "mtu", mtu_iface, NULL };

	if (!nvram_contains_word("rc_support", "switchctrl"))
		return;

	if (nvram_get_int("jumbo_frame_enable"))
		mtu = 9000;

	snprintf(mtu_iface, sizeof(mtu_iface), "%d", mtu);
	snprintf(mtu_frame, sizeof(mtu_frame), "%d", mtu + 18);

	m = get_lan_port_mask();
	for (vport = 0; m > 0 && vport < MAX_WANLAN_PORT ; vport++, m >>= 1) {
		if (!(m & 1) || !(p = vport_to_iface_name(vport)))
			continue;
		strlcpy(ifname, p, sizeof(ifname));
		_eval(ifconfig_argv, NULL, 0, NULL);
	}
}

/* Platform-specific function of wgn_sysdep_swtich_unset()
 * Unconfigure VLAN settings that is used to connect AiMesh guest network.
 * @vid:	VLAN ID
 */
void __wgn_sysdep_swtich_unset(int vid)
{

}

/* Platform-specific function of wgn_sysdep_swtich_set()
 * Unconfigure VLAN settings that is used to connect AiMesh guest network.
 * @vid:	VLAN ID
 */
void __wgn_sysdep_swtich_set(int vid)
{

}

