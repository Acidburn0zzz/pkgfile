/*
 * Copyright (C) 2011 by Dave Reisner <dreisner@archlinux.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <alpm.h>

#include "nosr.h"
#include "update.h"
#include "util.h"

static alpm_handle_t *alpm;
static int interactive;

static struct repo_t *repo_new(const char *reponame)
{
	struct repo_t *repo;

	CALLOC(repo, 1, sizeof(struct repo_t), return NULL);

	if(asprintf(&repo->name, "%s", reponame) == -1) {
		fprintf(stderr, "error: failed to allocate memory\n");
		free(repo);
		return NULL;
	}

	return repo;
}

void repo_free(struct repo_t *repo)
{
	size_t i;

	free(repo->name);
	for(i = 0; i < repo->servercount; i++) {
		free(repo->servers[i]);
	}
	free(repo->servers);

	free(repo);
}

static int repo_add_server(struct repo_t *repo, const char *server)
{
	if(!repo) {
		return 1;
	}

	repo->servers = realloc(repo->servers,
			sizeof(char *) * (repo->servercount + 1));

	repo->servers[repo->servercount] = strdup(server);
	repo->servercount++;

	return 0;
}

static void alpm_progress_cb(const char *filename, off_t xfer, off_t total)
{
	double size, perc = 100 * ((double)xfer / total);
	const char *label;

	size = humanize_size(total, 'K', &label);

	printf("  %-40s %7.2f %3s [%6.2f%%]\r", filename, size, label, perc);
	fflush(stdout);
}

static char *prepare_url(const char *url, const char *repo, const char *arch,
		const char *suffix)
{
	char *string, *temp = NULL;
	const char * const archvar = "$arch";
	const char * const repovar = "$repo";

	string = strdup(url);
	temp = string;
	if(strstr(temp, archvar)) {
		string = strreplace(temp, archvar, arch);
		free(temp);
		temp = string;
	}

	if(strstr(temp, repovar)) {
		string = strreplace(temp, repovar, repo);
		free(temp);
		temp = string;
	}

	if(asprintf(&temp, "%s/%s%s", string, repo, suffix) == -1) {
		fprintf(stderr, "error: failed to allocate memory\n");
	}

	free(string);

	return temp;
}

static char *line_get_val(char *line, const char *sep)
{
	strsep(&line, sep);
	strtrim(line);
	return line;
}

static int add_servers_from_include(struct repo_t *repo, char *file)
{
	char *ptr;
	char line[4096];
	const char * const server = "Server";
	FILE *fp;

	fp = fopen(file, "r");
	if(!fp) {
		perror("fopen");
		return 1;
	}

	while(fgets(line, 4096, fp)) {
		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}
		if(*strtrim(line) == '\0') {
			continue;
		}

		if(strncmp(line, server, strlen(server)) == 0) {
			ptr = line_get_val(line, "=");
			repo_add_server(repo, ptr);
		}
	}

	fclose(fp);

	return 0;
}

struct repo_t **find_active_repos(const char *filename, int *repocount)
{
	FILE *fp;
	char *ptr, *section = NULL;
	char line[4096];
	const char * const server = "Server";
	const char * const include = "Include";
	struct repo_t **active_repos = NULL;
	int in_options = 0;

	*repocount = 0;

	fp = fopen(filename, "r");
	if(!fp) {
		fprintf(stderr, "error: failed to open %s: %s\n", filename, strerror(errno));
		return NULL;
	}

	while(fgets(line, 4096, fp)) {
		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}
		if(*strtrim(line) == '\0') {
			continue;
		}

		if (line[0] == '[' && line[strlen(line) - 1] == ']') {
			free(section);
			section = strndup(&line[1], strlen(line) - 2);
			if(strcmp(section, "options") == 0) {
				in_options = 1;
				continue;
			} else {
				in_options = 0;
				active_repos = realloc(active_repos, sizeof(struct repo_t *) * (*repocount + 1));
				active_repos[*repocount] = repo_new(section);
				(*repocount)++;
			}
		}

		if(in_options) {
			continue;
		}

		if(strchr(line, '=')) {
			char *key = line, *val = line_get_val(line, "=");
			strtrim(key);

			if(strcmp(key, server) == 0) {
				repo_add_server(active_repos[*repocount - 1], val);
			} else if(strcmp(key, include) == 0) {
				add_servers_from_include(active_repos[*repocount - 1], val);
			}
		}
	}

	free(section);
	fclose(fp);

	return active_repos;
}

static int unlink_files_dbfile(const char *dbname)
{
	char b[PATH_MAX];

	snprintf(b, sizeof(b), CACHEPATH "/%s.files", dbname);

	return unlink(b);
}

static int download_repo_files(struct repo_t *repo)
{
	char *ret, *url;
	size_t i;
	struct utsname un;

	uname(&un);

	for(i = 0; i < repo->servercount; i++) {
		url = prepare_url(repo->servers[i], repo->name, un.machine, ".files");

		if(!interactive) {
			printf("downloading %s.files...", repo->name);
			fflush(stdout);
		}

		unlink_files_dbfile(repo->name);
		ret = alpm_fetch_pkgurl(alpm, url);
		if(!ret) {
			fprintf(stderr, "warning: failed to download: %s\n", url);
		}
		free(url);
		if(ret) {
			putchar(10);
			return 0;
		}
	}

	return 1;
}

static int decompress_repo_file(struct repo_t *repo)
{
	char infilename[PATH_MAX], outfilename[PATH_MAX];
	int ret = -1;
	struct archive *a_in, *a_out;
	struct archive_entry *ae;

	/* generally, repo files are gzip compressed, but there's no guarantee of
	 * this. in order to be compression-agnostic, re-use libarchive's
	 * reader/writer methods. this also gives us an opportunity to rewrite
	 * the archive as CPIO, which is marginally faster given our staunch
	 * sequential access. */

	snprintf(infilename, PATH_MAX, CACHEPATH "/%s.files", repo->name);
	snprintf(outfilename, PATH_MAX, CACHEPATH "/%s.files~", repo->name);

	a_in = archive_read_new();
	a_out = archive_write_new();

	if(a_in == NULL || a_out == NULL) {
		fprintf(stderr, "failed to allocate memory for archive objects\n");
		return -1;
	}

	archive_read_support_format_tar(a_in);
	archive_read_support_compression_all(a_in);
	ret = archive_read_open_filename(a_in, infilename, BUFSIZ);
	if (ret != ARCHIVE_OK) {
		fprintf(stderr, "failed to open file for writing: %s: %s\n",
				outfilename, archive_error_string(a_in));
		goto done;
	}

	archive_write_set_format_cpio(a_out);
	archive_write_add_filter_none(a_out);
	ret = archive_write_open_filename(a_out, outfilename);
	if (ret != ARCHIVE_OK) {
		fprintf(stderr, "failed to open file for reading: %s: %s\n",
				infilename, archive_error_string(a_in));
		goto done;
	}

	while(archive_read_next_header(a_in, &ae) == ARCHIVE_OK) {
		unsigned char buf[BUFSIZ];
		int done = 0;
		if(archive_write_header(a_out, ae) != ARCHIVE_OK) {
			fprintf(stderr, "failed to write cpio header: %s\n", archive_error_string(a_out));
			break;
		}
		for(;;) {
			int bytes_r = archive_read_data(a_in, buf, sizeof(buf));
			if(bytes_r == 0) {
				break;
			}

			if(archive_write_data(a_out, buf, bytes_r) != bytes_r) {
				fprintf(stderr, "failed to write %d bytes to new files db: %s\n",
						bytes_r, archive_error_string(a_out));
				done = 1;
				break;
			}
		}
		if(done) {
			break;
		}
	}

	archive_read_close(a_in);
	archive_write_close(a_out);
	ret = 0;

