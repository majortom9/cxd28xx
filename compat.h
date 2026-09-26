/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compat shims for building on older kernels (e.g. Debian 12 / kernel 6.1).
 * DVB-S2X enums (FEC, modulation, rolloff) were added in kernel 6.2.
 * The i2c .probe_new → .probe rename happened in kernel 6.3.
 */
#ifndef _CXD28XX_COMPAT_H_
#define _CXD28XX_COMPAT_H_

#include <linux/version.h>

/*
 * 2026-09-25: removed the "#ifndef SYS_ATSC3 / #define SYS_ATSC3 35" shim
 * that used to live here. #ifndef only detects preprocessor macros - it
 * has no visibility into a C enum member, and the real SYS_ATSC3 is an
 * enum value (added locally to <linux/dvb/frontend.h> for this project's
 * ATSC3 kernel patch, not a #define). That meant the #ifndef was *always*
 * true and this shim *always* fired, silently overriding the real,
 * correct value (21) with the wrong one (35) even when building against
 * this project's own patched kernel, which always has it. Confirmed live:
 * dvb_core's dvbv5_set_delivery_system() rejected every ATSC3 tune
 * attempt with "Delivery system 21 not supported" - runtime dump of the
 * frontend's own delsys[] array showed 35 sitting where SYS_ATSC3 (21)
 * belonged, byte-for-byte matching this shim's wrong value. This project
 * always builds against a kernel where the real enum value exists, so
 * there is nothing left for this shim to compat-shim.
 */

#endif /* _CXD28XX_COMPAT_H_ */
