#include "config.h"

#include <endian.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <gelf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "common.h"
#include "proc.h"
#include "library.h"

#ifdef PLT_REINITALISATION_BP
extern char *PLTs_initialized_by_here;
#endif

#ifndef DT_PPC_GOT
# define DT_PPC_GOT		(DT_LOPROC + 0)
#endif


#ifndef ARCH_HAVE_LTELF_DATA
int
arch_elf_init(struct ltelf *lte)
{
	return 0;
}

void
arch_elf_destroy(struct ltelf *lte)
{
}
#endif

int
default_elf_add_plt_entry(struct Process *proc, struct ltelf *lte,
			  const char *a_name, GElf_Rela *rela, size_t ndx,
			  struct library_symbol **ret)
{
	char *name = strdup(a_name);
	if (name == NULL) {
	fail:
		free(name);
		return -1;
	}

	enum toplt pltt = PLTS_ARE_EXECUTABLE(lte)
		?  LS_TOPLT_EXEC : LS_TOPLT_POINT;
	GElf_Addr addr = arch_plt_sym_val(lte, ndx, rela);

	struct library_symbol *libsym = malloc(sizeof(*libsym));
	if (libsym == NULL)
		goto fail;

	target_address_t taddr = (target_address_t)(addr + lte->bias);

	/* The logic behind this conditional translation is as
	 * follows.  PLT entries do not typically need custom TOC
	 * pointer, and therefore aren't redirected via OPD.  POINT
	 * PLT, on the other hand, most likely contains addresses of
	 * target functions, not PLT entries themselves, and would
	 * need the OPD redirection.  */
	if (pltt == LS_TOPLT_POINT
	    && arch_translate_address(proc, taddr, &taddr) < 0) {
		free(libsym);
		goto fail;
	}

	library_symbol_init(libsym, taddr, name, 1, pltt);
	*ret = libsym;
	return 0;
}

#ifndef ARCH_HAVE_ADD_PLT_ENTRY
enum plt_status
arch_elf_add_plt_entry(struct Process *proc, struct ltelf *lte,
		       const char *a_name, GElf_Rela *rela, size_t ndx,
		       struct library_symbol **ret)
{
	return plt_default;
}
#endif

Elf_Data *
elf_loaddata(Elf_Scn *scn, GElf_Shdr *shdr)
{
	Elf_Data *data = elf_getdata(scn, NULL);
	if (data == NULL || elf_getdata(scn, data) != NULL
	    || data->d_off || data->d_size != shdr->sh_size)
		return NULL;
	return data;
}

static int
elf_get_section_if(struct ltelf *lte, Elf_Scn **tgt_sec, GElf_Shdr *tgt_shdr,
		   int (*predicate)(Elf_Scn *, GElf_Shdr *, void *data),
		   void *data)
{
	int i;
	for (i = 1; i < lte->ehdr.e_shnum; ++i) {
		Elf_Scn *scn;
		GElf_Shdr shdr;

		scn = elf_getscn(lte->elf, i);
		if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL) {
			debug(1, "Couldn't read section or header.");
			return -1;
		}
		if (predicate(scn, &shdr, data)) {
			*tgt_sec = scn;
			*tgt_shdr = shdr;
			return 0;
		}
	}
	return -1;

}

static int
inside_p(Elf_Scn *scn, GElf_Shdr *shdr, void *data)
{
	GElf_Addr addr = *(GElf_Addr *)data;
	return addr >= shdr->sh_addr
		&& addr < shdr->sh_addr + shdr->sh_size;
}

int
elf_get_section_covering(struct ltelf *lte, GElf_Addr addr,
			 Elf_Scn **tgt_sec, GElf_Shdr *tgt_shdr)
{
	return elf_get_section_if(lte, tgt_sec, tgt_shdr,
				  &inside_p, &addr);
}

static int
type_p(Elf_Scn *scn, GElf_Shdr *shdr, void *data)
{
	GElf_Word type = *(GElf_Word *)data;
	return shdr->sh_type == type;
}

int
elf_get_section_type(struct ltelf *lte, GElf_Word type,
		     Elf_Scn **tgt_sec, GElf_Shdr *tgt_shdr)
{
	return elf_get_section_if(lte, tgt_sec, tgt_shdr,
				  &type_p, &type);
}

static int
need_data(Elf_Data *data, size_t offset, size_t size)
{
	assert(data != NULL);
	if (data->d_size < size || offset > data->d_size - size) {
		debug(1, "Not enough data to read %zd-byte value"
		      " at offset %zd.", size, offset);
		return -1;
	}
	return 0;
}

