#include <archive.h>
#include <archive_entry.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <errno.h>

#include "pkg.h"
#include "pkg_error.h"
#include "pkg_private.h"

#define EXTRACT_ARCHIVE_FLAGS  (ARCHIVE_EXTRACT_OWNER |ARCHIVE_EXTRACT_PERM| \
		ARCHIVE_EXTRACT_TIME  |ARCHIVE_EXTRACT_ACL | \
		ARCHIVE_EXTRACT_FFLAGS|ARCHIVE_EXTRACT_XATTR)

static int
do_extract(struct archive *a, struct archive_entry *ae)
{
	int retcode = EPKG_OK;
	int ret = 0;
	char path[MAXPATHLEN];
	struct stat st;

	do {
		if (archive_read_extract(a, ae, EXTRACT_ARCHIVE_FLAGS) != ARCHIVE_OK) {
			retcode = pkg_error_set(EPKG_FATAL, "%s", archive_error_string(a));
			break;
		}

		/*
		 * if the file is a configuration file and the configuration
		 * file does not already exist on the file system, then
		 * extract it
		 * ex: conf1.cfg.pkgconf:
		 * if conf1.cfg doesn't exists create it based on
		 * conf1.cfg.pkgconf
		 */
		if (is_conf_file(archive_entry_pathname(ae), path)
		    && lstat(path, &st) == ENOENT) {
			archive_entry_set_pathname(ae, path);
			if (archive_read_extract(a, ae, EXTRACT_ARCHIVE_FLAGS) != ARCHIVE_OK) {
				retcode = pkg_error_set(EPKG_FATAL, "%s", archive_error_string(a));
				break;
			}
		}
	} while ((ret = archive_read_next_header(a, &ae)) == ARCHIVE_OK);

	if (ret != ARCHIVE_EOF)
		retcode = pkg_error_set(EPKG_FATAL, "%s", archive_error_string(a));

	return (retcode);
}

