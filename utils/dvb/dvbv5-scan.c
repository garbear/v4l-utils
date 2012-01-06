/*
 * Copyright (c) 2011 - Mauro Carvalho Chehab <mchehab@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * Or, point your browser to http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * Based on dvbv5-tzap utility.
 */

/*
 * FIXME: It lacks DISEqC support and DVB-CA. Tested only with ISDB-T
 */

#if 0
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE 1
#define _LARGEFILE64_SOURCE 1
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>

#include <linux/dvb/dmx.h>
#include "dvb-file.h"
#include "dvb-demux.h"
#include "libscan.h"

static char DEMUX_DEV[80];
static char DVR_DEV[80];
static int verbose = 0;
#define CHANNEL_FILE "channels.conf"

#define ERROR(x...)                                                     \
	do {                                                            \
		fprintf(stderr, "ERROR: ");                             \
		fprintf(stderr, x);                                     \
		fprintf(stderr, "\n");                                 \
	} while (0)

#define PERROR(x...)                                                    \
	do {                                                            \
		fprintf(stderr, "ERROR: ");                             \
		fprintf(stderr, x);                                     \
		fprintf(stderr, " (%s)\n", strerror(errno));		\
	} while (0)

static int check_frontend(struct dvb_v5_fe_parms *parms, int timeout)
{
	int rc, i;
	fe_status_t status;
	uint32_t snr = 0, _signal = 0;
	uint32_t ber = 0, uncorrected_blocks = 0;

	for (i = 0; i < timeout * 10; i++) {
		rc = dvb_fe_get_stats(parms);
		if (rc < 0)
			PERROR("dvb_fe_get_stats failed");

		rc = dvb_fe_retrieve_stats(parms, DTV_STATUS, &status);
		if (status & FE_HAS_LOCK)
			break;
		usleep(100000);
	};
	dvb_fe_retrieve_stats(parms, DTV_STATUS, &status);
	dvb_fe_retrieve_stats(parms, DTV_BER, &ber);
	dvb_fe_retrieve_stats(parms, DTV_SIGNAL_STRENGTH, &_signal);
	dvb_fe_retrieve_stats(parms, DTV_UNCORRECTED_BLOCKS,
				    &uncorrected_blocks);
	dvb_fe_retrieve_stats(parms, DTV_SNR, &snr);

	printf("status %02x | signal %3u%% | snr %3u%% | ber %d | unc %d ",
		status, (_signal * 100) / 0xffff, (snr * 100) / 0xffff,
		ber, uncorrected_blocks);

	if (status & FE_HAS_LOCK)
		printf("| FE_HAS_LOCK\n");
	else {
		printf("| tune failed\n");
		return -1;
	}

	return 0;
}

static int run_scan(const char *fname, int format,
		    struct dvb_v5_fe_parms *parms)
{
	struct dvb_file *dvb_file = NULL, *dvb_file_new = NULL;
	struct dvb_entry *entry;
	int i, rc, count = 0;
	uint32_t freq, sys;

	switch (format) {
	case 0:
	default:
		dvb_file = read_dvb_file(fname);
		break;
	case 1:			/* DVB channel/transponder old format */
		dvb_file = parse_format_oneline(fname, " \n", SYS_UNDEFINED,
						channel_formats);
		break;
	case 2: 			/* DVB old zap format */
		switch (parms->current_sys) {
		case SYS_DVBT:
		case SYS_DVBS:
		case SYS_DVBC_ANNEX_A:
		case SYS_ATSC:
			sys = parms->current_sys;
			break;
		case SYS_DVBC_ANNEX_C:
			sys = SYS_DVBC_ANNEX_A;
			break;
		case SYS_DVBC_ANNEX_B:
			sys = SYS_ATSC;
			break;
		case SYS_ISDBT:
			sys = SYS_DVBT;
			break;
		default:
			ERROR("Doesn't know how to emulate the delivery system");
			return -1;
		}
		dvb_file = parse_format_oneline(fname, ":", sys, zap_formats);
		break;
	}
	if (!dvb_file)
		return -2;

