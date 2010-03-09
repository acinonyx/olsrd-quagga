/*
 * OLSRd Quagga plugin
 *
 * Copyright (C) 2006-2008 Immo 'FaUl' Wehrenberg <immo@chaostreff-dortmund.de>
 * Copyright (C) 2007-2010 Vasilis Tsiligiannis <acinonyxs@yahoo.gr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation or - at your option - under
 * the terms of the GNU General Public Licence version 2 but can be
 * linked to any BSD-Licenced Software with public available sourcecode
 *
 */

/* -------------------------------------------------------------------------
 * File               : plugin.h
 * Description        : header file for plugin.c
 * ------------------------------------------------------------------------- */

int zplugin_redistribute(unsigned char);
void zplugin_localpref(void);
void zplugin_distance(unsigned char);
void zplugin_exportroutes(unsigned char);
void zplugin_sockpath(char *);
void zplugin_port(unsigned int);
void zplugin_version(char);

/*
 * Local Variables:
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */
