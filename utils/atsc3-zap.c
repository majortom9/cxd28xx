/* SPX-License-Identifier: GPL-2.0-only */
/*
 * atsc3-zap - ATSC 3.0 tuning utility for cxd28xx driver.
 *
 * Tunes a DVB frontend to an ATSC 3.0 channel using DVBv5 ioctls
 * (bypassing dvbv5-zap which cannot handle SYS_ATSC3), brings up
 * the associated atscN network interface on lock, and keeps running
 * until interrupted.
 *
 * Usage: atsc3-zap <freq_hz> [--plp <id>[,<id>...]] [-a <adapter>] [-f <frontend>] [-r]
 *
 * Copyright (c) 2026 Yoonji Park <koreapyj@dcmys.kr>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

/*
 * NOT koreapyj's value (35, his own private compat #define for an
 * out-of-tree driver). This tree added SYS_ATSC3 as a real local enum
 * member in include/uapi/linux/dvb/frontend.h, positioned right after
 * SYS_DCII - which computes to 21, not 35. Since it's a genuine enum
 * member rather than a macro, `#ifndef SYS_ATSC3` can never detect it
 * (the preprocessor has no visibility into enum constants), so
 * koreapyj's original #ifndef guard here would always fire and
 * silently send the wrong wire value to this driver. Removed entirely
 * - the real enum value from <linux/dvb/frontend.h> is used directly.
 */

#ifndef NO_STREAM_ID_FILTER
#define NO_STREAM_ID_FILTER (~0U)
#endif

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

/*
 * The wildcard-PID (0x2000) ALP demux feed that it930x_alp_open()/
 * it930x_alp_stop() create/destroy is tied to this netdev's own
 * ndo_open/ndo_stop (alp.c), NOT to the frontend fd's open/close
 * lifecycle - and that feed goes through the same dvb_usb_start_feed()/
 * feed_count/ADAP_STREAMING accounting as a normal PID feed. If the
 * interface is left up when this tool exits, ADAP_STREAMING never
 * clears, and the next frontend open hangs forever in
 * dvb_usb_fe_sleep()'s wait_on_bit(ADAP_STREAMING) - a real, previously
 * undiagnosed root cause for a hang that looked like it needed a reboot
 * to clear. Always bring the interface down on exit, however we get
 * there, so nobody has to remember to do it by hand.
 */
static int set_alp_iface_up(const char *ifname, int up)
{
	struct ifreq ifr;
	int sock, ret;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

	ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
	if (ret < 0) {
		close(sock);
		return -1;
	}

	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;

	ret = ioctl(sock, SIOCSIFFLAGS, &ifr);
	close(sock);
	return ret;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <freq_hz> [--plp <id>[,<id>...]] [-a <adapter>] [-f <frontend>] [-r]\n"
		"\n"
		"  <freq_hz>       RF frequency in Hz (e.g. 599000000)\n"
		"  --plp <id>[,..] PLP ID(s) 0-63, comma-separated for a bonded\n"
		"                  set (e.g. --plp 0,1); omit for all PLPs\n"
		"  -a <adapter>    DVB adapter number (default 0)\n"
		"  -f <frontend>   Frontend number (default 0)\n"
		"  -r              Record full TS to stdout\n",
		prog);
}

