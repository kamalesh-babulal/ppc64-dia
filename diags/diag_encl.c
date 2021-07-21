/*
 * Copyright (C) 2009, 2015, 2016 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <dirent.h>
#include <sys/wait.h>
#include <scsi/scsi.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include "encl_util.h"
#include "diag_encl.h"
#include "platform.h"

#define VPD_PATHNAME_EXTN_MAXLEN	5

static struct option long_options[] = {
	{"cmp_prev",		no_argument,		NULL, 'c'},
	{"disk",                required_argument,      NULL, 'd'},
	{"fake",		required_argument,	NULL, 'f'},
	{"help",		no_argument,		NULL, 'h'},
	{"leds",		no_argument,		NULL, 'l'},
	{"serv_event",		no_argument,		NULL, 's'},
	{"verbose",		no_argument,		NULL, 'v'},
	{"version",		no_argument,		NULL, 'V'},
	{0, 0, 0, 0}
};

int platform;	/* Holds PLATFORM_* */
struct cmd_opts cmd_opts;

static struct {
	char *mtm;
	int (*func)(int, struct dev_vpd *);
} encl_diags[] = {
	{"7031-D24/T24", diag_7031_D24_T24},	/* Pearl enclosure */
	{"5888", diag_bluehawk},		/* Bluehawk enclosure */
	{"EDR1", diag_bluehawk},		/* Bluehawk enclosure */
	{"5887", diag_homerun},			/* Home Run enclosure */
	{"ESLL", diag_slider_lff},		/* Slider enclosure - LFF */
	{"ESLS", diag_slider_sff},		/* Slider enclosure - SFF */
	{NULL, NULL},
};

/**
 * print_usage
 * @brief Print the usage message for this command
 *
 * @param name the name of this executable
 */
static void
print_usage(const char *name) {
	printf("Usage: %s [-h] [-V] [-s [-c][-l]] [-v] [-f <path.pg2>]"
							" [<scsi_enclosure>]\n"
		"\n\t-h: print this help message\n"
		"\t-s: generate serviceable events for any failures and\n"
		"\t      write events to the servicelog\n"
		"\t-c: compare with previous status; report only new failures\n"
		"\t-d: collect disk health information\n"
		"\t-l: turn on fault LEDs for serviceable events\n"
		"\t-v: verbose output\n"
		"\t-V: print the version of the command and exit\n"
		"\t-f: for testing, read SES data from path.pg2 and VPD\n"
		"\t      from path.vpd\n"
		"\t<scsi_enclosure>: the sg device on which to operate, such\n"
		"\t                    as sg7; if not specified, all such\n"
		"\t                    devices will be diagnosed\n", name);
}

/*
 * Given pg2_path = /some/file.pg2, extract the needed VPD values from
 * /some/file.vpd.
 */
static int
read_fake_vpd(const char *sg, const char *pg2_path, struct dev_vpd *vpd)
{
	char *vpd_path, *dot;
	char *result;
	FILE *f;

	vpd_path = strdup(pg2_path);
	assert(vpd_path);
	dot = strrchr(vpd_path, '.');
	assert(dot && !strcmp(dot, ".pg2"));
	strncpy(dot, ".vpd", VPD_PATHNAME_EXTN_MAXLEN - 1);
	dot[VPD_PATHNAME_EXTN_MAXLEN - 1] = '\0';

	f = fopen(vpd_path, "r");
	if (!f) {
		perror(vpd_path);
		free(vpd_path);
		return -1;
	}

	result = fgets_nonl(vpd->mtm, VPD_LENGTH, f);
	if (!result)
		goto missing_vpd;
	result = fgets_nonl(vpd->full_loc, LOCATION_LENGTH, f);
	if (!result)
		goto missing_vpd;
	result = fgets_nonl(vpd->sn, VPD_LENGTH, f);
	if (!result)
		goto missing_vpd;
	result = fgets_nonl(vpd->fru, VPD_LENGTH, f);
	if (!result)
		goto missing_vpd;
	fclose(f);
	free(vpd_path);

	/* Add sg device name */
	strncpy(vpd->dev, sg, PATH_MAX - 1);

	trim_location_code(vpd);
	return 0;

missing_vpd:
	fprintf(stderr, "%s lacks acceptable mtm, location code, serial number,"
			" and FRU number.\n", vpd_path);
	fclose(f);
	free(vpd_path);
	return -1;
}