done:
	archive_read_free(a_in);
	archive_write_free(a_out);

	/* success, rotate in the decompressed tarball */
	if(ret == 0 && rename(outfilename, infilename) < 0) {
		fprintf(stderr, "failed to rotate file for repo '%s' into place: %s\n",
				repo->name, strerror(errno));
	}

	return ret;
}

int nosr_update(struct repo_t **repos, int repocount)
{
	int i, r, ret = 0;
	enum _alpm_errno_t err;

	interactive = isatty(STDOUT_FILENO);

	if(access(CACHEPATH, W_OK)) {
		fprintf(stderr, "error: unable to write to %s: ", CACHEPATH);
		perror("");
		return 1;
	}

	alpm = alpm_initialize("/", DBPATH, &err);
	if(!alpm) {
		fprintf(stderr, "error: unable to initialize alpm: %s\n", alpm_strerror(err));
		return 1;
	}

	alpm_option_add_cachedir(alpm, CACHEPATH);

	if(interactive) {
		/* avoid spam if we're not writing to a TTY */
		alpm_option_set_dlcb(alpm, alpm_progress_cb);
	}


	for(i = 0; i < repocount; i++) {
		r = download_repo_files(repos[i]);
		if(r == 0) {
			r = decompress_repo_file(repos[i]);
		}

		if(r != 0) {
			ret = r;
		}
	}

	alpm_release(alpm);

	return ret;
}

/* vim: set ts=2 sw=2 noet: */
