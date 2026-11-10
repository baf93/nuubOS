/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Downloaded cores nuubOS cannot run (GitHub #22: a game set to the
 * buildbot's Flycast closed at once). Shared by nuubos-emud (such a core is
 * not one of the system's emulators) and nuubos-libretroctl (not offered,
 * not installed):
 *
 * - unsupported-cores.conf names the buildbot cores known not to work;
 * - a core whose ELF needs a library the system lacks (desktop GL, GLX,
 *   OpenMP...) cannot even be loaded, whatever its name.
 *
 * Include after SHARE_DIR is defined.
 */

#ifndef NUUBOS_CORE_CHECK_H
#define NUUBOS_CORE_CHECK_H

#include <elf.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNSUPPORTED_CORES SHARE_DIR "/unsupported-cores.conf"

/* unsupported-cores.conf lists the core ("id  # reason" lines). */
static inline bool core_unsupported(const char *core)
{
	char line[256];
	size_t len = strlen(core);
	FILE *fp = fopen(UNSUPPORTED_CORES, "r");
	bool found = false;

	if (!fp)
		return false;
	while (!found && fgets(line, sizeof(line), fp))
		found = !strncmp(line, core, len) &&
			(line[len] == '\0' || strchr(" \t#\r\n", line[len]));
	fclose(fp);
	return found;
}

/* File offset of virtual address addr (a PT_LOAD segment), 0 = none. */
static inline off_t core_elf_offset(const Elf64_Phdr *ph, int count, Elf64_Addr addr)
{
	for (int i = 0; i < count; i++)
		if (ph[i].p_type == PT_LOAD && addr >= ph[i].p_vaddr &&
		    addr < ph[i].p_vaddr + ph[i].p_filesz)
			return (off_t)(ph[i].p_offset + addr - ph[i].p_vaddr);
	return 0;
}

/* Every DT_NEEDED library of the core at path is in /lib or /usr/lib (the
 * dynamic loader's paths on nuubOS). A file that is not a 64-bit ELF
 * cannot be loaded either. */
static inline bool core_libraries_present(const char *path)
{
	Elf64_Ehdr eh;
	Elf64_Phdr *ph = NULL;
	Elf64_Dyn *dyn = NULL;
	size_t ndyn = 0;
	Elf64_Addr strtab = 0;
	off_t stroff = 0;
	bool ok = false;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return false;
	if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) ||
	    memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_phentsize != sizeof(Elf64_Phdr) || !eh.e_phnum || eh.e_phnum > 64)
		goto out;
	ph = calloc(eh.e_phnum, sizeof(*ph));
	if (!ph || pread(fd, ph, eh.e_phnum * sizeof(*ph), (off_t)eh.e_phoff) !=
		   (ssize_t)(eh.e_phnum * sizeof(*ph)))
		goto out;
	for (int i = 0; i < eh.e_phnum && !dyn; i++) {
		if (ph[i].p_type != PT_DYNAMIC || ph[i].p_filesz > 64 * 1024)
			continue;
		ndyn = ph[i].p_filesz / sizeof(Elf64_Dyn);
		dyn = malloc(ph[i].p_filesz + 1);
		if (!dyn || pread(fd, dyn, ph[i].p_filesz, (off_t)ph[i].p_offset) !=
			    (ssize_t)ph[i].p_filesz)
			goto out;
	}
	if (!dyn) {
		ok = true; /* static: nothing to load */
		goto out;
	}
	for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++)
		if (dyn[i].d_tag == DT_STRTAB)
			strtab = dyn[i].d_un.d_ptr;
	if (!strtab || !(stroff = core_elf_offset(ph, eh.e_phnum, strtab)))
		goto out;
	ok = true;
	for (size_t i = 0; ok && i < ndyn && dyn[i].d_tag != DT_NULL; i++) {
		static const char *const dirs[] = { "/lib", "/usr/lib" };
		char name[256];
		char lib[PATH_MAX];
		ssize_t n;
		bool found = false;

		if (dyn[i].d_tag != DT_NEEDED)
			continue;
		n = pread(fd, name, sizeof(name) - 1, stroff + (off_t)dyn[i].d_un.d_val);
		if (n <= 0 || !memchr(name, '\0', (size_t)n) || strchr(name, '/')) {
			ok = false;
			break;
		}
		for (size_t d = 0; d < 2 && !found; d++) {
			snprintf(lib, sizeof(lib), "%s/%s", dirs[d], name);
			found = access(lib, F_OK) == 0;
		}
		ok = found;
	}
out:
	free(dyn);
	free(ph);
	close(fd);
	return ok;
}

/* A downloaded core at path that nuubOS can run. */
static inline bool core_runs(const char *core, const char *path)
{
	return !core_unsupported(core) && core_libraries_present(path);
}

#endif