#define DIAG_ENCL_PREV_PAGES_DIR "/etc/ppc64-diag/ses_pages/"
static void
make_prev_path(const char *encl_loc)
{
	int path_len = strlen(DIAG_ENCL_PREV_PAGES_DIR) + strlen(encl_loc) + 5;

	free(cmd_opts.prev_path);
	cmd_opts.prev_path = malloc(path_len);
	if (!cmd_opts.prev_path)
		return;

	memset(cmd_opts.prev_path, 0, path_len);
	snprintf(cmd_opts.prev_path, path_len, DIAG_ENCL_PREV_PAGES_DIR);

	path_len -= strlen(DIAG_ENCL_PREV_PAGES_DIR);
	strncat(cmd_opts.prev_path, encl_loc, path_len - 1);

	path_len -= strlen(encl_loc);
	strncat(cmd_opts.prev_path, ".pg2", path_len - 1);
}

static void
free_dev_vpd(struct dev_vpd *vpd)
{
	struct dev_vpd *tmp;

	while (vpd) {
		tmp = vpd;
		vpd = vpd->next;
		free(tmp);
	}
}

/**
 * diagnose
 * @brief Diagnose a specific SCSI generic enclosure
 *
 * @param sg the SCSI generic device to diagnose (e.g. "sg7")
 * @return 0 for no failure, 1 if there is a failure on the enclosure
 */
static int
diagnose(const char *sg, struct dev_vpd **diagnosed)
{
	int rc = 0, fd, found = 0, i;
	struct dev_vpd *vpd = NULL;
	struct dev_vpd *v;

	/* Skip sg device validation in test path */
	if (!cmd_opts.fake_path) {
		rc = valid_enclosure_device(sg);
		if (rc)
			return -1;
	}

	printf("DIAGNOSING %s\n", sg);

	vpd = calloc(1, sizeof(struct dev_vpd));
	if (vpd == NULL) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	if (cmd_opts.fake_path)
		rc = read_fake_vpd(sg, cmd_opts.fake_path, vpd);
	else
		rc = read_vpd_from_lscfg(vpd, sg);

	if (vpd->mtm[0] == '\0') {
		fprintf(stderr, "Unable to find machine type/model for %s\n",
				sg);
		goto error_out;
	}
	if (cmd_opts.serv_event && vpd->location[0] == '\0') {
		fprintf(stderr, "Unable to find location code for %s; needed "
				"for -s\n", sg);
		goto error_out;
	}
	if (rc != 0)
		fprintf(stderr, "Warning: unable to find all relevant VPD for "
				"%s\n", sg);

	printf("\tModel    : %s\n\tLocation : %s\n\n", vpd->mtm, vpd->full_loc);

	for (i = 0; encl_diags[i].mtm; i++) {
		if (!strcmp(vpd->mtm, encl_diags[i].mtm)) {
			for (v = *diagnosed; v; v = v->next) {
				if (!strcmp(v->location, vpd->location)) {
					printf("\t(Enclosure already diagnosed)\n\n");
					free(vpd);
					return 0;
				}
			}

			/* fake path ? */
			if (cmd_opts.fake_path)
				fd = -1;
			else {
				/* Skip diagnostics if the enclosure is
				 * temporarily disabled for maintenance.
				 */
				if (enclosure_maint_mode(sg))
					goto error_out;

				/* Open sg device */
				fd = open_sg_device(sg);
				if (fd < 0)
					goto error_out;
			}

			/* diagnose */
			if (cmd_opts.serv_event)
				make_prev_path(vpd->location);
			found = 1;
			rc += encl_diags[i].func(fd, vpd);

			if (fd != -1)
				close(fd);
			break;
		}
	}

	if (found) {
		vpd->next = *diagnosed;
		*diagnosed = vpd;
	} else {
		fprintf(stderr, "\tSCSI enclosure diagnostics not supported "
				"for this model.\n");
		free(vpd);
	}
	return rc;

error_out:
	free(vpd);
	return 1;
}

