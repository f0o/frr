/*
 * PIM for Quagga
 * Copyright (C) 2008  Everton da Silva Marques
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "log.h"
#include "privs.h"
#include "version.h"
#include <getopt.h>
#include "command.h"
#include "thread.h"
#include <signal.h>

#include "memory.h"
#include "vrf.h"
#include "filter.h"
#include "vty.h"
#include "sigevent.h"
#include "version.h"
#include "prefix.h"
#include "plist.h"
#include "vrf.h"
#include "libfrr.h"
#include "routemap.h"
#include "routing_nb.h"

#include "pimd.h"
#include "pim_instance.h"
#include "pim_signals.h"
#include "pim_zebra.h"
#include "pim_msdp.h"
#include "pim_iface.h"
#include "pim_bfd.h"
#include "pim_mlag.h"
#include "pim_errors.h"
#include "pim_nb.h"

#define FUZZING 1
#ifdef FUZZING
#include "fuzz.h"
#include "pim_pim.h"
#include "pim_mroute.h"
#endif

extern struct host host;

struct option longopts[] = {{0}};

/* pimd privileges */
zebra_capabilities_t _caps_p[] = {
	ZCAP_NET_ADMIN, ZCAP_SYS_ADMIN, ZCAP_NET_RAW, ZCAP_BIND,
};

/* pimd privileges to run with */
struct zebra_privs_t pimd_privs = {
#if defined(FRR_USER) && defined(FRR_GROUP)
	.user = FRR_USER,
	.group = FRR_GROUP,
#endif
#ifdef VTY_GROUP
	.vty_group = VTY_GROUP,
#endif
	.caps_p = _caps_p,
	.cap_num_p = array_size(_caps_p),
	.cap_num_i = 0};

static const struct frr_yang_module_info *const pimd_yang_modules[] = {
	&frr_filter_info,
	&frr_interface_info,
	&frr_route_map_info,
	&frr_vrf_info,
	&frr_routing_info,
	&frr_pim_info,
	&frr_pim_rp_info,
	&frr_igmp_info,
};

FRR_DAEMON_INFO(pimd, PIM, .vty_port = PIMD_VTY_PORT,

		.proghelp = "Implementation of the PIM routing protocol.",

		.signals = pimd_signals,
		.n_signals = 4 /* XXX array_size(pimd_signals) XXX*/,

		.privs = &pimd_privs, .yang_modules = pimd_yang_modules,
		.n_yang_modules = array_size(pimd_yang_modules), )

#ifdef FUZZING

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct interface *FuzzingIfp;
static struct in_addr FuzzingSrc = { .s_addr = 0x0900001b };

static bool FuzzingInit(void)
{
	vrf_configure_backend(VRF_BACKEND_VRF_LITE);

	const char *name[] = {"pimd"};

	frr_preinit(&pimd_di, 1, (char **)name);
	pim_router_init();

	/*
	 * Initializations
	 */
	pim_error_init();
	pim_vrf_init();
	access_list_init();
	prefix_list_init();
	prefix_list_add_hook(pim_prefix_list_update);
	prefix_list_delete_hook(pim_prefix_list_update);

	pim_route_map_init();
	pim_init();

	/*
	 * Initialize zclient "update" and "lookup" sockets
	 */
	if_zapi_callbacks(pim_ifp_create, pim_ifp_up,
			  pim_ifp_down, pim_ifp_destroy);
	pim_zebra_init();
	pim_bfd_init();
	pim_mlag_init();

	/* Create some fake interface */

	/* Source address stuff */
	struct prefix p;
	str2prefix("27.0.0.9/24", &p);

	/* Create system interface */
	FuzzingIfp = if_create_name("fuzziface", VRF_DEFAULT);
	if_set_index(FuzzingIfp, 69);

	connected_add_by_prefix(FuzzingIfp, &p, NULL);

	return true;
}

static struct pim_instance *FuzzingCreatePimInstance(void)
{
	/* Create pim stuff */
	fprintf(stderr, ">>>>>>>>> %p\n", FuzzingIfp);
	struct pim_interface *pim_ifp = pim_if_new(FuzzingIfp, true, true, false, false);
	pim_igmp_sock_add(pim_ifp->igmp_socket_list, FuzzingSrc, FuzzingIfp, false);