	for (entry = dvb_file->first_entry; entry != NULL; entry = entry->next) {
		struct dvb_descriptors *dvb_desc = NULL;

		/* First of all, set the delivery system */
		for (i = 0; i < entry->n_props; i++)
			if (entry->props[i].cmd == DTV_DELIVERY_SYSTEM)
				dvb_set_compat_delivery_system(parms,
							       entry->props[i].u.data);

		/* Copy data into parms */
		for (i = 0; i < entry->n_props; i++) {
			uint32_t data = entry->props[i].u.data;

			/* Don't change the delivery system */
			if (entry->props[i].cmd == DTV_DELIVERY_SYSTEM)
				continue;

			dvb_fe_store_parm(parms, entry->props[i].cmd, data);
			if (parms->current_sys == SYS_ISDBT) {
				dvb_fe_store_parm(parms, DTV_ISDBT_PARTIAL_RECEPTION, 0);
				dvb_fe_store_parm(parms, DTV_ISDBT_SOUND_BROADCASTING, 0);
				dvb_fe_store_parm(parms, DTV_ISDBT_LAYER_ENABLED, 0x07);
				if (entry->props[i].cmd == DTV_CODE_RATE_HP) {
					dvb_fe_store_parm(parms, DTV_ISDBT_LAYERA_FEC,
							data);
					dvb_fe_store_parm(parms, DTV_ISDBT_LAYERB_FEC,
							data);
					dvb_fe_store_parm(parms, DTV_ISDBT_LAYERC_FEC,
							data);
				} else if (entry->props[i].cmd == DTV_MODULATION) {
					dvb_fe_store_parm(parms,
							DTV_ISDBT_LAYERA_MODULATION,
							data);
					dvb_fe_store_parm(parms,
							DTV_ISDBT_LAYERB_MODULATION,
							data);
					dvb_fe_store_parm(parms,
							DTV_ISDBT_LAYERC_MODULATION,
							data);
				}
			}
			if (parms->current_sys == SYS_ATSC &&
			    entry->props[i].cmd == DTV_MODULATION) {
				if (data != VSB_8 && data != VSB_16)
					dvb_fe_store_parm(parms,
							DTV_DELIVERY_SYSTEM,
							SYS_DVBC_ANNEX_B);
			}
		}

		rc = dvb_fe_set_parms(parms);
		if (rc < 0) {
			PERROR("dvb_fe_set_parms failed");
			return -1;
		}

		/* As the DVB core emulates it, better to always use auto */
		dvb_fe_store_parm(parms, DTV_INVERSION, INVERSION_AUTO);

		dvb_fe_retrieve_parm(parms, DTV_FREQUENCY, &freq);
		count++;
		printf("Scanning frequency #%d %d\n", count, freq);
		if (verbose)
			dvb_fe_prt_parms(stdout, parms);

		rc = check_frontend(parms, 4);
		if (rc < 0)
			continue;

		dvb_desc = get_dvb_ts_tables(DEMUX_DEV, verbose);
		if (!dvb_desc)
			continue;

		for (i = 0; i < dvb_desc->sdt_table.service_table_len; i++) {
			struct service_table *service_table = &dvb_desc->sdt_table.service_table[i];
			if (service_table->service_name)
				printf("Service #%d: %s\n", i, service_table->service_name);
		}

		store_dvb_channel(&dvb_file_new, parms, dvb_desc, 0);

		free_dvb_ts_tables(dvb_desc);
	}

	write_dvb_file("dvb_channels.conf", dvb_file_new);

	dvb_file_free(dvb_file);
	dvb_file_free(dvb_file_new);
	return 0;
}

static char *usage =
    "usage:\n"
    "       dvbv5-scan [options] <channels_file>\n"
    "        scan DVB services using the channel file\n"
    "     -a number : use given adapter (default 0)\n"
    "     -f number : use given frontend (default 0)\n"
    "     -d number : use given demux (default 0)\n"
    "     -v        : be (very) verbose\n"
    "     -o file   : output filename (use -o - for stdout)\n"
    "     -O        : uses old channel format\n"
    "     -z        : uses zap services file, discarding video/audio pid's\n"
    "     -h -?     : display this help and exit\n";

int main(int argc, char **argv)
{
	char *confname = NULL;
	int adapter = 0, frontend = 0, demux = 0;
	int opt, format = 0;
	struct dvb_v5_fe_parms *parms;

	while ((opt = getopt(argc, argv, "H?ha:f:d:vzO")) != -1) {
		switch (opt) {
		case 'a':
			adapter = strtoul(optarg, NULL, 0);
			break;
		case 'f':
			frontend = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			demux = strtoul(optarg, NULL, 0);
			break;
		case 'O':
			format = 1;
			break;
		case 'z':
			format = 2;
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		case 'h':
		default:
			fprintf(stderr, usage, argv[0]);
			return -1;
		};
	}

	snprintf(DEMUX_DEV, sizeof(DEMUX_DEV),
		 "/dev/dvb/adapter%i/demux%i", adapter, demux);

	snprintf(DVR_DEV, sizeof(DVR_DEV),
		 "/dev/dvb/adapter%i/dvr%i", adapter, demux);

	if (verbose)
		fprintf(stderr, "using demux '%s'\n", DEMUX_DEV);

	if (optind < argc)
		confname = argv[optind];

	if (!confname) {
		fprintf(stderr, usage, argv[0]);
		return -1;
	}

	parms = dvb_fe_open(adapter, frontend, verbose, 0);

	if (run_scan(confname, format, parms))
		return -1;

	dvb_fe_close(parms);
	return 0;
}