int
pkg_add(struct pkgdb *db, const char *path, struct pkg **pkg_p)
{
	struct archive *a;
	struct archive_entry *ae;
	struct pkgdb_it *it;
	struct pkg *p = NULL;
	struct pkg *pkg = NULL;
	struct pkg **deps;
	struct pkg_exec **execs;
	struct pkg_script **scripts;
	struct sbuf *script_cmd;
	bool extract = true;
	char dpath[MAXPATHLEN];
	const char *basedir;
	const char *ext;
	int retcode = EPKG_OK;
	int ret;
	int i;

	if (path == NULL)
		return (ERROR_BAD_ARG("path"));

	/*
	 * Open the package archive file, read all the meta files and set the
	 * current archive_entry to the first non-meta file.
	 * If there is no non-meta files, EPKG_END is returned.
	 */
	ret = pkg_open2(&pkg, &a, &ae, path);
	if (ret == EPKG_END)
		extract = false;
	else if (ret != EPKG_OK) {
		retcode = ret;
		goto cleanup;
	}

	/*
	 * Check if the package is already installed
	 */
	it = pkgdb_query(db, pkg_get(pkg, PKG_ORIGIN), MATCH_EXACT);
	if (it == NULL) {
		retcode = EPKG_FATAL;
		goto cleanup;
	}

	ret = pkgdb_it_next(it, &p, PKG_LOAD_BASIC);
	pkgdb_it_free(it);
	pkg_free(p);

	if (ret == EPKG_OK) {
		retcode = pkg_error_set(EPKG_INSTALLED, "package already installed");
		goto cleanup;
	} else if (ret != EPKG_END) {
		retcode = ret;
		goto cleanup;
	}

	/*
	 * Check for dependencies
	 */
	deps = pkg_deps(pkg);
	basedir = dirname(path);
	if ((ext = strrchr(path, '.')) == NULL) {
		retcode = pkg_error_set(EPKG_FATAL, "%s has no extension", path);
		goto cleanup;
	}
	pkg_resolvdeps(pkg, db);
	for (i = 0; deps[i] != NULL; i++) {
		if (pkg_type(deps[i]) == PKG_NOTFOUND) {
			snprintf(dpath, sizeof(dpath), "%s/%s-%s%s", basedir,
					 pkg_get(deps[i], PKG_NAME), pkg_get(deps[i], PKG_VERSION),
					 ext);

			if (access(dpath, F_OK) == 0) {
				if (pkg_add(db, dpath, NULL) == EPKG_OK) {
					/*
					 * Recheck the deps because the last installed package may
					 * have installed our deps too.
					 * TODO: do not it database here.
					 */
					pkg_resolvdeps(pkg, db);
				} else {
					retcode = pkg_error_set(EPKG_FATAL, "error while "
											"installing %s (dependency): %s",
											dpath,
											pkg_error_string());
					goto cleanup;
				}
			} else {
				retcode = pkg_error_set(EPKG_DEPENDENCY, "missing %s-%s dependency",
										pkg_get(deps[i], PKG_NAME),
										pkg_get(deps[i], PKG_VERSION));
				goto cleanup;
			}

		}
	}

	/*
	 * Execute pre-install scripts
	 */
	script_cmd = sbuf_new_auto();

	if ((scripts = pkg_scripts(pkg)) != NULL)
		for (i= 0; scripts[i] != NULL; i++) {
			switch (pkg_script_type(scripts[i])) {
				case PKG_SCRIPT_INSTALL:
					sbuf_reset(script_cmd);
					sbuf_printf(script_cmd, "set -- %s-%s INSTALL\n%s", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION), pkg_script_data(scripts[i]));
					sbuf_finish(script_cmd);
					system(sbuf_data(script_cmd));
					break;
				case PKG_SCRIPT_PRE_INSTALL:
					sbuf_reset(script_cmd);
					sbuf_printf(script_cmd, "set -- %s-%s\n%s", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION), pkg_script_data(scripts[i]));
					sbuf_finish(script_cmd);
					system(sbuf_data(script_cmd));
					break;
				default:
					/* just ignore */
					break;
			}
		}

	/*
	 * Extract the files on disk.
	 */
	if (extract == true && (retcode = do_extract(a, ae)) != EPKG_OK)
		goto cleanup;

	/*
	 * Execute post install scripts
	 */
	if (scripts != NULL)
		for (i= 0; scripts[i] != NULL; i++) {
			switch (pkg_script_type(scripts[i])) {
				case PKG_SCRIPT_INSTALL:
					sbuf_reset(script_cmd);
					sbuf_printf(script_cmd, "set -- %s-%s POST-INSTALL\n%s", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION), pkg_script_data(scripts[i]));
					sbuf_finish(script_cmd);
					system(sbuf_data(script_cmd));
					break;
				case PKG_SCRIPT_POST_INSTALL:
					sbuf_reset(script_cmd);
					sbuf_printf(script_cmd, "set -- %s-%s\n%s", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION), pkg_script_data(scripts[i]));
					sbuf_finish(script_cmd);
					system(sbuf_data(script_cmd));
					break;
				default:
					/* just ignore */
					break;
			}
		}

	sbuf_free(script_cmd);

	/*
	 * Execute @exec
	 */
	if ((execs = pkg_execs(pkg)) != NULL)
		for (i = 0; execs[i] != NULL; i++)
			if (pkg_exec_type(execs[i]) == PKG_EXEC)
				system(pkg_exec_cmd(execs[i]));

	cleanup:

	if (a != NULL)
		archive_read_finish(a);

	if (retcode == EPKG_OK)
		retcode = pkgdb_register_pkg(db, pkg);

	if (pkg_p != NULL)
		*pkg_p = (retcode == EPKG_OK) ? pkg : NULL;
	else
		pkg_free(pkg);

	return (retcode);
}