#define DEF_READER(NAME, SIZE)						\
	int								\
	NAME(Elf_Data *data, size_t offset, uint##SIZE##_t *retp)	\
	{								\
		if (!need_data(data, offset, SIZE / 8) < 0)		\
			return -1;					\
									\
		if (data->d_buf == NULL) /* NODATA section */ {		\
			*retp = 0;					\
			return 0;					\
		}							\
									\
		union {							\
			uint##SIZE##_t dst;				\
			char buf[0];					\
		} u;							\
		memcpy(u.buf, data->d_buf + offset, sizeof(u.dst));	\
		*retp = u.dst;						\
		return 0;						\
	}

DEF_READER(elf_read_u16, 16)
DEF_READER(elf_read_u32, 32)
DEF_READER(elf_read_u64, 64)

#undef DEF_READER

int
open_elf(struct ltelf *lte, const char *filename)
{
	lte->fd = open(filename, O_RDONLY);
	if (lte->fd == -1)
		return 1;

	elf_version(EV_CURRENT);

#ifdef HAVE_ELF_C_READ_MMAP
	lte->elf = elf_begin(lte->fd, ELF_C_READ_MMAP, NULL);
#else
	lte->elf = elf_begin(lte->fd, ELF_C_READ, NULL);
#endif

	if (lte->elf == NULL || elf_kind(lte->elf) != ELF_K_ELF)
		error(EXIT_FAILURE, 0, "Can't open ELF file \"%s\"", filename);

	if (gelf_getehdr(lte->elf, &lte->ehdr) == NULL)
		error(EXIT_FAILURE, 0, "Can't read ELF header of \"%s\"",
		      filename);

	if (lte->ehdr.e_type != ET_EXEC && lte->ehdr.e_type != ET_DYN)
		error(EXIT_FAILURE, 0,
		      "\"%s\" is not an ELF executable nor shared library",
		      filename);

	if ((lte->ehdr.e_ident[EI_CLASS] != LT_ELFCLASS
	     || lte->ehdr.e_machine != LT_ELF_MACHINE)
#ifdef LT_ELF_MACHINE2
	    && (lte->ehdr.e_ident[EI_CLASS] != LT_ELFCLASS2
		|| lte->ehdr.e_machine != LT_ELF_MACHINE2)
#endif
#ifdef LT_ELF_MACHINE3
	    && (lte->ehdr.e_ident[EI_CLASS] != LT_ELFCLASS3
		|| lte->ehdr.e_machine != LT_ELF_MACHINE3)
#endif
	    )
		error(EXIT_FAILURE, 0,
		      "\"%s\" is ELF from incompatible architecture", filename);

	return 0;
}

static int
do_init_elf(struct ltelf *lte, const char *filename, GElf_Addr bias)
{
	int i;
	GElf_Addr relplt_addr = 0;
	GElf_Addr soname_offset = 0;

	debug(DEBUG_FUNCTION, "do_init_elf(filename=%s)", filename);
	debug(1, "Reading ELF from %s...", filename);

	if (open_elf(lte, filename) < 0)
		return -1;

	/* Find out the base address.  */
	{
		GElf_Phdr phdr;
		for (i = 0; gelf_getphdr (lte->elf, i, &phdr) != NULL; ++i) {
			if (phdr.p_type == PT_LOAD) {
				lte->base_addr = phdr.p_vaddr - bias;
				fprintf(stderr,
					" + vaddr=%#lx, bias=%#lx, base=%#lx\n",
					phdr.p_vaddr, bias, lte->base_addr);
				break;
			}
		}
	}

	if (lte->base_addr == 0) {
		fprintf(stderr, "Couldn't determine base address of %s\n",
			filename);
		return -1;
	}

	lte->bias = bias;
	lte->entry_addr = lte->ehdr.e_entry + lte->bias;

	for (i = 1; i < lte->ehdr.e_shnum; ++i) {
		Elf_Scn *scn;
		GElf_Shdr shdr;
		const char *name;

		scn = elf_getscn(lte->elf, i);
		if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
			error(EXIT_FAILURE, 0,
			      "Couldn't get section header from \"%s\"",
			      filename);

		name = elf_strptr(lte->elf, lte->ehdr.e_shstrndx, shdr.sh_name);
		if (name == NULL)
			error(EXIT_FAILURE, 0,
			      "Couldn't get section header from \"%s\"",
			      filename);

		if (shdr.sh_type == SHT_SYMTAB) {
			Elf_Data *data;

			lte->symtab = elf_getdata(scn, NULL);
			lte->symtab_count = shdr.sh_size / shdr.sh_entsize;
			if ((lte->symtab == NULL
			     || elf_getdata(scn, lte->symtab) != NULL)
			    && opt_x != NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get .symtab data from \"%s\"",
				      filename);

			scn = elf_getscn(lte->elf, shdr.sh_link);
			if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get section header from \"%s\"",
				      filename);

			data = elf_getdata(scn, NULL);
			if (data == NULL || elf_getdata(scn, data) != NULL
			    || shdr.sh_size != data->d_size || data->d_off)
				error(EXIT_FAILURE, 0,
				      "Couldn't get .strtab data from \"%s\"",
				      filename);

			lte->strtab = data->d_buf;
		} else if (shdr.sh_type == SHT_DYNSYM) {
			Elf_Data *data;

			lte->dynsym = elf_getdata(scn, NULL);
			lte->dynsym_count = shdr.sh_size / shdr.sh_entsize;
			if (lte->dynsym == NULL
			    || elf_getdata(scn, lte->dynsym) != NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get .dynsym data from \"%s\"",
				      filename);

			scn = elf_getscn(lte->elf, shdr.sh_link);
			if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get section header from \"%s\"",
				      filename);

			data = elf_getdata(scn, NULL);
			if (data == NULL || elf_getdata(scn, data) != NULL
			    || shdr.sh_size != data->d_size || data->d_off)
				error(EXIT_FAILURE, 0,
				      "Couldn't get .dynstr data from \"%s\"",
				      filename);

			lte->dynstr = data->d_buf;
		} else if (shdr.sh_type == SHT_DYNAMIC) {
			Elf_Data *data;
			size_t j;

			lte->dyn_addr = shdr.sh_addr;
			fprintf(stderr, "dyn_addr = %#lx\n", lte->dyn_addr);
			lte->dyn_sz = shdr.sh_size;

			data = elf_getdata(scn, NULL);
			if (data == NULL || elf_getdata(scn, data) != NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get .dynamic data from \"%s\"",
				      filename);

			for (j = 0; j < shdr.sh_size / shdr.sh_entsize; ++j) {
				GElf_Dyn dyn;

				if (gelf_getdyn(data, j, &dyn) == NULL)
					error(EXIT_FAILURE, 0,
					      "Couldn't get .dynamic data from \"%s\"",
					      filename);
				if (dyn.d_tag == DT_JMPREL)
					relplt_addr = dyn.d_un.d_ptr;
				else if (dyn.d_tag == DT_PLTRELSZ)
					lte->relplt_size = dyn.d_un.d_val;
				else if (dyn.d_tag == DT_SONAME)
					soname_offset = dyn.d_un.d_val;
			}
		} else if (shdr.sh_type == SHT_PROGBITS
			   || shdr.sh_type == SHT_NOBITS) {
			if (strcmp(name, ".plt") == 0) {
				lte->plt_addr = shdr.sh_addr;
				lte->plt_size = shdr.sh_size;
				lte->plt_data = elf_loaddata(scn, &shdr);
				if (lte->plt_data == NULL)
					fprintf(stderr,
						"Can't load .plt data\n");
				if (shdr.sh_flags & SHF_EXECINSTR)
					lte->lte_flags |= LTE_PLT_EXECUTABLE;
			}
#ifdef ARCH_SUPPORTS_OPD
			else if (strcmp(name, ".opd") == 0) {
				lte->opd_addr = (GElf_Addr *) (long) shdr.sh_addr;
				lte->opd_size = shdr.sh_size;
				lte->opd = elf_rawdata(scn, NULL);
			}
#endif
		}
	}

	if (lte->dynsym == NULL || lte->dynstr == NULL)
		error(EXIT_FAILURE, 0,
		      "Couldn't find .dynsym or .dynstr in \"%s\"", filename);

	if (!relplt_addr || !lte->plt_addr) {
		debug(1, "%s has no PLT relocations", filename);
		lte->relplt = NULL;
		lte->relplt_count = 0;
	} else if (lte->relplt_size == 0) {
		debug(1, "%s has unknown PLT size", filename);
		lte->relplt = NULL;
		lte->relplt_count = 0;
	} else {

		for (i = 1; i < lte->ehdr.e_shnum; ++i) {
			Elf_Scn *scn;
			GElf_Shdr shdr;

			scn = elf_getscn(lte->elf, i);
			if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
				error(EXIT_FAILURE, 0,
				      "Couldn't get section header from \"%s\"",
				      filename);
			if (shdr.sh_addr == relplt_addr
			    && shdr.sh_size == lte->relplt_size) {
				lte->relplt = elf_getdata(scn, NULL);
				lte->relplt_count =
				    shdr.sh_size / shdr.sh_entsize;
				if (lte->relplt == NULL
				    || elf_getdata(scn, lte->relplt) != NULL)
					error(EXIT_FAILURE, 0,
					      "Couldn't get .rel*.plt data from \"%s\"",
					      filename);
				break;
			}
		}

		if (i == lte->ehdr.e_shnum)
			error(EXIT_FAILURE, 0,
			      "Couldn't find .rel*.plt section in \"%s\"",
			      filename);

		debug(1, "%s %zd PLT relocations", filename, lte->relplt_count);
	}

	if (soname_offset != 0)
		lte->soname = lte->dynstr + soname_offset;

	if (arch_elf_init(lte) < 0) {
		fprintf(stderr, "Backend initialization failed.\n");
		return -1;
	}

	return 0;
}

