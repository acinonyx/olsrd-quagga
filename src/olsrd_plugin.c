/*
 * Copyright (c) 2006, Immo 'FaUl' Wehrenberg <immo@chaostreff-dortmund.de>
 *
 * this code is covered by GPLv2
 *
 */


#include <stdio.h>
#include <string.h>

#include "olsrd_plugin.h"
#include "olsr.h"
#include "scheduler.h"
#include "defs.h"
#include "quagga.h"
#include "kernel_routes.h"

#define PLUGIN_NAME    "OLSRD quagga plugin"
#define PLUGIN_VERSION "0.2.1"
#define PLUGIN_AUTHOR  "Immo 'FaUl' Wehrenberg"
#define MOD_DESC PLUGIN_NAME " " PLUGIN_VERSION " by " PLUGIN_AUTHOR

static void __attribute__ ((constructor)) my_init(void);
static void __attribute__ ((destructor)) my_fini(void);
static void redist_hna (void);


int olsrd_plugin_interface_version() {
  return OLSRD_PLUGIN_INTERFACE_VERSION;
}


int olsrd_plugin_register_param(char *key, char *value) {
  const char *zebra_route_types[] = {"system","kernel","connect","static",
			      "rip","ripng","ospf","ospf6","isis",
 			      "bgp","hsls", NULL};
  unsigned char i = 0;

  if(!strcmp(key, "redistribute")) {
    for (i = 0; zebra_route_types[i]; i++)
      if (!strcmp(value, zebra_route_types[i])) {
	zebra_redistribute(i);
	return 1;
      }
  }
  else if(!strcmp(key, "ExportRoutes")) {
    if (!strcmp(value, "only")) {
      if (!olsr_addroute_remove_function(&olsr_ioctl_add_route, AF_INET))
	puts ("AIII, could not remove the kernel route exporter");
      if (!olsr_delroute_remove_function(&olsr_ioctl_del_route, AF_INET))
	puts ("AIII, could not remove the kernel route deleter");
      olsr_addroute_add_function(&zebra_add_olsr_v4_route, AF_INET);
      olsr_delroute_add_function(&zebra_del_olsr_v4_route, AF_INET);
      return 1;
    }
    else if (!strcmp(value, "additional")) {
      olsr_addroute_add_function(&zebra_add_olsr_v4_route, AF_INET);
      olsr_delroute_add_function(&zebra_del_olsr_v4_route, AF_INET);
      return 1;
    }
  }
  return -1;
}


int olsrd_plugin_init() {
  if(olsr_cnf->ip_version != AF_INET) {
    fputs("see the source - ipv4 so far not supportet\n" ,stderr);
    return 1;
  }

  //  olsr_register_timeout_function(&olsr_timeout);
  olsr_register_scheduler_event(&zebra_check, NULL, 1, 0, NULL);
  return 0;
}

static void my_init(void) {
  init_zebra();
}

static void my_fini(void) {
}