	struct pim_instance *pim = vrf_lookup_by_id(VRF_DEFAULT)->info;
	pim_if_create_pimreg(pim);
	pim_hello_options ho = 0;
	pim_neighbor_add(FuzzingIfp, FuzzingSrc, ho, 210, 1, 1, 30, 20, NULL, 0);

	return pim;
}

static bool FuzzingInitialized;

static struct pim_instance *FuzzingPim;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!FuzzingInitialized) {
		FuzzingInit();
		FuzzingInitialized = true;
		FuzzingPim = FuzzingCreatePimInstance();
	}

	struct pim_instance *pim;
#ifdef FUZZING_LIBFUZZER
	pim = FuzzingPim;
#else
	pim = FuzzingPim;
#endif

	int result;

	uint8_t *input = malloc(size);
	memcpy(input, data, size);

#ifdef KERNEL_IFACE
	result = pim_mroute_msg(pim, (const char *) input, size, 69);
#else
	result = pim_pim_packet(FuzzingIfp, input, size);
#endif /* KERNEL_IFACE */

	free(input);

	return result;
}
#endif /* FUZZING */

#ifndef FUZZING_LIBFUZZER

int main(int argc, char **argv, char **envp)
{
	frr_preinit(&pimd_di, argc, argv);

#ifdef FUZZING
	FuzzingInit();
	FuzzingInitialized = true;
	FuzzingPim = FuzzingCreatePimInstance();

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif /* __AFL_HAVE_MANUAL_CONTROL */

	uint8_t *input = NULL;
	int r = frrfuzz_read_input(&input);

	int ret = LLVMFuzzerTestOneInput(input, r);

	free(input);

	return ret;
#endif /* FUZZING */

	frr_opt_add("", longopts, "");

	/* this while just reads the options */
	while (1) {
		int opt;

		opt = frr_getopt(argc, argv, NULL);

		if (opt == EOF)
			break;

		switch (opt) {
		case 0:
			break;
		default:
			frr_help_exit(1);
			break;
		}
	}


	pim_router_init();

	/*
	 * Initializations
	 */
	pim_error_init();
	pim_vrf_init();
	access_list_init();
	prefix_list_init();
	prefix_list_add_hook(pim_prefix_list_update);
	prefix_list_delete_hook(pim_prefix_list_update);

	pim_route_map_init();
	pim_init();


	/*
	 * Initialize zclient "update" and "lookup" sockets
	 */
	if_zapi_callbacks(pim_ifp_create, pim_ifp_up,
			  pim_ifp_down, pim_ifp_destroy);
	pim_zebra_init();
	pim_bfd_init();
	pim_mlag_init();

	hook_register(routing_conf_event,
		      routing_control_plane_protocols_name_validate);

	frr_config_fork();

#ifdef PIM_DEBUG_BYDEFAULT
	zlog_notice("PIM_DEBUG_BYDEFAULT: Enabling all debug commands");
	PIM_DO_DEBUG_PIM_EVENTS;
	PIM_DO_DEBUG_PIM_PACKETS;
	PIM_DO_DEBUG_PIM_TRACE;
	PIM_DO_DEBUG_IGMP_EVENTS;
	PIM_DO_DEBUG_IGMP_PACKETS;
	PIM_DO_DEBUG_IGMP_TRACE;
	PIM_DO_DEBUG_ZEBRA;
#endif

#ifdef PIM_CHECK_RECV_IFINDEX_SANITY
	zlog_notice(
		"PIM_CHECK_RECV_IFINDEX_SANITY: will match sock/recv ifindex");
#ifdef PIM_REPORT_RECV_IFINDEX_MISMATCH
	zlog_notice(
		"PIM_REPORT_RECV_IFINDEX_MISMATCH: will report sock/recv ifindex mismatch");
#endif
#endif

#ifdef PIM_UNEXPECTED_KERNEL_UPCALL
	zlog_notice(
		"PIM_UNEXPECTED_KERNEL_UPCALL: report unexpected kernel upcall");
#endif

	frr_run(router->master);

	/* never reached */
	return 0;
}
#endif /* FUZZING_LIBFUZZER */
