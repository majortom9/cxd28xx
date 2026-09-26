/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * spectrum-test - minimal FE_GET_SPECTRUM_SCAN smoke test for cxd2878's
 * new get_spectrum_scan() implementation. Requests a handful of
 * frequencies across the frontend's supported range and prints back
 * the dBm level reported at each - no demod lock required.
 *
 * Usage: spectrum-test [-a <adapter>] [-f <frontend>]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>

int main(int argc, char **argv)
{
	int adapter = 0, frontend = 0, i;
	char fe_path[64];
	int fe_fd;
	__u32 freq[9];
	__s32 rf_level[9];
	__u32 type;
	struct dvb_fe_spectrum_scan s;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
			adapter = atoi(argv[++i]);
		else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
			frontend = atoi(argv[++i]);
	}

	snprintf(fe_path, sizeof(fe_path), "/dev/dvb/adapter%d/frontend%d",
		 adapter, frontend);
	fe_fd = open(fe_path, O_RDWR);
	if (fe_fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", fe_path, strerror(errno));
		return 1;
	}

	/* Spread across the CXD2878's declared 45-868 MHz range, including
	 * RF16 (485 MHz) where we know a real ATSC3 signal exists. */
	freq[0] = 60000000;
	freq[1] = 150000000;
	freq[2] = 300000000;
	freq[3] = 485000000;
	freq[4] = 500000000;
	freq[5] = 581000000;
	freq[6] = 650000000;
	freq[7] = 750000000;
	freq[8] = 850000000;

	s.freq = freq;
	s.num_freq = 9;
	s.rf_level = rf_level;
	s.type = &type;

	if (ioctl(fe_fd, FE_GET_SPECTRUM_SCAN, &s) < 0) {
		fprintf(stderr, "FE_GET_SPECTRUM_SCAN failed: %s\n", strerror(errno));
		close(fe_fd);
		return 1;
	}

	printf("units: %s\n", type == SC_DBM ? "dBm" : type == SC_DB ? "dB" : "gain");
	for (i = 0; i < 9; i++)
		printf("  %10u Hz -> %d\n", freq[i], rf_level[i]);

	close(fe_fd);
	return 0;
}