/* XXX temporarily non-static */
void
do_close_elf(struct ltelf *lte) {
	debug(DEBUG_FUNCTION, "do_close_elf()");
	arch_elf_destroy(lte);
	elf_end(lte->elf);
	close(lte->fd);
}

struct library *
ltelf_read_library(struct Process *proc, const char *filename, GElf_Addr bias)
{
	 // XXX we leak LTE contents
	struct ltelf lte = {};
	if (do_init_elf(&lte, filename, bias) < 0)
		return NULL;
	proc->e_machine = lte.ehdr.e_machine;

	struct library *lib = malloc(sizeof(*lib));
	char *soname = NULL;
	if (lib == NULL) {
	fail:
		free(soname);
		library_destroy(lib);
		free(lib);
		lib = NULL;
		goto done;
	}

	int own_soname;
	if (lte.soname != NULL) {
		soname = strdup(lte.soname);
		own_soname = 1;
	} else {
		soname = rindex(filename, '/') + 1;
		own_soname = 0;
	}
	if (soname == NULL)
		goto fail;

	target_address_t entry = (target_address_t)lte.entry_addr;
	if (arch_translate_address(proc, entry + lte.bias, &entry) < 0)
		goto fail;

	library_init(lib);
	library_set_soname(lib, soname, own_soname);
	library_set_pathname(lib, filename, 1);
	lib->base = (target_address_t)lte.base_addr;
	lib->entry = entry;
	lib->dyn_addr = (target_address_t)lte.dyn_addr;

	size_t i;
	for (i = 0; i < lte.relplt_count; ++i) {
		GElf_Rel rel;
		GElf_Rela rela;
		GElf_Sym sym;
		void *ret;

		if (lte.relplt->d_type == ELF_T_REL) {
			ret = gelf_getrel(lte.relplt, i, &rel);
			rela.r_offset = rel.r_offset;
			rela.r_info = rel.r_info;
			rela.r_addend = 0;
		} else {
			ret = gelf_getrela(lte.relplt, i, &rela);
		}

		if (ret == NULL
		    || ELF64_R_SYM(rela.r_info) >= lte.dynsym_count
		    || gelf_getsym(lte.dynsym, ELF64_R_SYM(rela.r_info),
				   &sym) == NULL)
			error(EXIT_FAILURE, 0,
			      "Couldn't get relocation from \"%s\"",
			      filename);

		char const *name = lte.dynstr + sym.st_name;
		struct library_symbol *libsym;
		switch (arch_elf_add_plt_entry(proc, &lte, name,
					       &rela, i, &libsym)) {
		case plt_default:
			if (default_elf_add_plt_entry(proc, &lte, name,
						      &rela, i, &libsym) < 0)
		case plt_fail:
				goto fail;
		case plt_ok:
			if (libsym != NULL)
				library_add_symbol(lib, libsym);
		}
	}

done:
	do_close_elf(&lte);
	return lib;
}

struct library *
ltelf_read_main_binary(struct Process *proc, const char *path)
{
	fprintf(stderr, "ltelf_read_main_binary %d %s\n", proc->pid, path);
	char *fname = pid2name(proc->pid);
	struct library *lib = ltelf_read_library(proc, fname, 0);
	if (lib != NULL) {
		library_set_pathname(lib, path, 0);
		library_set_soname(lib, rindex(path, '/') + 1, 0);
	}
	return lib;
}