int
main(int argc, char *argv[])
{
	int failure = 0, option_index, rc;
	char path[PATH_MAX];
	DIR *edir, *sdir;
	struct dirent *sdirent, *edirent;
	struct dev_vpd *diagnosed = NULL;

	platform = get_platform();
	memset(&cmd_opts, 0, sizeof(cmd_opts));

	for (;;) {
		option_index = 0;
		rc = getopt_long(argc, argv, "cd::f:hlsvV", long_options,
				 &option_index);

		if (rc == -1)
			break;

		switch (rc) {
		case 'c':
			cmd_opts.cmp_prev = 1;
			break;
		case 'd':
			cmd_opts.disk_health = 1;
			cmd_opts.disk_name = optarg;
			break;
		case 'f':
			if (cmd_opts.fake_path) {
				fprintf(stderr, "Multiple -f options not "
						"supported.\n");
				return -1;
			}
			cmd_opts.fake_path = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		case 'l':
			cmd_opts.leds = 1;
			break;
		case 's':
			cmd_opts.serv_event = 1;
			break;
		case 'v':
			cmd_opts.verbose = 1;
			break;
		case 'V':
			printf("%s %s\n", argv[0], VERSION);
			return 0;
		case '?':
			print_usage(argv[0]);
			return -1;
		default:
			/* Shouldn't get here. */
			fprintf(stderr, "huh?\n");
			print_usage(argv[0]);
			return -1;
		}
	}

	if (cmd_opts.disk_health) {
		if (cmd_opts.cmp_prev || cmd_opts.fake_path || cmd_opts.leds
			|| cmd_opts.serv_event ||  cmd_opts.verbose) {
			fprintf(stderr, "-d option is exclusive to "
					"all other options\n");
			return -1;
		}
		return diag_disk(cmd_opts.disk_name);
	}

	if (cmd_opts.cmp_prev && !cmd_opts.serv_event) {
		fprintf(stderr, "No -c option without -s\n");
		return -1;
	}

	if (cmd_opts.leds && !cmd_opts.serv_event) {
		fprintf(stderr, "No -l option without -s\n");
		return -1;
	}

	if ((cmd_opts.serv_event || cmd_opts.leds) && geteuid() != 0) {
		fprintf(stderr, "-s and -l options require superuser "
				"privileges\n");
		return -1;
	}

	if (cmd_opts.fake_path) {
		const char *dot = strrchr(cmd_opts.fake_path, '.');
		if (!dot || strcmp(dot, ".pg2") != 0) {
			fprintf(stderr, "Name of file with fake diagnostic "
					"data must end in '.pg2'.\n");
			return -1;
		}
		if (optind + 1 != argc) {
			fprintf(stderr, "Please specify an sg device with the "
					"-f pathname. It need not be an "
					"enclosure.\n");
			return -1;
		}
		failure += diagnose(argv[optind++], &diagnosed);
	} else if (optind < argc) {
		while (optind < argc)
			failure += diagnose(argv[optind++], &diagnosed);

	} else {
		edir = opendir(SCSI_SES_PATH);
		if (!edir) {
			fprintf(stderr,
				"System does not have SCSI enclosure(s).\n");
			return -1;
		}

		/* loop over all enclosures */
		while ((edirent = readdir(edir)) != NULL) {
			if (!strcmp(edirent->d_name, ".") ||
			    !strcmp(edirent->d_name, ".."))
				continue;

			snprintf(path, PATH_MAX, "%s/%s/device/scsi_generic",
				 SCSI_SES_PATH, edirent->d_name);

			sdir = opendir(path);
			if (!sdir)
				continue;

			while ((sdirent = readdir(sdir)) != NULL) {
				if (!strcmp(sdirent->d_name, ".") ||
				    !strcmp(sdirent->d_name, ".."))
					continue;

				/* run diagnostics */
				failure += diagnose(sdirent->d_name,
						    &diagnosed);
			}
			closedir(sdir);
		} /* outer while loop */
		closedir(edir);
	}

	free(cmd_opts.prev_path);
	free_dev_vpd(diagnosed);

	return failure;
}