int main(int argc, char **argv)
{
	unsigned int freq = 0, bw = 6000000;
	unsigned int stream_id = NO_STREAM_ID_FILTER;
	int adapter = 0, frontend = 0, record = 0;
	char fe_path[64], alp_ifname[IFNAMSIZ];
	int alp_iface_up = 0;
	int fe_fd, dmx_fd = -1, dvr_fd = -1, i;

	/* Parse arguments */
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	freq = strtoul(argv[1], NULL, 0);
	if (freq == 0) {
		fprintf(stderr, "Invalid frequency: %s\n", argv[1]);
		return 1;
	}

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--plp") == 0 && i + 1 < argc) {
			/*
			 * comma-separated list of up to 4 PLP IDs (e.g.
			 * "0,1" for a bonded pair), packed one per byte into
			 * stream_id - matches the driver's OREGD_PLP_ID_0..3
			 * register layout directly. A bare single ID keeps
			 * working unchanged.
			 */
			char *list = argv[++i];
			char *tok = strtok(list, ",");
			int slot = 0;

			stream_id = 0;
			while (tok && slot < 4) {
				unsigned long id = strtoul(tok, NULL, 0);

				if (id > 63) {
					fprintf(stderr, "PLP ID must be 0-63: %s\n", tok);
					return 1;
				}
				stream_id |= (id & 0xFF) << (slot * 8);
				slot++;
				tok = strtok(NULL, ",");
			}
			if (slot == 0) {
				fprintf(stderr, "No PLP IDs given\n");
				return 1;
			}
			for (; slot < 4; slot++)
				stream_id |= 0xFFUL << (slot * 8);
		} else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
			adapter = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
			frontend = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-r") == 0) {
			record = 1;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	/* Open frontend */
	snprintf(fe_path, sizeof(fe_path),
		 "/dev/dvb/adapter%d/frontend%d", adapter, frontend);
	fe_fd = open(fe_path, O_RDWR);
	if (fe_fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", fe_path, strerror(errno));
		return 1;
	}

	/* Setup signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Clear frontend state */
	{
		struct dtv_property p = { .cmd = DTV_CLEAR };
		struct dtv_properties cmd = { .num = 1, .props = &p };

		if (ioctl(fe_fd, FE_SET_PROPERTY, &cmd) < 0) {
			perror("FE_SET_PROPERTY DTV_CLEAR");
			goto out;
		}
	}

	/* Tune ATSC 3.0 */
	{
		struct dtv_property props[6];
		struct dtv_properties cmd;
		int n = 0;

		memset(props, 0, sizeof(props));

		props[n].cmd = DTV_DELIVERY_SYSTEM;
		props[n].u.data = SYS_ATSC3;
		n++;

		props[n].cmd = DTV_FREQUENCY;
		props[n].u.data = freq;
		n++;

		props[n].cmd = DTV_BANDWIDTH_HZ;
		props[n].u.data = bw;
		n++;

		if (stream_id != NO_STREAM_ID_FILTER) {
			props[n].cmd = DTV_STREAM_ID;
			props[n].u.data = stream_id;
			n++;
		}

		props[n].cmd = DTV_TUNE;
		n++;

		cmd.num = n;
		cmd.props = props;

		if (ioctl(fe_fd, FE_SET_PROPERTY, &cmd) < 0) {
			perror("FE_SET_PROPERTY DTV_TUNE");
			goto out;
		}
	}

	if (stream_id != NO_STREAM_ID_FILTER)
		fprintf(stderr, "Tuning %u Hz, PLP %u ...\n", freq, stream_id);
	else
		fprintf(stderr, "Tuning %u Hz, all PLPs ...\n", freq);

	/* See set_alp_iface_up()'s comment: bringing this up is what
	 * actually starts the wildcard ALP feed, and it must come back
	 * down before we exit or the next frontend open hangs. */
	snprintf(alp_ifname, sizeof(alp_ifname), "alp%d", adapter);
	if (set_alp_iface_up(alp_ifname, 1) < 0)
		fprintf(stderr, "Warning: couldn't bring up %s: %s\n",
			alp_ifname, strerror(errno));
	else
		alp_iface_up = 1;

	/* Set up demux and DVR for recording */
	if (record) {
		char dmx_path[64], dvr_path[64];
		struct dmx_pes_filter_params filter;

		snprintf(dmx_path, sizeof(dmx_path),
			 "/dev/dvb/adapter%d/demux0", adapter);
		snprintf(dvr_path, sizeof(dvr_path),
			 "/dev/dvb/adapter%d/dvr0", adapter);

		dmx_fd = open(dmx_path, O_RDWR);
		if (dmx_fd < 0) {
			fprintf(stderr, "Cannot open %s: %s\n",
				dmx_path, strerror(errno));
			goto out;
		}

		memset(&filter, 0, sizeof(filter));
		filter.pid = 0x2000;
		filter.input = DMX_IN_FRONTEND;
		filter.output = DMX_OUT_TS_TAP;
		filter.pes_type = DMX_PES_OTHER;
		filter.flags = DMX_IMMEDIATE_START;

		if (ioctl(dmx_fd, DMX_SET_PES_FILTER, &filter) < 0) {
			perror("DMX_SET_PES_FILTER");
			goto out;
		}

		dvr_fd = open(dvr_path, O_RDONLY);
		if (dvr_fd < 0) {
			fprintf(stderr, "Cannot open %s: %s\n",
				dvr_path, strerror(errno));
			goto out;
		}
	}

	/* Main loop */
	if (record) {
		unsigned char buf[188 * 348];
		struct pollfd pfd = { .fd = dvr_fd, .events = POLLIN };

		while (running) {
			if (poll(&pfd, 1, 100) > 0) {
				ssize_t n = read(dvr_fd, buf, sizeof(buf));

				if (n > 0) {
					if (write(STDOUT_FILENO, buf, n) != n) {
						perror("write stdout");
						break;
					}
				} else if (n < 0 && errno != EINTR &&
					   errno != EAGAIN) {
					perror("read dvr");
					break;
				}
			}
		}
	} else {
		time_t last_stats = 0;

		while (running) {
			enum fe_status status = 0;

			if (ioctl(fe_fd, FE_READ_STATUS, &status) < 0) {
				perror("FE_READ_STATUS");
				break;
			}

			if (status & FE_HAS_LOCK) {
				time_t now = time(NULL);

				if (now - last_stats >= 5) {
					struct dtv_property props[3];
					struct dtv_properties cmd;

					memset(props, 0, sizeof(props));
					props[0].cmd = DTV_STAT_SIGNAL_STRENGTH;
					props[1].cmd = DTV_STAT_CNR;
					props[2].cmd = DTV_STAT_POST_ERROR_BIT_COUNT;
					cmd.num = 3;
					cmd.props = props;

					if (ioctl(fe_fd, FE_GET_PROPERTY, &cmd) == 0) {
						fprintf(stderr, "Lock | ");
						if (props[0].u.st.len > 0 &&
						    props[0].u.st.stat[0].scale == FE_SCALE_DECIBEL)
							fprintf(stderr, "Sig: %.1f dBm ",
								props[0].u.st.stat[0].svalue / 1000.0);
						if (props[1].u.st.len > 0 &&
						    props[1].u.st.stat[0].scale == FE_SCALE_DECIBEL)
							fprintf(stderr, "SNR: %.1f dB ",
								props[1].u.st.stat[0].svalue / 1000.0);
						if (props[2].u.st.len > 0 &&
						    props[2].u.st.stat[0].scale == FE_SCALE_COUNTER)
							fprintf(stderr, "BER: %llu ",
								props[2].u.st.stat[0].uvalue);
						fprintf(stderr, "\n");
					} else {
						fprintf(stderr, "Locked.\n");
					}
					last_stats = now;
				}
				usleep(500000);
			} else {
				fprintf(stderr, "Lock lost.\n");
				last_stats = 0;
				usleep(100000);
			}
		}
	}

	/* Cleanup */
	fprintf(stderr, "\nStopping.\n");

out:
	/*
	 * Always try this, even on an early error path before the main
	 * loop - alp_iface_up only actually gates whether we succeeded
	 * in bringing it up in the first place, not whether the rest of
	 * tuning succeeded, so this can't accidentally skip the one
	 * cleanup step that matters most for avoiding the next hang.
	 */
	if (alp_iface_up && set_alp_iface_up(alp_ifname, 0) < 0)
		fprintf(stderr, "Warning: couldn't bring down %s: %s\n",
			alp_ifname, strerror(errno));

	if (dvr_fd >= 0)
		close(dvr_fd);
	if (dmx_fd >= 0)
		close(dmx_fd);
	close(fe_fd);
	return 0;
}
